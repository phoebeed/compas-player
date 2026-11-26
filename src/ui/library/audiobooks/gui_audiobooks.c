#include "gui_audiobooks.h"

#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../widgets/lists/compact_list.h"
#include "../../player/screen/gui_player.h"
#include "../../player/queue/player_queue_model.h"
#include "../library_thumbnails.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/process/worker_thread.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/metadata/scan/library_walk.h"
#include "../../../library/metadata/scan/library_scan_storage.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/playlists/list_positions.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../../library/artwork/decode/library_artwork_internal.h"
#include "../../../library/artwork/coordinator/artwork_coordinator.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUDIOBOOKS_DIR MUSIC_ROOT_DIR "/" AUDIOBOOKS_LIBRARY_DIR_NAME
#define AB_MAX_BOOKS 500
#define AB_MAX_TRACKS 2000
/* Audiobooks/Author/Series/Book/CD1 is four levels; leave room for one more. */
#define AB_MAX_DEPTH 5
#define AB_NAME_MAX 256
#define AB_COVER_PX ALBUM_THUMBNAIL_PX
#define AB_COVER_CACHE_MAX 300
#define AB_POLL_MS 1000
/* While playing, the place is written at most this often; a pause, a chapter
 * change or the end of the book is written at once. */
#define AB_WRITE_INTERVAL_MS 30000
/* Polls in a row with nothing playing before a book that reached its last
 * chapter counts as finished (chapter changes can briefly stop). */
#define AB_STOPPED_POLLS 3
#define AB_ART_TIMEOUT_MS 5000

/* ---- the shelf -------------------------------------------------------------
 * One scan's books. Immutable once handed to the list, because the list's
 * page fetches run on a worker thread; a new scan makes a new shelf. */

typedef struct {
    char * title;
    char * author;
    char ** tracks;      /* in play order */
    int track_count;
    char subtitle[256];  /* written on the UI thread before the list sees it */
} ab_book_t;

typedef struct {
    ab_book_t * books;
    int count;
    int cap;
    int * cover;         /* UI thread only: index into covers[], or -1 */
} ab_shelf_t;

/* A book's key in list_positions: its first file. */
static const char * book_key(const ab_book_t * b) { return b->tracks[0]; }

static void shelf_free(ab_shelf_t * s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) {
        ab_book_t * b = &s->books[i];
        free(b->title);
        free(b->author);
        for (int t = 0; t < b->track_count; t++) free(b->tracks[t]);
        free(b->tracks);
    }
    free(s->books);
    free(s->cover);
    free(s);
}

/* Case-insensitive compare that orders runs of digits by value, so
 * "Chapter 2" comes before "Chapter 10". */
static int natural_cmp(const char * a, const char * b) {
    while (*a && *b) {
        if (isdigit((unsigned char) *a) && isdigit((unsigned char) *b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char * da = a, * db = b;
            while (isdigit((unsigned char) *a)) a++;
            while (isdigit((unsigned char) *b)) b++;
            size_t la = (size_t) (a - da), lb = (size_t) (b - db);
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(da, db, la);
            if (c) return c;
            continue;
        }
        int ca = tolower((unsigned char) *a), cb = tolower((unsigned char) *b);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++;
        b++;
    }
    return *a ? 1 : (*b ? -1 : 0);
}

static int natural_cmp_p(const void * a, const void * b) {
    return natural_cmp(*(char * const *) a, *(char * const *) b);
}

static int book_cmp(const void * a, const void * b) {
    const ab_book_t * x = a, * y = b;
    int c = natural_cmp(x->title, y->title);
    if (c) return c;
    c = natural_cmp(x->author, y->author);
    return c ? c : strcmp(book_key(x), book_key(y));
}

/* "CD1", "CD 2", "Disc 3", "disk_04", "Part 5": one part of a book. */
static bool is_disc_dir(const char * name) {
    static const char * const words[] = { "cd", "disc", "disk", "part" };
    for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        size_t n = strlen(words[w]);
        if (strncasecmp(name, words[w], n) != 0) continue;
        const char * p = name + n;
        while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
        if (!isdigit((unsigned char) *p)) continue;
        while (isdigit((unsigned char) *p)) p++;
        if (!*p) return true;
    }
    return false;
}

