#include <stdarg.h>
#include "discworld_sync.h"
#include "../download/download.h"
#include "cJSON.h"
#include "../http/client/mdns_resolve.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DW_MANIFEST_MAX_BYTES (64U * 1024U * 1024U)
#define DW_PLAYLISTS_MAX_BYTES (16U * 1024U * 1024U)
#define DW_RECORD_MAX_BYTES (64U * 1024U * 1024U)
#define DW_FILE_MAX_BYTES (UINT64_C(4) << 30)
#define DW_CONNECT_TIMEOUT_MS 10000U
#define DW_READ_TIMEOUT_MS 60000U
#define DW_API_TOTAL_TIMEOUT_MS 180000U
#define DW_LISTING_WAIT_MS 2000U
#define DW_LISTING_MAX_WAIT_MS (20U * 60U * 1000U)
#define DW_SAVE_EVERY 100
#define DW_MUSIC_FOLDER "Music"
#define DW_AUDIOBOOKS_FOLDER "Audiobooks"

/* ---- small helpers ------------------------------------------------------------------------ */

/* The HTTP client's error code in words for the status line. */
static const char * dw_why(const char * error) {
    if (!error || !error[0]) return "no answer";
    if (!strcmp(error, HTTP_ERR_DNS)) return "nothing on the Wi-Fi has that name";
    if (!strcmp(error, HTTP_ERR_CONNECT)) return "found, but nothing answers on that port";
    if (!strcmp(error, HTTP_ERR_CONNECT_TIMEOUT)) return "no answer in time";
    if (!strcmp(error, HTTP_ERR_TLS)) return "the https connection failed";
    if (!strcmp(error, HTTP_ERR_TIMEOUT)) return "it stopped answering";
    if (!strcmp(error, HTTP_ERR_INVALID_URL)) return "not a web address";
    if (strstr(error, "too_large")) return "answered, but it is not Discworld";
    return error;
}

static void dw_detail(discworld_result_t * r, const char * fmt, const char * arg) {
    snprintf(r->detail, sizeof(r->detail), fmt, arg ? arg : "");
}

/* The host of "scheme://host[:port][/...]", lower-cased as given. */
static bool dw_url_host(const char * url, char * out, size_t size) {
    const char * h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, ":/");
    if (!n || n >= size) return false;
    memcpy(out, h, n);
    out[n] = '\0';
    return true;
}

static bool dw_is_ip(const char * host) {
    struct in_addr a;
    return inet_pton(AF_INET, host, &a) == 1;
}

void discworld_normalize_url(const char * input, char * out, size_t size) {
    if (!out || !size) return;
    out[0] = '\0';
    if (!input) return;
    while (*input == ' ' || *input == '\t') input++;
    size_t len = strlen(input);
    while (len && (input[len - 1] == ' ' || input[len - 1] == '\t' || input[len - 1] == '/')) len--;
    if (!len) return;
    bool has_scheme = (len >= 7 && strncasecmp(input, "http://", 7) == 0) ||
                      (len >= 8 && strncasecmp(input, "https://", 8) == 0);
    snprintf(out, size, "%s%.*s", has_scheme ? "" : "http://", (int) len, input);
}

/* The top folder of a card path the sync may write: Music or Audiobooks
 * (compás's Books reads <card>/Audiobooks). Its length, or 0 for neither. */
static size_t dw_top_folder_len(const char * path) {
    static const char * const tops[] = { DW_MUSIC_FOLDER "/", DW_AUDIOBOOKS_FOLDER "/" };
    for (size_t i = 0; path && i < sizeof(tops) / sizeof(tops[0]); i++)
        if (strncmp(path, tops[i], strlen(tops[i])) == 0) return strlen(tops[i]) - 1;
    return 0;
}

bool discworld_card_path_ok(const char * path) {
    if (!dw_top_folder_len(path)) return false;
    if (strlen(path) >= PATH_MAX - 300) return false;
    for (const unsigned char * c = (const unsigned char *) path; *c; c++)
        if (*c < 32 || *c == '\\' || *c == 127) return false;
    const char * part = path;
    for (;;) {
        const char * slash = strchr(part, '/');
        size_t n = slash ? (size_t) (slash - part) : strlen(part);
        if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!slash) break;
        part = slash + 1;
    }
    return true;
}

static bool dw_url_append_encoded(char * out, size_t size, const char * text, bool keep_slash) {
    size_t used = strlen(out);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char * c = (const unsigned char *) text; *c; c++) {
        bool plain = isalnum(*c) || *c == '-' || *c == '_' || *c == '.' || *c == '~' || (keep_slash && *c == '/');
        if (used + (plain ? 1 : 3) >= size) return false;
        if (plain) out[used++] = (char) *c;
        else {
            out[used++] = '%';
            out[used++] = hex[*c >> 4];
            out[used++] = hex[*c & 15];
        }
    }
    out[used] = '\0';
    return true;
}

/* mkdir -p for the folders above path (not path itself). */
static bool dw_make_parents(const char * path) {
    char buf[PATH_MAX];
    if (strlen(path) >= sizeof(buf)) return false;
    strcpy(buf, path);
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return true;
}

static bool dw_join(char * out, size_t size, const char * root, const char * rel) {
    int n = snprintf(out, size, "%s/%s", root, rel);
    return n > 0 && (size_t) n < size;
}

/* ---- a JSON reader that works in place ---------------------------------------------------- *
 * The manifest lists every file in Music (tens of thousands of rows). cJSON
 * would keep five nodes and four allocations per row; this reads the text in
 * place instead: strings are unescaped where they lie and NUL-terminated, so
 * a row is three pointers into the one buffer. */

typedef struct {
    char * p;
    char * end;
    bool bad;
} dw_json_t;

static void dw_ws(dw_json_t * j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}

static bool dw_peek(dw_json_t * j, char c) {
    dw_ws(j);
    return j->p < j->end && *j->p == c;
}

static bool dw_take(dw_json_t * j, char c) {
    if (!dw_peek(j, c)) return false;
    j->p++;
    return true;
}

static int dw_hex4(const char * p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char) p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v * 16 + d;
    }
    return v;
}

static char * dw_put_utf8(char * w, unsigned cp) {
    if (cp < 0x80) *w++ = (char) cp;
    else if (cp < 0x800) { *w++ = (char) (0xC0 | (cp >> 6)); *w++ = (char) (0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *w++ = (char) (0xE0 | (cp >> 12)); *w++ = (char) (0x80 | ((cp >> 6) & 0x3F)); *w++ = (char) (0x80 | (cp & 0x3F));
    } else {
        *w++ = (char) (0xF0 | (cp >> 18)); *w++ = (char) (0x80 | ((cp >> 12) & 0x3F));
        *w++ = (char) (0x80 | ((cp >> 6) & 0x3F)); *w++ = (char) (0x80 | (cp & 0x3F));
    }
    return w;
}

/* The string at the cursor, unescaped in place; NULL (and bad) when malformed. */
static char * dw_string(dw_json_t * j) {
    if (!dw_take(j, '"')) { j->bad = true; return NULL; }
    char * start = j->p, * w = j->p;
    while (j->p < j->end) {
        char c = *j->p++;
        if (c == '"') { *w = '\0'; return start; }
        if ((unsigned char) c < 32) break;
        if (c != '\\') { *w++ = c; continue; }
        if (j->p >= j->end) break;
        char e = *j->p++;
        switch (e) {
        case '"': case '\\': case '/': *w++ = e; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'n': *w++ = '\n'; break;
        case 'r': *w++ = '\r'; break;
        case 't': *w++ = '\t'; break;
        case 'u': {
            if (j->end - j->p < 4) { j->bad = true; return NULL; }
            int cp = dw_hex4(j->p);
            if (cp < 0) { j->bad = true; return NULL; }
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                int lo = dw_hex4(j->p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    j->p += 6;
                }
            }
            if (cp == 0 || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD; /* no NULs, no lone surrogates */
            w = dw_put_utf8(w, (unsigned) cp); /* never longer than the 6+ bytes it came from */
            break;
        }
        default: j->bad = true; return NULL;
        }
    }
    j->bad = true;
    return NULL;
}

static bool dw_number(dw_json_t * j, double * out) {
    dw_ws(j);
    char * start = j->p;
    while (j->p < j->end && (isdigit((unsigned char) *j->p) || strchr("+-.eE", *j->p))) j->p++;
    if (j->p == start) { j->bad = true; return false; }
    char tmp[48];
    size_t n = (size_t) (j->p - start);
    if (n >= sizeof(tmp)) { j->bad = true; return false; }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    char * end;
    *out = strtod(tmp, &end);
    if (*end) { j->bad = true; return false; }
    return true;
}

static void dw_skip(dw_json_t * j, int depth) {
    if (depth > 64) { j->bad = true; return; }
    dw_ws(j);
    if (j->p >= j->end) { j->bad = true; return; }
    char c = *j->p;
    if (c == '"') { dw_string(j); return; }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (dw_take(j, close)) return;
        do {
            if (c == '{') { dw_string(j); if (!dw_take(j, ':')) { j->bad = true; return; } }
            dw_skip(j, depth + 1);
            if (j->bad) return;
        } while (dw_take(j, ','));
        if (!dw_take(j, close)) j->bad = true;
        return;
    }
    if (c == 't' && j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) { j->p += 4; return; }
    if (c == 'f' && j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; return; }
    if (c == 'n' && j->end - j->p >= 4 && !memcmp(j->p, "null", 4)) { j->p += 4; return; }
    double ignored;
    dw_number(j, &ignored);
}