static bool has_ext(const char * name, const char * ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

typedef struct {
    char ** names;
    int count;
    int cap;
} name_list_t;

static bool names_add(name_list_t * l, const char * name) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char ** grown = realloc(l->names, (size_t) cap * sizeof(char *));
        if (!grown) return false;
        l->names = grown;
        l->cap = cap;
    }
    char * copy = strdup(name);
    if (!copy) return false;
    l->names[l->count++] = copy;
    return true;
}

static void names_free(name_list_t * l) {
    for (int i = 0; i < l->count; i++) free(l->names[i]);
    free(l->names);
    memset(l, 0, sizeof(*l));
}

/* The playable files and the subfolders directly in `dir`, each sorted.
 * Hidden entries are left out. */
static void list_dir(const char * dir, name_list_t * files, name_list_t * dirs) {
    DIR * d = opendir(dir);
    if (!d) return;
    struct dirent * e;
    char path[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        bool is_dir = false, is_file = false;
#ifdef DT_DIR
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_REG) is_file = true;
        else
#endif
        {
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (stat(path, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
            is_file = S_ISREG(st.st_mode);
        }
        if (is_dir && dirs) names_add(dirs, e->d_name);
        else if (is_file && files && file_browser_is_playable_name(e->d_name)) names_add(files, e->d_name);
    }
    closedir(d);
    if (files && files->count > 1) qsort(files->names, (size_t) files->count, sizeof(char *), natural_cmp_p);
    if (dirs && dirs->count > 1) qsort(dirs->names, (size_t) dirs->count, sizeof(char *), natural_cmp_p);
}

typedef struct ab_job ab_job_t;
static bool job_cancelled(const ab_job_t * job);

typedef struct {
    ab_shelf_t * shelf;
    ab_job_t * job;
    char comps[AB_MAX_DEPTH][AB_NAME_MAX]; /* folder names below Audiobooks */
} scan_ctx_t;

/* Adds a book; takes ownership of `tracks` either way. */
static void shelf_add(scan_ctx_t * c, const char * title, const char * author, char ** tracks, int count) {
    ab_shelf_t * s = c->shelf;
    if (count <= 0 || s->count >= AB_MAX_BOOKS) goto drop;
    if (s->count == s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        ab_book_t * grown = realloc(s->books, (size_t) cap * sizeof(ab_book_t));
        if (!grown) goto drop;
        s->books = grown;
        s->cap = cap;
    }
    ab_book_t * b = &s->books[s->count];
    memset(b, 0, sizeof(*b));
    b->title = strdup(title);
    b->author = strdup(author ? author : "");
    if (!b->title || !b->author) {
        free(b->title);
        free(b->author);
        goto drop;
    }
    b->tracks = tracks;
    b->track_count = count;
    s->count++;
    return;
drop:
    for (int i = 0; i < count; i++) free(tracks[i]);
    free(tracks);
}

static char * join_path(const char * dir, const char * name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char * p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, name);
    return p;
}

/* Appends dir/name for every name in `files` to tracks. */
static void tracks_append(char *** tracks, int * count, int * cap, const char * dir, const name_list_t * files) {
    for (int i = 0; i < files->count && *count < AB_MAX_TRACKS; i++) {
        if (*count == *cap) {
            int grown_cap = *cap ? *cap * 2 : 16;
            char ** grown = realloc(*tracks, (size_t) grown_cap * sizeof(char *));
            if (!grown) return;
            *tracks = grown;
            *cap = grown_cap;
        }
        char * p = join_path(dir, files->names[i]);
        if (p) (*tracks)[(*count)++] = p;
    }
}

/* A single file as a book: its name without the extension. */
static void add_file_book(scan_ctx_t * c, const char * dir, const char * name, int depth) {
    char ** tracks = malloc(sizeof(char *));
    if (!tracks) return;
    tracks[0] = join_path(dir, name);
    if (!tracks[0]) { free(tracks); return; }
    char title[AB_NAME_MAX];
    snprintf(title, sizeof(title), "%s", name);
    char * dot = strrchr(title, '.');
    if (dot && dot != title) *dot = '\0';
    shelf_add(c, title, depth >= 1 ? c->comps[0] : "", tracks, 1);
}

static void scan_dir(scan_ctx_t * c, const char * dir, int depth) {
    if (job_cancelled(c->job) || c->shelf->count >= AB_MAX_BOOKS) return;
    name_list_t files = { 0 }, dirs = { 0 };
    list_dir(dir, &files, &dirs);

    if (depth == 0) {
        /* Loose files straight in Audiobooks: a book each. */
        for (int i = 0; i < files.count; i++) add_file_book(c, dir, files.names[i], 0);
        for (int i = 0; i < dirs.count && depth + 1 < AB_MAX_DEPTH; i++) {
            snprintf(c->comps[0], AB_NAME_MAX, "%s", dirs.names[i]);
            char * sub = join_path(dir, dirs.names[i]);
            if (sub) scan_dir(c, sub, 1);
            free(sub);
        }
        names_free(&files);
        names_free(&dirs);
        return;
    }

    bool all_m4b = files.count > 1;
    for (int i = 0; i < files.count && all_m4b; i++) all_m4b = has_ext(files.names[i], ".m4b");

    char ** tracks = NULL;
    int count = 0, cap = 0;
    if (all_m4b) {
        /* Author/Book one.m4b, Author/Book two.m4b: a book per file. */
        for (int i = 0; i < files.count; i++) add_file_book(c, dir, files.names[i], depth);
    } else {
        tracks_append(&tracks, &count, &cap, dir, &files);
    }
    /* CD1/CD2 style folders are parts of this folder's book, in order. */
    for (int i = 0; i < dirs.count && !all_m4b; i++) {
        if (!is_disc_dir(dirs.names[i])) continue;
        char * sub = join_path(dir, dirs.names[i]);
        if (!sub) continue;
        name_list_t disc = { 0 };
        list_dir(sub, &disc, NULL);
        tracks_append(&tracks, &count, &cap, sub, &disc);
        names_free(&disc);
        free(sub);
    }
    if (count > 0) {
        /* The folder is the book; the top folder is its author. */
        shelf_add(c, c->comps[depth - 1], depth >= 2 ? c->comps[0] : "", tracks, count);
    } else {
        free(tracks);
    }

    for (int i = 0; i < dirs.count && depth + 1 < AB_MAX_DEPTH; i++) {
        if (count > 0 && is_disc_dir(dirs.names[i])) continue; /* already part of the book */
        snprintf(c->comps[depth], AB_NAME_MAX, "%s", dirs.names[i]);
        char * sub = join_path(dir, dirs.names[i]);
        if (sub) scan_dir(c, sub, depth + 1);
        free(sub);
    }
    names_free(&files);
    names_free(&dirs);
}

/* ---- the worker: scan, then covers ------------------------------------------ */

struct ab_job {
    pthread_t thread;
    atomic_bool cancel;
    atomic_bool scanned;    /* shelf is complete; the UI takes it */
    atomic_bool covers_go;  /* the UI marked which covers it needs */
    atomic_bool done;
    atomic_int ready;       /* covers [0, ready) are decided */
    int applied;
    ab_shelf_t * shelf;
    bool * need;
    uint16_t ** pixels;
};

static bool job_cancelled(const ab_job_t * job) {
    return atomic_load(&((ab_job_t *) job)->cancel);
}

static bool job_cancel_cb(void * user) {
    return atomic_load(&((ab_job_t *) user)->cancel);
}

static uint16_t * cover_decode(ab_job_t * job, const uint8_t * data, uint32_t size) {
    uint16_t * px = NULL;
    if (cover_decode_to_rgb565_ex(data, size, AB_COVER_PX, AB_COVER_PX, ARTWORK_PRIO_THUMBNAIL,
                                  job_cancel_cb, job, &px) != COVER_DECODE_OK) {
        free(px);
        return NULL;
    }
    return px;
}

/* The folder's cover/folder picture (or the parent's, for a CD1 folder),
 * else the picture inside the first file. */
static uint16_t * find_cover(ab_job_t * job, const ab_book_t * b) {
    const char * first = book_key(b);
    albumart_info_t info;
    memset(&info, 0, sizeof(info));
    snprintf(info.path, sizeof(info.path), "%s", first);
    char file[PATH_MAX];
    if (albumart_search_source_files(&info, "", file, sizeof(file))) {
        uint8_t * data = NULL;
        uint32_t size = 0;
        if (albumart_load_file_ex(file, &data, &size, ALBUMART_SIDECAR_MAX_BYTES, ARTWORK_PRIO_THUMBNAIL) ==
            ALBUMART_LOAD_OK) {
            uint16_t * px = cover_decode(job, data, size);
            free(data);
            if (px) return px;
        }
    }
    if (atomic_load(&job->cancel)) return NULL;

    if (artwork_coordinator_acquire(ARTWORK_PRIO_THUMBNAIL, ALBUM_ART_METADATA_START_BYTES, 300,
                                    job_cancel_cb, job) != ARTWORK_ACQUIRE_OK)
        return NULL;
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_read_artwork_isolated(first, &meta, AB_ART_TIMEOUT_MS, ARTWORK_PRIO_THUMBNAIL);
    artwork_coordinator_release(ARTWORK_PRIO_THUMBNAIL);
    free(meta.lyrics);
    uint16_t * px = NULL;
    if (meta.picture_data && meta.picture_size && !atomic_load(&job->cancel))
        px = cover_decode(job, meta.picture_data, meta.picture_size);
    free(meta.picture_data);
    return px;
}