/* ---- the manifest and the record ---------------------------------------------------------- */

typedef struct {
    const char * p;        /* path in Discworld's Music */
    const char * c;        /* path on the card */
    const char * v;        /* version */
    uint64_t s;            /* bytes */
    const char * card_p;   /* what the record says is on the card for c, NULL when nothing */
    const char * card_v;
} dw_file_t;

typedef struct {
    const char * c, * p, * v;
    bool wanted;   /* the manifest still lists it */
    bool removed;
} dw_rec_t;

typedef struct {
    char * text;                 /* the manifest body, strings point into it */
    dw_file_t * files;
    int count;
    char * record_text;          /* the record as read, its strings likewise */
    dw_rec_t * rec;
    int rec_count;
    bool had_record;
    char ** usb_playlists;       /* the USB page's "playlists", kept as they are */
    int usb_playlist_count;
    char ** our_playlists;       /* compas_playlists: what this wrote to Playlists/ */
    int our_playlist_count;
} dw_state_t;

static bool dw_array_push(char *** list, int * count, const char * text) {
    char * copy = strdup(text);
    char ** grown = copy ? realloc(*list, sizeof(char *) * (size_t) (*count + 1)) : NULL;
    if (!grown) { free(copy); return false; }
    grown[(*count)++] = copy;
    *list = grown;
    return true;
}

static void dw_array_free(char *** list, int * count) {
    for (int i = 0; i < *count; i++) free((*list)[i]);
    free(*list);
    *list = NULL;
    *count = 0;
}

static void dw_state_free(dw_state_t * st) {
    free(st->text);
    free(st->files);
    free(st->record_text);
    free(st->rec);
    dw_array_free(&st->usb_playlists, &st->usb_playlist_count);
    dw_array_free(&st->our_playlists, &st->our_playlist_count);
    memset(st, 0, sizeof(*st));
}

/* {"files":[{"p":..,"c":..,"s":..,"v":..},...],"folder":"Music",...}; takes text. */
static bool dw_parse_manifest(dw_state_t * st, char * text, size_t len) {
    st->text = text;
    dw_json_t j = { text, text + len, false };
    int cap = 0;
    if (!dw_take(&j, '{')) return false;
    if (dw_take(&j, '}')) return false; /* no "files": not a manifest */
    bool saw_files = false;
    do {
        char * key = dw_string(&j);
        if (!key || !dw_take(&j, ':')) return false;
        if (strcmp(key, "folder") == 0) {
            char * folder = dw_string(&j);
            if (!folder || strcmp(folder, DW_MUSIC_FOLDER) != 0) return false; /* a layout this does not know */
            continue;
        }
        if (strcmp(key, "files") != 0) { dw_skip(&j, 0); if (j.bad) return false; continue; }
        saw_files = true;
        if (!dw_take(&j, '[')) return false;
        if (dw_take(&j, ']')) continue;
        do {
            dw_file_t f = { 0 };
            bool has_s = false;
            if (!dw_take(&j, '{')) return false;
            if (!dw_take(&j, '}')) {
                do {
                    char * k = dw_string(&j);
                    if (!k || !dw_take(&j, ':')) return false;
                    if (strcmp(k, "p") == 0) f.p = dw_string(&j);
                    else if (strcmp(k, "c") == 0) f.c = dw_string(&j);
                    else if (strcmp(k, "v") == 0) f.v = dw_string(&j);
                    else if (strcmp(k, "s") == 0) {
                        double s;
                        if (!dw_number(&j, &s) || s < 0) return false;
                        f.s = (uint64_t) s;
                        has_s = true;
                    } else dw_skip(&j, 0);
                    if (j.bad) return false;
                } while (dw_take(&j, ','));
                if (!dw_take(&j, '}')) return false;
            }
            if (!f.p || !f.c || !f.v || !has_s || !f.p[0]) return false;
            if (!discworld_card_path_ok(f.c)) continue; /* never write outside Music/ or Audiobooks/ */
            if (st->count == cap) {
                cap = cap ? cap * 2 : 1024;
                dw_file_t * grown = realloc(st->files, sizeof(*grown) * (size_t) cap);
                if (!grown) return false;
                st->files = grown;
            }
            st->files[st->count++] = f;
        } while (dw_take(&j, ','));
        if (!dw_take(&j, ']')) return false;
    } while (dw_take(&j, ','));
    return dw_take(&j, '}') && saw_files;
}

static bool dw_parse_string_array(dw_json_t * j, char *** list, int * count) {
    if (!dw_take(j, '[')) return false;
    if (dw_take(j, ']')) return true;
    do {
        char * s = dw_string(j);
        if (!s || !dw_array_push(list, count, s)) return false;
    } while (dw_take(j, ','));
    return dw_take(j, ']');
}

/* The record the last sync (USB or this) left; false only when it is there
 * but cannot be read. A record that is not valid JSON counts as none, as the
 * USB page treats it. */
static bool dw_read_record(dw_state_t * st, const char * card_root) {
    char path[PATH_MAX];
    if (!dw_join(path, sizeof(path), card_root, DISCWORLD_RECORD_NAME)) return false;
    FILE * f = fopen(path, "rb");
    if (!f) return errno == ENOENT;
    struct stat sb;
    if (fstat(fileno(f), &sb) != 0 || sb.st_size < 0 || (uint64_t) sb.st_size > DW_RECORD_MAX_BYTES) {
        fclose(f);
        return false;
    }
    size_t len = (size_t) sb.st_size;
    char * text = malloc(len + 1);
    if (!text) { fclose(f); return false; }
    bool read_ok = fread(text, 1, len, f) == len;
    fclose(f);
    if (!read_ok) { free(text); return false; }
    text[len] = '\0';
    st->record_text = text;

    dw_json_t j = { text, text + len, false };
    int cap = 0;
    bool files_seen = false;
    if (!dw_take(&j, '{')) goto invalid;
    if (!dw_take(&j, '}')) {
        do {
            char * key = dw_string(&j);
            if (!key || !dw_take(&j, ':')) goto invalid;
            if (strcmp(key, "files") == 0) {
                files_seen = true;
                if (!dw_take(&j, '{')) goto invalid;
                if (dw_take(&j, '}')) continue;
                do {
                    dw_rec_t r = { 0 };
                    r.c = dw_string(&j);
                    if (!r.c || !dw_take(&j, ':') || !dw_take(&j, '{')) goto invalid;
                    if (!dw_take(&j, '}')) {
                        do {
                            char * k = dw_string(&j);
                            if (!k || !dw_take(&j, ':')) goto invalid;
                            if (strcmp(k, "p") == 0) r.p = dw_string(&j);
                            else if (strcmp(k, "v") == 0) r.v = dw_string(&j);
                            else dw_skip(&j, 0);
                            if (j.bad) goto invalid;
                        } while (dw_take(&j, ','));
                        if (!dw_take(&j, '}')) goto invalid;
                    }
                    if (!r.p) r.p = "";
                    if (!r.v) r.v = "";
                    if (st->rec_count == cap) {
                        cap = cap ? cap * 2 : 1024;
                        dw_rec_t * grown = realloc(st->rec, sizeof(*grown) * (size_t) cap);
                        if (!grown) return false;
                        st->rec = grown;
                    }
                    st->rec[st->rec_count++] = r;
                } while (dw_take(&j, ','));
                if (!dw_take(&j, '}')) goto invalid;
            } else if (strcmp(key, "playlists") == 0 && dw_peek(&j, '[')) {
                if (!dw_parse_string_array(&j, &st->usb_playlists, &st->usb_playlist_count)) goto invalid;
            } else if (strcmp(key, "compas_playlists") == 0 && dw_peek(&j, '[')) {
                if (!dw_parse_string_array(&j, &st->our_playlists, &st->our_playlist_count)) goto invalid;
            } else {
                dw_skip(&j, 0);
                if (j.bad) goto invalid;
            }
        } while (dw_take(&j, ','));
        if (!dw_take(&j, '}')) goto invalid;
    }
    st->had_record = files_seen; /* the USB page also wants "files" */
    return true;
invalid:
    free(st->rec);
    st->rec = NULL;
    st->rec_count = 0;
    dw_array_free(&st->usb_playlists, &st->usb_playlist_count);
    dw_array_free(&st->our_playlists, &st->our_playlist_count);
    st->had_record = false;
    return true;
}