static void * job_thread(void * arg) {
    ab_job_t * job = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    scan_ctx_t * c = calloc(1, sizeof(*c));
    if (c) {
        c->shelf = job->shelf;
        c->job = job;
        scan_dir(c, AUDIOBOOKS_DIR, 0);
        free(c);
    }
    ab_shelf_t * s = job->shelf;
    if (s->count > 1) qsort(s->books, (size_t) s->count, sizeof(ab_book_t), book_cmp);
    job->need = calloc((size_t) (s->count ? s->count : 1), sizeof(bool));
    job->pixels = calloc((size_t) (s->count ? s->count : 1), sizeof(uint16_t *));
    atomic_store(&job->scanned, true);

    while (!atomic_load(&job->covers_go) && !atomic_load(&job->cancel)) usleep(20000);
    for (int i = 0; i < s->count && job->need && job->pixels && !atomic_load(&job->cancel); i++) {
        if (job->need[i]) job->pixels[i] = find_cover(job, &s->books[i]);
        atomic_store(&job->ready, i + 1);
    }
    atomic_store(&job->done, true);
    return NULL;
}

/* ---- cover cache: decoded covers by book, kept while compás runs (a UI
 * reload reuses them) ---------------------------------------------------- */

typedef struct {
    char * key;
    uint16_t * pixels;   /* NULL: the book has no cover */
    lv_image_dsc_t dsc;
} ab_cover_t;

static ab_cover_t covers[AB_COVER_CACHE_MAX];
static int cover_count;

static int cover_find(const char * key) {
    for (int i = 0; i < cover_count; i++)
        if (strcmp(covers[i].key, key) == 0) return i;
    return -1;
}

static int cover_store(const char * key, uint16_t * pixels) {
    if (cover_count >= AB_COVER_CACHE_MAX) { free(pixels); return -1; }
    ab_cover_t * c = &covers[cover_count];
    c->key = strdup(key);
    if (!c->key) { free(pixels); return -1; }
    c->pixels = pixels;
    memset(&c->dsc, 0, sizeof(c->dsc));
    if (pixels) {
        c->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        c->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        c->dsc.header.w = AB_COVER_PX;
        c->dsc.header.h = AB_COVER_PX;
        c->dsc.header.stride = AB_COVER_PX * 2;
        c->dsc.data_size = AB_COVER_PX * AB_COVER_PX * 2;
        c->dsc.data = (const uint8_t *) pixels;
    }
    return cover_count++;
}

/* ---- screen ------------------------------------------------------------------ */

static lv_obj_t * screen;
static lv_obj_t * list;
static ab_shelf_t * shelf;          /* what the list shows */
static ab_job_t * job;
/* Shelves the list stopped showing, freed once no page fetch can still read them. */
#define AB_RETIRED_MAX 4
static ab_shelf_t * retired[AB_RETIRED_MAX];

static void job_free(ab_job_t * j) {
    if (!j) return;
    atomic_store(&j->cancel, true);
    atomic_store(&j->covers_go, true);
    pthread_join(j->thread, NULL);
    if (j->pixels && j->shelf)
        for (int i = 0; i < j->shelf->count; i++) free(j->pixels[i]);
    free(j->pixels);
    free(j->need);
    /* A shelf the UI never took is the job's own. */
    if (j->shelf && j->shelf != shelf) {
        bool held = false;
        for (int i = 0; i < AB_RETIRED_MAX; i++) held |= retired[i] == j->shelf;
        if (!held) shelf_free(j->shelf);
    }
    free(j);
}

static void retired_sweep(bool force) {
    if (!force && compact_list_any_fetch_pending()) return;
    for (int i = 0; i < AB_RETIRED_MAX; i++) {
        if (!retired[i] || (job && job->shelf == retired[i])) continue;
        shelf_free(retired[i]);
        retired[i] = NULL;
    }
}

static void retire(ab_shelf_t * s) {
    if (!s) return;
    for (int i = 0; i < AB_RETIRED_MAX; i++) if (!retired[i]) { retired[i] = s; return; }
    /* Full: a fetch has been pending through four rescans; keep the newest. */
    retired_sweep(true);
    retired[0] = s;
}

static void format_time(double seconds, char * out, size_t size) {
    long s = seconds > 0 && isfinite(seconds) ? (long) seconds : 0;
    if (s >= 3600) snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

/* Author, then how far in: "Terry Pratchett · Chapter 3 of 12, 4:05". */
static void fill_subtitle(ab_book_t * b) {
    char progress[128] = "";
    list_position_t saved;
    if (list_positions_get(book_key(b), &saved)) {
        char time[32];
        format_time(saved.position, time, sizeof(time));
        if (saved.track_index < 0) snprintf(progress, sizeof(progress), "%s", TR("Finished"));
        else if (b->track_count > 1)
            snprintf(progress, sizeof(progress), TR("Chapter %d of %d, %s"), saved.track_index + 1, b->track_count, time);
        else snprintf(progress, sizeof(progress), TR("%s in"), time);
    } else if (b->track_count > 1) {
        snprintf(progress, sizeof(progress), TR("%d chapters"), b->track_count);
    }
    if (b->author[0] && progress[0]) snprintf(b->subtitle, sizeof(b->subtitle), "%s · %s", b->author, progress);
    else snprintf(b->subtitle, sizeof(b->subtitle), "%s", b->author[0] ? b->author : progress);
}

/* Runs on the list's fetch worker: reads only the immutable shelf. */
static int fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    const ab_shelf_t * s = ctx;
    int n = 0;
    for (int i = offset; s && i < s->count && n < count; i++, n++) {
        compact_list_page_row_t * r = &out_rows[n];
        memset(r, 0, sizeof(*r));
        snprintf(r->label, sizeof(r->label), "%s", s->books[i].title);
        snprintf(r->subtitle, sizeof(r->subtitle), "%s", s->books[i].subtitle);
        r->identity = i;
        r->artwork_key = (uint64_t) i + 1;
    }
    return n;
}

static void row_decorator(lv_obj_t * l, lv_obj_t * row, lv_obj_t * image, int logical_index, int pool_slot,
                          int64_t identity, uint64_t artwork_key, const char * artwork_name, void * ctx) {
    (void) pool_slot; (void) identity; (void) artwork_key; (void) artwork_name; (void) ctx;
    const lv_image_dsc_t * cover = NULL;
    if (shelf && shelf->cover && logical_index >= 0 && logical_index < shelf->count) {
        int c = shelf->cover[logical_index];
        if (c >= 0 && covers[c].pixels) cover = &covers[c].dsc;
    }
    library_thumbnails_show_row_image(l, row, image, cover);
}

/* ---- playing and saving the place -------------------------------------------- */

static struct {
    bool on;
    char key[LIST_POSITIONS_PATH_MAX];
    int count;
    char last[PATH_MAX];
    int index;
    bool paused;
    uint32_t last_write;
    int stopped_polls;
    bool dirty;
} tracker;
static uint64_t adopt_checked_revision;

static void tracker_write(void) {
    if (!tracker.dirty) return;
    if (!list_positions_save()) fprintf(stderr, "[audiobooks] could not save book positions\n");
    tracker.dirty = false;
    tracker.last_write = lv_tick_get();
}

static bool queue_matches_tracker(void) {
    int n = player_queue_model_count();
    return n == tracker.count && n > 0 &&
           strcmp(playlist_path_at(0), tracker.key) == 0 &&
           strcmp(playlist_path_at(n - 1), tracker.last) == 0;
}

static void tracker_start(char ** tracks, int count) {
    memset(&tracker, 0, sizeof(tracker));
    tracker.on = true;
    snprintf(tracker.key, sizeof(tracker.key), "%s", tracks[0]);
    snprintf(tracker.last, sizeof(tracker.last), "%s", tracks[count - 1]);
    tracker.count = count;
    tracker.index = -1;
    tracker.last_write = lv_tick_get();
}