static int dw_rec_cmp(const void * a, const void * b) {
    return strcmp(((const dw_rec_t *) a)->c, ((const dw_rec_t *) b)->c);
}

static int dw_file_ptr_cmp(const void * a, const void * b) {
    return strcmp((*(dw_file_t * const *) a)->c, (*(dw_file_t * const *) b)->c);
}

static int dw_file_cmp(const void * a, const void * b) {
    return strcmp(((const dw_file_t *) a)->c, ((const dw_file_t *) b)->c);
}

static dw_rec_t * dw_rec_find(dw_state_t * st, const char * c) {
    dw_rec_t key = { .c = c };
    return st->rec_count ? bsearch(&key, st->rec, (size_t) st->rec_count, sizeof(dw_rec_t), dw_rec_cmp) : NULL;
}

static bool dw_write_json_string(FILE * f, const char * s) {
    if (fputc('"', f) == EOF) return false;
    for (const unsigned char * c = (const unsigned char *) s; *c; c++) {
        int ok;
        if (*c == '"' || *c == '\\') ok = fprintf(f, "\\%c", *c);
        else if (*c < 32) ok = fprintf(f, "\\u%04x", *c);
        else ok = fputc(*c, f) == EOF ? -1 : 1;
        if (ok < 0) return false;
    }
    return fputc('"', f) != EOF;
}

static bool dw_write_string_array(FILE * f, char ** list, int count) {
    if (fputc('[', f) == EOF) return false;
    for (int i = 0; i < count; i++)
        if ((i && fputc(',', f) == EOF) || !dw_write_json_string(f, list[i])) return false;
    return fputc(']', f) != EOF;
}

static bool dw_write_entry(FILE * f, bool * first, const char * c, const char * p, const char * v) {
    if (!*first && fputc(',', f) == EOF) return false;
    *first = false;
    return dw_write_json_string(f, c) && fputs(":{\"p\":", f) >= 0 && dw_write_json_string(f, p) &&
           fputs(",\"v\":", f) >= 0 && dw_write_json_string(f, v) && fputc('}', f) != EOF;
}

/* Saves what is on the card now, through a temporary file renamed over the
 * old record, so a sync cut off mid-write still leaves the last good one. */