static void tracker_record(bool force_write) {
    int current = player_queue_model_current();
    if (current < 0) return;
    bool paused = audio_is_paused();
    if (!audio_is_playing() && !paused) return;
    bool changed = current != tracker.index || paused != tracker.paused;
    tracker.index = current;
    tracker.paused = paused;
    if (list_positions_set(tracker.key, current, playlist_path_at(current), audio_get_resume_position_seconds()))
        tracker.dirty = true;
    if (force_write || changed || lv_tick_elaps(tracker.last_write) >= AB_WRITE_INTERVAL_MS) tracker_write();
}

static bool under_audiobooks(const char * path) {
    size_t n = strlen(AUDIOBOOKS_DIR);
    return strncmp(path, AUDIOBOOKS_DIR, n) == 0 && path[n] == '/';
}

/* After a reboot the restored queue may be a book that was playing: keep
 * saving into it. Its key is its first file, so the queue alone says which. */
static void try_adopt_restored_queue(void) {
    int n = player_queue_model_count();
    if (n <= 0) return;
    uint64_t revision = gui_player_queue_revision();
    if (revision == adopt_checked_revision) return;
    adopt_checked_revision = revision;
    const char * first = playlist_path_at(0);
    list_position_t saved;
    if (!under_audiobooks(first) || !list_positions_get(first, &saved)) return;
    memset(&tracker, 0, sizeof(tracker));
    tracker.on = true;
    snprintf(tracker.key, sizeof(tracker.key), "%s", first);
    snprintf(tracker.last, sizeof(tracker.last), "%s", playlist_path_at(n - 1));
    tracker.count = n;
    tracker.index = -1;
    tracker.last_write = lv_tick_get();
}

static void show_now_playing(void) {
    if (!list || !shelf) return;
    int at = -1;
    for (int i = 0; tracker.on && i < shelf->count && at < 0; i++)
        if (strcmp(book_key(&shelf->books[i]), tracker.key) == 0) at = i;
    compact_list_set_now_playing(list, at);
}

static void play_book(int index, bool from_start) {
    if (!shelf || index < 0 || index >= shelf->count) return;
    const ab_book_t * b = &shelf->books[index];
    if (access(book_key(b), R_OK) != 0) {
        show_error_toast(TR("This book is no longer on the SD card"));
        return;
    }
    char ** tracks = calloc((size_t) b->track_count, sizeof(char *));
    if (!tracks) return;
    for (int t = 0; t < b->track_count; t++) {
        tracks[t] = strdup(b->tracks[t]);
        if (!tracks[t]) {
            for (int k = 0; k < t; k++) free(tracks[k]);
            free(tracks);
            return;
        }
    }
    int count = b->track_count;

    int start = 0;
    double position = 0;
    list_position_t saved;
    if (!from_start && list_positions_get(tracks[0], &saved) && saved.track_index >= 0) {
        start = -1;
        if (saved.track_index < count && strcmp(tracks[saved.track_index], saved.track_path) == 0)
            start = saved.track_index;
        /* Chapters were added or removed: find the one by its path. */
        for (int k = 0; k < count && start < 0; k++)
            if (strcmp(tracks[k], saved.track_path) == 0) start = k;
        if (start >= 0) position = saved.position;
        else start = 0;
    }

    if (tracker.on) {
        tracker_record(true);
        tracker.on = false;
    }
    clear_player_source();
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
    tracker_start(tracks, count);
    if (list_positions_set(tracks[0], start, tracks[start], position)) tracker.dirty = true;
    tracker.index = start;
    tracker_write();
    /* Takes ownership of tracks, and opens the player screen. */
    on_file_selected_at(tracks, count, start, position);
    adopt_checked_revision = gui_player_queue_revision();
    if (!queue_matches_tracker()) tracker.on = false; /* playback was refused */
    show_now_playing();
}

static void row_click_cb(int index) { play_book(index, false); }
static void row_long_press_cb(int index) { play_book(index, true); }

/* ---- scanning and showing ------------------------------------------------------ */

static void start_scan(void) {
    job_free(job);
    job = NULL;
    ab_job_t * j = calloc(1, sizeof(*j));
    if (!j) return;
    j->shelf = calloc(1, sizeof(ab_shelf_t));
    if (!j->shelf) { free(j); return; }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&j->thread, &attr, job_thread, j) == 0) {
        job = j;
    } else {
        free(j->shelf);
        free(j);
    }
    pthread_attr_destroy(&attr);
}

/* The scan finished: show its books, using covers already decoded. */
static void publish_shelf(ab_job_t * j) {
    ab_shelf_t * s = j->shelf;
    s->cover = calloc((size_t) (s->count ? s->count : 1), sizeof(int));
    for (int i = 0; i < s->count; i++) {
        fill_subtitle(&s->books[i]);
        int c = cover_find(book_key(&s->books[i]));
        if (s->cover) s->cover[i] = c;
        if (j->need) j->need[i] = c < 0;
    }
    atomic_store(&j->covers_go, true);

    ab_shelf_t * old = shelf;
    shelf = s;
    if (list) {
        compact_list_set_empty_heading(list, s->count ? NULL : TR("No audiobooks"));
        compact_list_set_paged_provider(list, fetch_page, s, s->count);
    }
    retire(old);
    show_now_playing();
}

static void apply_covers(ab_job_t * j) {
    int ready = atomic_load(&j->ready);
    bool any = false;
    for (; j->applied < ready; j->applied++) {
        int i = j->applied;
        if (!j->need || !j->need[i]) continue;
        int c = cover_store(book_key(&j->shelf->books[i]), j->pixels[i]);
        j->pixels[i] = NULL;
        if (j->shelf == shelf && shelf->cover) shelf->cover[i] = c;
        any |= c >= 0 && covers[c].pixels;
    }
    if (any && list && j->shelf == shelf) compact_list_refresh_decorations(list);
}

void gui_audiobooks_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !screen) return;
    if (!sd_card_root_is_mounted()) {
        show_error_toast(TR("SD card not available"));
        return;
    }
    start_scan();
    /* The last shelf shows until the new scan lands, which is quick. */
    nav_push(screen);
}

void gui_audiobooks_poll(void) {
    if (job) {
        if (atomic_load(&job->scanned) && !atomic_load(&job->covers_go)) publish_shelf(job);
        if (atomic_load(&job->covers_go)) apply_covers(job);
        if (atomic_load(&job->done) && job->applied >= atomic_load(&job->ready)) {
            job_free(job);
            job = NULL;
        }
    }
    retired_sweep(false);

    static uint32_t last_poll;
    if (lv_tick_elaps(last_poll) < AB_POLL_MS) return;
    last_poll = lv_tick_get();

    if (!tracker.on) {
        try_adopt_restored_queue();
        if (!tracker.on) return;
    }
    /* Something else is playing now: stop saving into the book. */
    if (!queue_matches_tracker()) {
        tracker_write();
        tracker.on = false;
        show_now_playing();
        return;
    }
    if (audio_is_playing() || audio_is_paused()) {
        tracker.stopped_polls = 0;
        tracker_record(false);
        return;
    }
    if (deferred_resume_pending) return; /* loaded at boot, not yet started */
    if (++tracker.stopped_polls < AB_STOPPED_POLLS) return;
    if (tracker.index == tracker.count - 1) {
        /* Played to the end: the next start begins the book again. */
        if (list_positions_set(tracker.key, -1, "", 0)) tracker.dirty = true;
        tracker_write();
        tracker.on = false;
        show_now_playing();
    }
}

void gui_audiobooks_checkpoint(void) {
    if (tracker.on) tracker_record(true);
}

bool gui_audiobooks_init(void) {
    list_positions_load();
    screen = build_compact_list_screen(TR("Audiobooks"), generic_back_cb, NULL, 0, row_click_cb, row_long_press_cb,
                                       &list, NULL, LIST_ROW_WIDTH_WIDE, true, gui_theme_accent_style());
    if (!screen) return false;
    compact_list_set_row_height(list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_row_decorator(list, row_decorator, NULL);
    compact_list_set_empty_heading(list, TR("Loading audiobooks"));
    if (shelf) {
        /* A UI reload keeps the books it already found. */
        compact_list_set_empty_heading(list, shelf->count ? NULL : TR("No audiobooks"));
        compact_list_set_paged_provider(list, fetch_page, shelf, shelf->count);
    }
    finalize_screen_navigation(screen);
    return true;
}

void gui_audiobooks_teardown(void) {
    if (tracker.on) tracker_record(true);
    job_free(job);
    job = NULL;
    if (screen) lv_obj_delete(screen);
    screen = NULL;
    list = NULL;
    retired_sweep(true);
    /* The covers and the shelf stay for gui_audiobooks_init() to reuse. */
}