static bool dw_save_record(const dw_state_t * st, const char * card_root) {
    char path[PATH_MAX], tmp[PATH_MAX];
    if (!dw_join(path, sizeof(path), card_root, DISCWORLD_RECORD_NAME) ||
        !dw_join(tmp, sizeof(tmp), card_root, DISCWORLD_RECORD_NAME ".compas-part")) return false;
    FILE * f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fputs("{\"files\":{", f) >= 0, first = true;
    for (int i = 0; ok && i < st->count; i++) {
        const dw_file_t * file = &st->files[i];
        if (file->card_v) ok = dw_write_entry(f, &first, file->c, file->card_p, file->card_v);
    }
    for (int i = 0; ok && i < st->rec_count; i++) {
        const dw_rec_t * r = &st->rec[i];
        if (!r->wanted && !r->removed) ok = dw_write_entry(f, &first, r->c, r->p, r->v);
    }
    char at[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(at, sizeof(at), "%Y-%m-%dT%H:%M:%S.000Z", &tm);
    ok = ok && fputs("},\"playlists\":", f) >= 0 && dw_write_string_array(f, st->usb_playlists, st->usb_playlist_count) &&
         fputs(",\"compas_playlists\":", f) >= 0 && dw_write_string_array(f, st->our_playlists, st->our_playlist_count) &&
         fprintf(f, ",\"at\":\"%s\"}", at) > 0;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* ---- talking to Discworld ----------------------------------------------------------------- */

typedef struct {
    const discworld_config_t * config;
    http_cancel_token_t * cancel;
    discworld_keep_going_fn keep_going;
    discworld_progress_fn progress_fn;
    void * user;
    discworld_progress_t progress;
    discworld_result_t * result;
    char cookie[HTTP_HEADER_VALUE_MAX];
    http_request_t * req;   /* ~26 KiB each: on the heap, not a worker's stack */
    http_response_t * resp;
} dw_ctx_t;

static bool dw_stopping(dw_ctx_t * x) {
    return (x->cancel && http_cancel_token_is_cancelled(x->cancel)) || (x->keep_going && !x->keep_going(x->user));
}

static void dw_report(dw_ctx_t * x) {
    if (x->progress_fn) x->progress_fn(&x->progress, x->user);
}

static void dw_request_init(dw_ctx_t * x, const char * path_and_query) {
    memset(x->req, 0, sizeof(*x->req));
    snprintf(x->req->url, sizeof(x->req->url), "%s%s", x->config->base_url, path_and_query);
    x->req->method = HTTP_METHOD_GET;
    x->req->verify_tls = false; /* Discworld's https is signed by its own home CA; plain http is the norm */
    x->req->connect_timeout_ms = DW_CONNECT_TIMEOUT_MS;
    x->req->read_timeout_ms = DW_READ_TIMEOUT_MS;
    x->req->total_timeout_ms = DW_API_TOTAL_TIMEOUT_MS;
    x->req->redirect_limit = 0; /* a redirect is the sign-in page: never follow it */
    if (x->cookie[0]) {
        snprintf(x->req->headers[0].name, sizeof(x->req->headers[0].name), "Cookie");
        snprintf(x->req->headers[0].value, sizeof(x->req->headers[0].value), "%s", x->cookie);
        x->req->header_count = 1;
    }
}

/* Signs in with the password (Discworld's form at /login) and keeps the
 * session cookie it hands back. */
static discworld_status_t dw_login(dw_ctx_t * x) {
    if (!x->config->password[0]) {
        dw_detail(x->result, "%s", "Discworld asked for its password");
        return DISCWORLD_ERR_PASSWORD;
    }
    char body[512] = "password=";
    if (!dw_url_append_encoded(body, sizeof(body), x->config->password, false)) return DISCWORLD_ERR_ARGUMENT;
    x->cookie[0] = '\0';
    dw_request_init(x, "/login");
    x->req->method = HTTP_METHOD_POST;
    x->req->body = (const uint8_t *) body;
    x->req->body_len = strlen(body);
    x->req->content_type = "application/x-www-form-urlencoded";
    bool ok = http_request_ex(x->req, x->cancel, x->resp);
    memset(body, 0, sizeof(body));
    if (!ok) {
        dw_detail(x->result, "%s", dw_why(x->resp->error));
        http_response_free(x->resp);
        return dw_stopping(x) ? DISCWORLD_STOPPED : DISCWORLD_ERR_CONNECT;
    }
    const char * set = http_headers_get(x->resp->headers, x->resp->header_count, "Set-Cookie");
    int status = x->resp->status;
    if (status >= 300 && status < 400 && set) {
        size_t n = strcspn(set, ";");
        if (n && n < sizeof(x->cookie)) {
            memcpy(x->cookie, set, n);
            x->cookie[n] = '\0';
        }
    }
    http_response_free(x->resp);
    if (!x->cookie[0]) {
        dw_detail(x->result, "%s", "Discworld did not accept the password");
        return DISCWORLD_ERR_PASSWORD;
    }
    return DISCWORLD_OK;
}

/* GETs a JSON API path; on a 401 signs in once and asks again. *out_status
 * is the HTTP status; the body stays in x->resp for the caller to take. */
static discworld_status_t dw_get(dw_ctx_t * x, const char * path, size_t max_bytes, int * out_status) {
    for (int attempt = 0; attempt < 2; attempt++) {
        dw_request_init(x, path);
        x->req->max_response_bytes = max_bytes;
        if (!http_request_ex(x->req, x->cancel, x->resp)) {
            dw_detail(x->result, "%s", dw_why(x->resp->error));
            http_response_free(x->resp);
            return dw_stopping(x) ? DISCWORLD_STOPPED : DISCWORLD_ERR_CONNECT;
        }
        int status = x->resp->status;
        if (status >= 300 && status < 400) {
            const char * to = http_headers_get(x->resp->headers, x->resp->header_count, "Location");
            if (to && strncasecmp(to, "https://", 8) == 0 && strncasecmp(x->config->base_url, "http://", 7) == 0) {
                /* Discworld behind a proxy that sends http to https: look for it there. */
                http_response_free(x->resp);
                dw_detail(x->result, "%s", "it only answers on https");
                return DISCWORLD_ERR_CONNECT;
            }
        }
        bool sign_in = status == 401 || (status >= 300 && status < 400);
        if (sign_in && attempt == 0) {
            http_response_free(x->resp);
            discworld_status_t s = dw_login(x);
            if (s != DISCWORLD_OK) return s;
            continue;
        }
        if (sign_in) {
            http_response_free(x->resp);
            dw_detail(x->result, "%s", "Discworld did not accept the password");
            return DISCWORLD_ERR_PASSWORD;
        }
        *out_status = status;
        return DISCWORLD_OK;
    }
    return DISCWORLD_ERR_SERVER;
}

/* The server's own words on an error answer ({"error": "..."}), for the detail line. */
static void dw_server_error(dw_ctx_t * x, int status) {
    char msg[120] = "";
    if (x->resp->body && x->resp->body_len) {
        cJSON * root = cJSON_ParseWithLength((const char *) x->resp->body, x->resp->body_len);
        const cJSON * e = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsString(e) && e->valuestring) snprintf(msg, sizeof(msg), "%s", e->valuestring);
        cJSON_Delete(root);
    }
    if (msg[0]) snprintf(x->result->detail, sizeof(x->result->detail), "Discworld: %s", msg);
    else snprintf(x->result->detail, sizeof(x->result->detail), "Discworld answered %d", status);
}

static void dw_sleep_ms(dw_ctx_t * x, unsigned ms) {
    for (unsigned slept = 0; slept < ms && !dw_stopping(x); slept += 100) {
        struct timespec ts = { 0, 100 * 1000000L };
        nanosleep(&ts, NULL);
    }
}

static discworld_status_t dw_fetch_manifest(dw_ctx_t * x, dw_state_t * st) {
    unsigned waited = 0;
    for (;;) {
        int status = 0;
        discworld_status_t s = dw_get(x, "/api/hiby/manifest", DW_MANIFEST_MAX_BYTES, &status);
        if (s != DISCWORLD_OK) return s;
        if (status == 200 && x->resp->body && x->resp->body_len) {
            char * text = (char *) x->resp->body;
            size_t len = x->resp->body_len;
            x->resp->body = NULL; /* the manifest owns it now */
            x->resp->body_len = 0;
            http_response_free(x->resp);
            if (!dw_parse_manifest(st, text, len)) {
                dw_detail(x->result, "%s", "Discworld's list of Music could not be read");
                return DISCWORLD_ERR_SERVER;
            }
            return DISCWORLD_OK;
        }
        if (status == 202) {   /* still listing: {building, done, total} */
            cJSON * root = cJSON_ParseWithLength((const char *) x->resp->body, x->resp->body_len);
            const cJSON * d = cJSON_GetObjectItemCaseSensitive(root, "done");
            const cJSON * t = cJSON_GetObjectItemCaseSensitive(root, "total");
            x->progress.phase = DISCWORLD_PHASE_LISTING;
            x->progress.list_done = cJSON_IsNumber(d) ? d->valueint : 0;
            x->progress.list_total = cJSON_IsNumber(t) ? t->valueint : 0;
            cJSON_Delete(root);
            http_response_free(x->resp);
            dw_report(x);
            if (waited >= DW_LISTING_MAX_WAIT_MS) {
                dw_detail(x->result, "%s", "Discworld took too long to list Music");
                return DISCWORLD_ERR_SERVER;
            }
            dw_sleep_ms(x, DW_LISTING_WAIT_MS);
            waited += DW_LISTING_WAIT_MS;
            if (dw_stopping(x)) return DISCWORLD_STOPPED;
            continue;
        }
        dw_server_error(x, status);
        http_response_free(x->resp);
        return DISCWORLD_ERR_SERVER;
    }
}

/* ---- the sync ----------------------------------------------------------------------------- */

typedef struct {
    dw_ctx_t * x;
    uint64_t base;   /* bytes of earlier files */
    uint64_t last_report;
} dw_dl_progress_t;

static bool dw_download_progress(uint64_t received, uint64_t total, void * user) {
    (void) total;
    dw_dl_progress_t * d = user;
    d->x->progress.bytes_done = d->base + received;
    if (received - d->last_report >= 256 * 1024) {
        d->last_report = received;
        dw_report(d->x);
    }
    return !(d->x->keep_going && !d->x->keep_going(d->x->user));
}

static bool dw_file_exists(const char * root, const char * rel, uint64_t * size) {
    char path[PATH_MAX];
    struct stat sb;
    if (!dw_join(path, sizeof(path), root, rel) || stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
    if (size) *size = (uint64_t) sb.st_size;
    return true;
}

/* Deletes what left Music or Audiobooks, then the folders that left empty
 * (never Music or Audiobooks itself). */
static void dw_remove_one(const char * root, const char * rel) {
    char path[PATH_MAX];
    if (!dw_join(path, sizeof(path), root, rel)) return;
    unlink(path);
    char top[PATH_MAX], music[PATH_MAX];
    snprintf(top, sizeof(top), "%.*s", (int) dw_top_folder_len(rel), rel);
    if (!top[0] || !dw_join(music, sizeof(music), root, top)) return;
    for (char * slash = strrchr(path, '/'); slash; slash = strrchr(path, '/')) {
        *slash = '\0';
        if (strlen(path) <= strlen(music) || rmdir(path) != 0) break;
    }
}

typedef enum { DW_COPY_OK, DW_COPY_SKIP, DW_COPY_STOP, DW_COPY_FULL, DW_COPY_CARD, DW_COPY_LOST } dw_copy_t;

static dw_copy_t dw_copy_one(dw_ctx_t * x, const dw_file_t * f, uint64_t base) {
    char dest[PATH_MAX];
    char url[2048];
    if (!dw_join(dest, sizeof(dest), x->config->card_root, f->c)) return DW_COPY_SKIP;
    int n = snprintf(url, sizeof(url), "%s/api/hiby/file?p=", x->config->base_url);
    if (n <= 0 || (size_t) n >= sizeof(url) || !dw_url_append_encoded(url, sizeof(url), f->p, true)) return DW_COPY_SKIP;
    if (!dw_make_parents(dest)) return errno == ENOSPC ? DW_COPY_FULL : DW_COPY_CARD;
    download_request_t request = { .url = url, .dest_path = dest, .max_bytes = DW_FILE_MAX_BYTES,
                                   .verify_tls = false, .connect_timeout_ms = DW_CONNECT_TIMEOUT_MS,
                                   .read_timeout_ms = DW_READ_TIMEOUT_MS, .required_status = 200,
                                   .cookie = x->cookie[0] ? x->cookie : NULL };
    download_result_t result;
    dw_dl_progress_t d = { x, base, 0 };
    download_error_t e = download_to_file(&request, x->cancel, dw_download_progress, &d, &result);
    if (e == DOWNLOAD_OK || (e == DOWNLOAD_ERR_DIR_SYNC && result.committed)) {
        x->result->bytes += result.bytes_written;
        return DW_COPY_OK;
    }
    if (e == DOWNLOAD_ERR_CANCELLED || dw_stopping(x)) return DW_COPY_STOP;
    if (result.system_errno == ENOSPC || result.system_errno == EDQUOT) return DW_COPY_FULL;
    if (e == DOWNLOAD_ERR_HTTP_STATUS) return DW_COPY_SKIP;   /* gone from Music since the list was made */
    if (e == DOWNLOAD_ERR_TRANSPORT || e == DOWNLOAD_ERR_REDIRECT) return DW_COPY_LOST;
    if (e == DOWNLOAD_ERR_OPEN || e == DOWNLOAD_ERR_WRITE || e == DOWNLOAD_ERR_SYNC ||
        e == DOWNLOAD_ERR_CLOSE || e == DOWNLOAD_ERR_RENAME) return DW_COPY_CARD;
    return DW_COPY_SKIP;
}

/* A playlist file name the server sent, safe to create in Playlists/. */
static bool dw_playlist_name_ok(const char * name) {
    size_t len = name ? strlen(name) : 0;
    if (len < 6 || len > 200 || name[0] == '.' || strcasecmp(name + len - 5, ".m3u8") != 0) return false;
    for (const unsigned char * c = (const unsigned char *) name; *c; c++)
        if (*c < 32 || strchr("/\\:*?\"<>|", *c)) return false;
    return true;
}

static bool dw_person_name_ok(const char * name) {
    size_t len = strlen(name);
    if (!len || len > 60 || name[0] == '.' || name[len - 1] == ' ' || name[len - 1] == '.') return false;
    for (const unsigned char * c = (const unsigned char *) name; *c; c++)
        if (*c < 32 || strchr("/\\:*?\"<>|", *c)) return false;
    return true;
}

void discworld_each_person(const char * people, void (*fn)(const char * name, void * user), void * user) {
    if (!people || !fn) return;
    const char * p = people;
    while (*p) {
        const char * comma = strchr(p, ',');
        size_t n = comma ? (size_t) (comma - p) : strlen(p);
        while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
        char name[64];
        if (n && n < sizeof(name)) {
            memcpy(name, p, n);
            name[n] = '\0';
            if (dw_person_name_ok(name)) fn(name, user);
        }
        if (!comma) break;
        p = comma + 1;
    }
}

/* A playlist this sync wrote, as the record keeps it: relative to Playlists/,
 * a bare name (the first builds), in the favourites folder, or in a person's
 * folder inside it. */
static bool dw_recorded_playlist_ok(const char * entry) {
    static const char * const subs[] = { DISCWORLD_FAVOURITES_DIR "/", DISCWORLD_FAVORITES_DIR "/" };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        if (strncmp(entry, subs[i], strlen(subs[i])) != 0) continue;
        const char * rest = entry + strlen(subs[i]);
        const char * slash = strchr(rest, '/');
        if (!slash) return dw_playlist_name_ok(rest);
        char person[64];
        size_t n = (size_t) (slash - rest);
        if (n >= sizeof(person)) return false;
        memcpy(person, rest, n);
        person[n] = '\0';
        return dw_person_name_ok(person) && dw_playlist_name_ok(slash + 1);
    }
    return dw_playlist_name_ok(entry);
}

/* Discworld writes each song as "../Music/..." from a playlist directly in
 * Playlists/; `levels` folders deeper, every relative entry needs that many
 * more "../". */
static char * dw_playlist_deeper(const char * text, int levels) {
    size_t lines = 1;
    for (const char * c = text; *c; c++) if (*c == '\n') lines++;
    size_t len = strlen(text);
    char * out = malloc(len + lines * 3 * (size_t) levels + 1);
    if (!out) return NULL;
    char * w = out;
    const char * line = text;
    while (*line) {
        const char * end = strchr(line, '\n');
        size_t n = end ? (size_t) (end - line) + 1 : strlen(line);
        /* A first line may start with the BOM before "#EXTM3U"; songs never do. */
        if (n >= 3 && line[0] == '.' && line[1] == '.' && line[2] == '/')
            for (int i = 0; i < levels; i++) { memcpy(w, "../", 3); w += 3; }
        memcpy(w, line, n);
        w += n;
        line += n;
    }
    *w = '\0';
    return out;
}

static bool dw_list_has(char ** list, int count, const char * name) {
    for (int i = 0; i < count; i++) if (strcasecmp(list[i], name) == 0) return true;
    return false;
}

/* True when the file at path holds exactly text. */
static bool dw_file_equals(const char * path, const char * text, size_t len) {
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    bool same = true;
    char buf[4096];
    size_t at = 0, n;
    while (same && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        same = at + n <= len && memcmp(buf, text + at, n) == 0;
        at += n;
    }
    same = same && !ferror(f) && at == len;
    fclose(f);
    return same;
}

static bool dw_write_file_atomic(const char * path, const char * text, size_t len) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.compas-part", path);
    if (n <= 0 || (size_t) n >= sizeof(tmp)) return false;
    FILE * f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* One person's playlists (or the shared ones when who is empty) into
 * folder (relative to Playlists/), adding what it wrote to *now. */
static discworld_status_t dw_sync_person(dw_ctx_t * x, dw_state_t * st, const char * who, const char * folder,
                                         int levels, char *** now, int * now_count) {
    char path[600] = "/api/hiby/playlists?who=";
    if (!dw_url_append_encoded(path, sizeof(path), who, false)) return DISCWORLD_ERR_ARGUMENT;
    int status = 0;
    discworld_status_t s = dw_get(x, path, DW_PLAYLISTS_MAX_BYTES, &status);
    if (s != DISCWORLD_OK) return s;
    if (status != 200) {
        dw_server_error(x, status);
        http_response_free(x->resp);
        return DISCWORLD_ERR_SERVER;
    }
    cJSON * root = x->resp->body ? cJSON_ParseWithLength((const char *) x->resp->body, x->resp->body_len) : NULL;
    http_response_free(x->resp);
    const cJSON * lists = cJSON_GetObjectItemCaseSensitive(root, "playlists");
    if (!cJSON_IsArray(lists)) {
        cJSON_Delete(root);
        dw_detail(x->result, "%s", "Discworld's playlists could not be read");
        return DISCWORLD_ERR_SERVER;
    }
    char base[PATH_MAX], dir[PATH_MAX];
    if (!dw_join(base, sizeof(base), x->config->card_root, DISCWORLD_PLAYLISTS_DIR) ||
        !dw_join(dir, sizeof(dir), base, folder) || !dw_make_parents(dir) ||
        (mkdir(dir, 0755) != 0 && errno != EEXIST)) {
        cJSON_Delete(root);
        return DISCWORLD_ERR_CARD;
    }
    discworld_status_t outcome = DISCWORLD_OK;
    const cJSON * item;
    cJSON_ArrayForEach(item, lists) {
        const cJSON * file = cJSON_GetObjectItemCaseSensitive(item, "file");
        const cJSON * text = cJSON_GetObjectItemCaseSensitive(item, "text");
        if (!cJSON_IsString(file) || !cJSON_IsString(text) || !dw_playlist_name_ok(file->valuestring)) continue;
        /* The user's own playlist of the same name stays: this one gets "(Discworld)". */
        char name[400], full[PATH_MAX];
        snprintf(name, sizeof(name), "%s/%s", folder, file->valuestring);
        if (!dw_join(full, sizeof(full), base, name)) continue;
        char * body = dw_playlist_deeper(text->valuestring, levels);
        if (!body) { outcome = DISCWORLD_ERR_MEMORY; break; }
        size_t body_len = strlen(body);
        /* A file already holding exactly this text counts as this sync's own
         * (a record lost or rewritten elsewhere must not breed copies). */
        struct stat sb;
        bool same = dw_file_equals(full, body, body_len);
        bool mine = same || dw_list_has(st->our_playlists, st->our_playlist_count, name);
        if (!mine && stat(full, &sb) == 0) {
            size_t stem = strlen(file->valuestring) - 5;
            snprintf(name, sizeof(name), "%s/%.*s (Discworld).m3u8", folder, (int) stem, file->valuestring);
            if (!dw_join(full, sizeof(full), base, name)) { free(body); continue; }
            same = dw_file_equals(full, body, body_len);
            mine = same || dw_list_has(st->our_playlists, st->our_playlist_count, name);
            if (!mine && stat(full, &sb) == 0) { free(body); continue; }
        }
        if (dw_list_has(*now, *now_count, name)) { free(body); continue; }
        if (!same) {
            bool written = dw_write_file_atomic(full, body, body_len);
            int saved = errno;
            free(body);
            if (!written) {
                outcome = saved == ENOSPC ? DISCWORLD_ERR_FULL : DISCWORLD_ERR_CARD;
                break;
            }
            x->result->changed = true;
        } else {
            free(body);
        }
        if (!dw_array_push(now, now_count, name)) { outcome = DISCWORLD_ERR_MEMORY; break; }
        x->result->playlists++;
    }
    cJSON_Delete(root);
    return outcome;
}

typedef struct {
    dw_ctx_t * x;
    dw_state_t * st;
    const char * fav;
    char *** now;
    int * now_count;
    discworld_status_t outcome;
} dw_people_t;

static void dw_sync_one_person(const char * name, void * user) {
    dw_people_t * p = user;
    if (p->outcome != DISCWORLD_OK || dw_stopping(p->x)) return;
    char folder[128];
    snprintf(folder, sizeof(folder), "%s/%s", p->fav, name);
    p->outcome = dw_sync_person(p->x, p->st, name, folder, 2, p->now, p->now_count);
}

/* Writes the playlists to Playlists/Favourites/ (the Home Favourites tile
 * reads that folder; an existing Favorites/ spelling is used instead): each
 * person's into a folder of their own, fresh each time. Then deletes the
 * ones this wrote before that Discworld no longer has, including ones older
 * builds put elsewhere. */
static discworld_status_t dw_sync_playlists(dw_ctx_t * x, dw_state_t * st) {
    char dir[PATH_MAX], fav[PATH_MAX], other[PATH_MAX];
    const char * sub = DISCWORLD_FAVOURITES_DIR;
    struct stat dsb;
    if (!dw_join(dir, sizeof(dir), x->config->card_root, DISCWORLD_PLAYLISTS_DIR) ||
        !dw_join(fav, sizeof(fav), dir, DISCWORLD_FAVOURITES_DIR) ||
        !dw_join(other, sizeof(other), dir, DISCWORLD_FAVORITES_DIR)) return DISCWORLD_ERR_CARD;
    if (stat(fav, &dsb) != 0 && stat(other, &dsb) == 0 && S_ISDIR(dsb.st_mode)) sub = DISCWORLD_FAVORITES_DIR;

    char ** now = NULL;
    int now_count = 0;
    dw_people_t people = { x, st, sub, &now, &now_count, DISCWORLD_OK };
    bool any = false;
    for (const char * c = x->config->people; *c; c++) if (*c != ',' && *c != ' ') { any = true; break; }
    if (any) discworld_each_person(x->config->people, dw_sync_one_person, &people);
    else people.outcome = dw_sync_person(x, st, "", sub, 1, &now, &now_count);
    discworld_status_t outcome = people.outcome;
    if (outcome == DISCWORLD_OK && dw_stopping(x)) outcome = DISCWORLD_STOPPED;

    if (outcome != DISCWORLD_OK) {
        /* Keep the old list too, so nothing written is forgotten. */
        for (int i = 0; i < st->our_playlist_count; i++)
            if (!dw_list_has(now, now_count, st->our_playlists[i]))
                dw_array_push(&now, &now_count, st->our_playlists[i]);
    } else {
        for (int i = 0; i < st->our_playlist_count; i++) {
            if (dw_list_has(now, now_count, st->our_playlists[i]) || !dw_recorded_playlist_ok(st->our_playlists[i])) continue;
            char full[PATH_MAX];
            if (!dw_join(full, sizeof(full), dir, st->our_playlists[i]) || unlink(full) != 0) continue;
            x->result->changed = true;
            /* A person taken off the list leaves no empty folder behind. */
            char * slash = strrchr(full, '/');
            if (slash) { *slash = '\0'; if (strlen(full) > strlen(fav) + 1) rmdir(full); }
        }
    }
    dw_array_free(&st->our_playlists, &st->our_playlist_count);
    st->our_playlists = now;
    st->our_playlist_count = now_count;
    return outcome;
}

static discworld_status_t dw_run(dw_ctx_t * x, dw_state_t * st) {
    x->progress.phase = DISCWORLD_PHASE_CONNECTING;
    dw_report(x);
    discworld_status_t s = dw_fetch_manifest(x, st);
    if (s != DISCWORLD_OK) return s;
    if (dw_stopping(x)) return DISCWORLD_STOPPED;

    x->progress.phase = DISCWORLD_PHASE_PLANNING;
    dw_report(x);
    if (!dw_read_record(st, x->config->card_root)) {
        dw_detail(x->result, "%s", "The sync record on the card could not be read");
        return DISCWORLD_ERR_CARD;
    }
    if (st->rec_count) qsort(st->rec, (size_t) st->rec_count, sizeof(dw_rec_t), dw_rec_cmp);
    /* Two rows for one card path: the second would overwrite the first. Keep the first. */
    if (st->count) qsort(st->files, (size_t) st->count, sizeof(dw_file_t), dw_file_cmp);
    int kept = 0;
    for (int i = 0; i < st->count; i++)
        if (!kept || strcmp(st->files[kept - 1].c, st->files[i].c) != 0) st->files[kept++] = st->files[i];
    st->count = kept;

    /* What changed: copy what is new, or whose version changed, or that is gone from the card. */
    dw_file_t ** copy = st->count ? malloc(sizeof(*copy) * (size_t) st->count) : NULL;
    if (st->count && !copy) return DISCWORLD_ERR_MEMORY;
    int copy_count = 0;
    uint64_t copy_bytes = 0;
    for (int i = 0; i < st->count; i++) {
        dw_file_t * f = &st->files[i];
        dw_rec_t * r = dw_rec_find(st, f->c);
        uint64_t size = 0;
        bool there = dw_file_exists(x->config->card_root, f->c, &size);
        if (r) {
            r->wanted = true;
            if (there) { f->card_p = r->p; f->card_v = r->v; }
            if (there && strcmp(r->v, f->v) == 0) continue;
        } else if (!st->had_record && there && !strstr(f->v, "-c") && size == f->s) {
            /* First sync to a card that already holds this library (copied by hand): keep it. */
            f->card_p = f->p;
            f->card_v = f->v;
            x->result->adopted++;
            continue;
        }
        copy[copy_count++] = f;
        copy_bytes += f->s;
    }
    int remove_count = 0;
    for (int i = 0; i < st->rec_count; i++)
        if (!st->rec[i].wanted && discworld_card_path_ok(st->rec[i].c)) remove_count++;
        else if (!st->rec[i].wanted) st->rec[i].removed = true; /* not ours to touch: forget it */

    /* 1. What has gone from Music goes from the card, first, so there is room for what is new. */
    x->progress.phase = DISCWORLD_PHASE_REMOVING;
    x->progress.files_done = 0;
    x->progress.files_total = remove_count;
    dw_report(x);
    for (int i = 0; i < st->rec_count && remove_count; i++) {
        dw_rec_t * r = &st->rec[i];
        if (r->wanted || r->removed) continue;
        if (dw_stopping(x)) { free(copy); dw_save_record(st, x->config->card_root); return DISCWORLD_STOPPED; }
        dw_remove_one(x->config->card_root, r->c);
        r->removed = true;
        x->result->removed++;
        x->result->changed = true;
        x->progress.files_done++;
        dw_report(x);
    }
    if (x->result->removed && !dw_save_record(st, x->config->card_root)) {
        free(copy);
        return DISCWORLD_ERR_CARD;
    }

    /* 2. What is new or changed is copied, in card order (album by album). */
    qsort(copy, (size_t) copy_count, sizeof(*copy), dw_file_ptr_cmp);
    x->progress.phase = DISCWORLD_PHASE_COPYING;
    x->progress.files_done = 0;
    x->progress.files_total = copy_count;
    x->progress.bytes_done = 0;
    x->progress.bytes_total = copy_bytes;
    dw_report(x);
    discworld_status_t outcome = DISCWORLD_OK;
    uint64_t base = 0;
    int since_save = 0, i = 0;
    for (; i < copy_count; i++) {
        dw_file_t * f = copy[i];
        if (dw_stopping(x)) { outcome = DISCWORLD_STOPPED; break; }
        snprintf(x->progress.current, sizeof(x->progress.current), "%s", f->c + dw_top_folder_len(f->c) + 1);
        x->progress.bytes_done = base;
        dw_report(x);
        bool update = f->card_v != NULL;
        dw_copy_t c = dw_copy_one(x, f, base);
        if (c == DW_COPY_OK) {
            f->card_p = f->p;
            f->card_v = f->v;
            if (update) x->result->updated++; else x->result->copied++;
            x->result->changed = true;
            if (++since_save >= DW_SAVE_EVERY) {
                since_save = 0;
                if (!dw_save_record(st, x->config->card_root)) { outcome = DISCWORLD_ERR_CARD; i++; break; }
            }
        } else if (c == DW_COPY_SKIP) {
            x->result->failed++;
        } else {
            outcome = c == DW_COPY_STOP ? DISCWORLD_STOPPED : c == DW_COPY_FULL ? DISCWORLD_ERR_FULL :
                      c == DW_COPY_LOST ? DISCWORLD_ERR_CONNECT : DISCWORLD_ERR_CARD;
            if (c == DW_COPY_LOST) dw_detail(x->result, "%s", "Lost Discworld part way through");
            if (c == DW_COPY_FULL) dw_detail(x->result, "%s", "The card is full");
            break;
        }
        base += f->s;
        x->progress.files_done = i + 1;
    }
    x->result->left = copy_count - i;
    free(copy);
    x->progress.current[0] = '\0';

    /* 3. The playlists, once every song they name has had its chance to be copied. */
    if (outcome == DISCWORLD_OK && !dw_stopping(x)) {
        x->progress.phase = DISCWORLD_PHASE_PLAYLISTS;
        dw_report(x);
        outcome = dw_sync_playlists(x, st);
    } else if (outcome == DISCWORLD_OK) {
        outcome = DISCWORLD_STOPPED;
    }
    if (!dw_save_record(st, x->config->card_root) && outcome == DISCWORLD_OK) outcome = DISCWORLD_ERR_CARD;
    return outcome;
}

/* ---- finding Discworld ------------------------------------------------------
 * When the set address does not answer: Discworld's lan-name.sh announces
 * "Discworld" (_http._tcp) on the Wi-Fi with its host name and port, so ask
 * for that first, then try the addresses its scripts give out. A candidate
 * counts when its /healthz (open without the password) answers like
 * Discworld's. */

#define DW_SERVICE "Discworld._http._tcp.local"
#define DW_PROBE_CONNECT_MS 5000U
#define DW_PROBE_TOTAL_MS 12000U

static bool dw_probe(dw_ctx_t * x, const char * base, char * why, size_t why_size) {
    memset(x->req, 0, sizeof(*x->req));
    snprintf(x->req->url, sizeof(x->req->url), "%s/healthz", base);
    x->req->method = HTTP_METHOD_GET;
    x->req->verify_tls = false;
    x->req->connect_timeout_ms = DW_PROBE_CONNECT_MS;
    x->req->read_timeout_ms = DW_PROBE_TOTAL_MS;
    x->req->total_timeout_ms = DW_PROBE_TOTAL_MS;
    x->req->redirect_limit = 0;
    x->req->max_response_bytes = 256;
    if (!http_request_ex(x->req, x->cancel, x->resp)) {
        snprintf(why, why_size, "%s", dw_why(x->resp->error));
        http_response_free(x->resp);
        return false;
    }
    int status = x->resp->status;
    bool ours = (status == 200 || status == 503) && x->resp->body && x->resp->body_len >= 2 &&
                (strncmp((const char *) x->resp->body, "ok", 2) == 0 ||
                 strncmp((const char *) x->resp->body, "the fetching", 12) == 0);
    if (!ours) snprintf(why, why_size, "answered %d, not Discworld", status);
    http_response_free(x->resp);
    return ours;
}

/* Names did not work (a router that does not pass mDNS on to the player):
 * looks for anything on the home network that takes a connection on 443,
 * 8080 or 80, many at once with a short wait, and writes each as a URL. */
#define DW_SCAN_BATCH 96
#define DW_SCAN_WAIT_MS 1500
#define DW_SCAN_MAX_HOSTS 1024
#define DW_SCAN_MAX_FOUND 12

typedef struct { uint32_t ip; uint16_t port; } dw_hit_t;

static uint64_t dw_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static int dw_scan_batch(const uint32_t * ips, const uint16_t * ports, int n, dw_hit_t * hits, int nhits, int max) {
    int fds[DW_SCAN_BATCH];
    struct pollfd pfd[DW_SCAN_BATCH];
    int open_count = 0;
    for (int i = 0; i < n; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        pfd[i].fd = -1;
        pfd[i].events = POLLOUT;
        pfd[i].revents = 0;
        if (fds[i] < 0) continue;
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(ports[i]), .sin_addr.s_addr = htonl(ips[i]) };
        int rc = connect(fds[i], (struct sockaddr *) &a, sizeof(a));
        if (rc == 0 || errno == EINPROGRESS) { pfd[i].fd = fds[i]; open_count++; }
    }
    uint64_t end = dw_ms() + DW_SCAN_WAIT_MS;
    while (open_count > 0 && nhits < max) {
        uint64_t now = dw_ms();
        if (now >= end) break;
        int ready = poll(pfd, (nfds_t) n, (int) (end - now));
        if (ready <= 0) { if (ready < 0 && errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            if (pfd[i].fd < 0 || !pfd[i].revents) continue;
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(pfd[i].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0 && nhits < max) {
                hits[nhits].ip = ips[i];
                hits[nhits].port = ports[i];
                nhits++;
            }
            pfd[i].fd = -1;
            open_count--;
        }
    }
    for (int i = 0; i < n; i++) if (fds[i] >= 0) close(fds[i]);
    return nhits;
}

static int dw_scan_network(dw_ctx_t * x, char urls[][64], int max, char * self_out, size_t self_size) {
    struct ifaddrs * list = NULL;
    if (getifaddrs(&list) != 0) return 0;
    uint32_t self = 0, mask = 0;
    for (struct ifaddrs * i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t ip = ntohl(((struct sockaddr_in *) i->ifa_addr)->sin_addr.s_addr);
        /* home networks only: 10/8, 172.16/12, 192.168/16 */
        bool home = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
#ifdef HOST_BUILD
        if (getenv("DW_TEST_SCAN_ANY")) home = true; /* the test network is not a home range */
#endif
        if (!home) continue;
        self = ip;
        mask = ntohl(((struct sockaddr_in *) i->ifa_netmask)->sin_addr.s_addr);
        break;
    }
    freeifaddrs(list);
    if (!self) return 0;
    struct in_addr me = { .s_addr = htonl(self) };
    if (self_out) inet_ntop(AF_INET, &me, self_out, (socklen_t) self_size);
    if (~mask + 1U > DW_SCAN_MAX_HOSTS || mask == 0) mask = 0xFFFFFF00U; /* a big network: its /24 */
    uint32_t net = self & mask, count = ~mask + 1U;
    static const uint16_t scan_ports[] = { 443, 8080, 80 };
    dw_hit_t hits[DW_SCAN_MAX_FOUND];
    int nhits = 0;
    uint32_t ips[DW_SCAN_BATCH];
    uint16_t ports[DW_SCAN_BATCH];
    int n = 0;
    for (size_t pi = 0; pi < sizeof(scan_ports) / sizeof(scan_ports[0]); pi++) {
        for (uint32_t h = 1; h + 1 < count && nhits < DW_SCAN_MAX_FOUND; h++) {
            uint32_t ip = net + h;
            bool skip_self = true;
#ifdef HOST_BUILD
            if (getenv("DW_TEST_SCAN_ANY")) skip_self = false; /* the test server runs here */
#endif
            if (ip == self && skip_self) continue;
            ips[n] = ip;
            ports[n] = scan_ports[pi];
            if (++n == DW_SCAN_BATCH) {
                if (dw_stopping(x)) return 0;
                nhits = dw_scan_batch(ips, ports, n, hits, nhits, DW_SCAN_MAX_FOUND);
                n = 0;
            }
        }
    }
    if (n && !dw_stopping(x)) nhits = dw_scan_batch(ips, ports, n, hits, nhits, DW_SCAN_MAX_FOUND);
    int out = 0;
    for (int i = 0; i < nhits && out < max; i++) {
        struct in_addr a = { .s_addr = htonl(hits[i].ip) };
        char dotted[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a, dotted, sizeof(dotted));
        if (hits[i].port == 443) snprintf(urls[out++], 64, "https://%s", dotted);
        else if (hits[i].port == 80) snprintf(urls[out++], 64, "http://%s", dotted);
        else snprintf(urls[out++], 64, "http://%s:%u", dotted, (unsigned) hits[i].port);
    }
    return out;
}

/* Appends one line to the failure detail, as much as fits. */
static void dw_add(discworld_result_t * r, size_t * used, const char * fmt, ...) __attribute__((format(printf, 3, 4)));
static void dw_add(discworld_result_t * r, size_t * used, const char * fmt, ...) {
    if (*used >= sizeof(r->detail) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(r->detail + *used, sizeof(r->detail) - *used, fmt, ap);
    va_end(ap);
    if (w > 0) *used += (size_t) w;
    if (*used >= sizeof(r->detail)) *used = sizeof(r->detail) - 1;
}

static bool dw_discover(dw_ctx_t * x, discworld_result_t * r) {
    char candidates[8][160];
    int n = 0;
    char target[120];
    uint16_t port = 0;
    if (mdns_resolve_srv(DW_SERVICE, 2000, target, sizeof(target), &port) && port) {
        if (port == 80) snprintf(candidates[n++], sizeof(candidates[0]), "http://%s", target);
        else snprintf(candidates[n++], sizeof(candidates[0]), "http://%s:%u", target, (unsigned) port);
    }
    char https_set[160] = "";
    char set_host[128];
    if (dw_url_host(x->config->base_url, set_host, sizeof(set_host)) && strncmp(x->config->base_url, "https://", 8) != 0)
        snprintf(https_set, sizeof(https_set), "https://%s", set_host);
    const char * usual[] = {
        https_set, "https://discworld.local", "https://jarvis.local",
        "http://discworld.local:8080", "http://discworld.local", "http://jarvis.local:8080", "http://jarvis.local",
    };
    for (size_t i = 0; i < sizeof(usual) / sizeof(usual[0]) && n < 8; i++) {
        if (!usual[i][0]) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (strcmp(candidates[k], usual[i]) == 0) dup = true;
        if (!dup && strcmp(usual[i], x->config->base_url) != 0)
            snprintf(candidates[n++], sizeof(candidates[0]), "%s", usual[i]);
    }
    size_t used = strlen(r->detail);
    /* Tries that fail the same way share a line: "a, b, c: why". */
    char last_why[120] = "";
    for (int i = 0; i < n && !dw_stopping(x); i++) {
        char why[120];
        if (dw_probe(x, candidates[i], why, sizeof(why))) {
            snprintf(r->found_url, sizeof(r->found_url), "%s", candidates[i]);
            return true;
        }
        size_t tail = strlen(last_why) + 2;
        if (last_why[0] && !strcmp(why, last_why) && used >= tail) {
            used -= tail; /* take ": why" off, add this one, put it back */
            r->detail[used] = '\0';
            dw_add(r, &used, ", %s: %s", candidates[i], why);
        } else {
            dw_add(r, &used, "\n%s: %s", candidates[i], why);
        }
        snprintf(last_why, sizeof(last_why), "%s", why);
    }
    /* Then anything on the home network that answers like Discworld. */
    char found[DW_SCAN_MAX_FOUND][64];
    char self_ip[INET_ADDRSTRLEN] = "";
    int nf = dw_stopping(x) ? 0 : dw_scan_network(x, found, DW_SCAN_MAX_FOUND, self_ip, sizeof(self_ip));
    if (self_ip[0]) dw_add(r, &used, "\nThis player is at %s", self_ip);
    /* An https server behind a proxy (Caddy) answers only under its name, so
     * each https address is also tried as the usual names, pinned to it. */
    const char * names[3] = { NULL, "discworld.local", "jarvis.local" };
    if (set_host[0] && !dw_is_ip(set_host) && strcasecmp(set_host, "discworld.local") && strcasecmp(set_host, "jarvis.local"))
        names[0] = set_host;
    for (int i = 0; i < nf && !dw_stopping(x); i++) {
        char why[120] = "";
        if (strncmp(found[i], "https://", 8) == 0) {
            const char * ip = found[i] + 8;
            for (int k = 0; k < 3 && !dw_stopping(x); k++) {
                if (!names[k]) continue;
                char url[160];
                snprintf(url, sizeof(url), "https://%s", names[k]);
                http_pin_host(names[k], ip);
                if (dw_probe(x, url, why, sizeof(why))) {
                    snprintf(r->found_url, sizeof(r->found_url), "%s", url);
                    snprintf(r->found_ip, sizeof(r->found_ip), "%s", ip);
                    return true;
                }
                http_pin_host(names[k], NULL);
                if (k == 1) dw_add(r, &used, "\n%s as discworld.local: %s", ip, why);
            }
        }
        if (dw_probe(x, found[i], why, sizeof(why))) {
            snprintf(r->found_url, sizeof(r->found_url), "%s", found[i]);
            return true;
        }
        dw_add(r, &used, "\n%s: %s", found[i], why);
    }
    if (!nf) dw_add(r, &used, "\n%s", "Searched the home network: no web servers answered");
    return false;
}

void discworld_sync_run(const discworld_config_t * config, http_cancel_token_t * cancel,
                        discworld_keep_going_fn keep_going, discworld_progress_fn progress, void * user,
                        discworld_result_t * result) {
    discworld_result_t local;
    discworld_result_t * r = result ? result : &local;
    memset(r, 0, sizeof(*r));
    if (!config || !config->base_url[0] || !config->card_root[0]) {
        r->status = DISCWORLD_ERR_ARGUMENT;
        dw_detail(r, "%s", "No Discworld address set");
        return;
    }
    struct stat sb, parent;
    char up[sizeof(config->card_root) + 4];
    snprintf(up, sizeof(up), "%s/..", config->card_root);
    if (stat(config->card_root, &sb) != 0 || !S_ISDIR(sb.st_mode) ||
        (config->require_mounted && (stat(up, &parent) != 0 || parent.st_dev == sb.st_dev))) {
        r->status = DISCWORLD_ERR_CARD;
        dw_detail(r, "%s", "No SD card");
        return;
    }
    char host[128] = "";
    bool pinned = config->pin_ip[0] && dw_url_host(config->base_url, host, sizeof(host)) && !dw_is_ip(host);
    if (pinned) http_pin_host(host, config->pin_ip);
    dw_ctx_t x = { .config = config, .cancel = cancel, .keep_going = keep_going, .progress_fn = progress,
                   .user = user, .result = r };
    x.req = calloc(1, sizeof(*x.req));
    x.resp = calloc(1, sizeof(*x.resp));
    dw_state_t st = { 0 };
    if (!x.req || !x.resp) r->status = DISCWORLD_ERR_MEMORY;
    else r->status = dw_run(&x, &st);
    if (r->status == DISCWORLD_ERR_CONNECT && x.req && x.resp && !dw_stopping(&x)) {
        /* The set address did not answer: look for Discworld, and sync with it there. */
        char first[sizeof(r->detail)];
        snprintf(first, sizeof(first), "%s: %s", config->base_url, r->detail);
        snprintf(r->detail, sizeof(r->detail), "%s", first);
        discworld_config_t found = *config;
        if (pinned) http_pin_host(host, NULL); /* the kept address may be old: let names try again */
        if (dw_discover(&x, r)) {
            snprintf(found.base_url, sizeof(found.base_url), "%s", r->found_url);
            char keep[sizeof(r->found_url)], keep_ip[sizeof(r->found_ip)];
            snprintf(keep, sizeof(keep), "%s", r->found_url);
            snprintf(keep_ip, sizeof(keep_ip), "%s", r->found_ip);
            dw_state_free(&st);
            memset(&st, 0, sizeof(st));
            memset(r, 0, sizeof(*r));
            snprintf(r->found_url, sizeof(r->found_url), "%s", keep);
            snprintf(r->found_ip, sizeof(r->found_ip), "%s", keep_ip);
            snprintf(found.pin_ip, sizeof(found.pin_ip), "%s", keep_ip);
            x.config = &found;
            x.cookie[0] = '\0';
            r->status = dw_run(&x, &st);
        }
    }
    if (r->status == DISCWORLD_ERR_MEMORY && !r->detail[0]) dw_detail(r, "%s", "Out of memory");
    if (r->status == DISCWORLD_ERR_CARD && !r->detail[0]) dw_detail(r, "%s", "Could not write to the SD card");
    dw_state_free(&st);
    memset(x.cookie, 0, sizeof(x.cookie));
    free(x.req);
    free(x.resp);
}
