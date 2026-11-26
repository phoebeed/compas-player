#include "list_positions.h"
#include "../../core/persistence/storage_paths.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_POSITIONS_FILE INTERNAL_COMPAS_DIR "/list_positions.txt"
#define LIST_POSITIONS_TMP LIST_POSITIONS_FILE ".tmp"

static list_position_t entries[LIST_POSITIONS_MAX];
static int entry_count;
static char active_list[LIST_POSITIONS_PATH_MAX];
static bool loaded;

static bool path_storable(const char * path) {
    if (!path || !path[0] || strlen(path) >= LIST_POSITIONS_PATH_MAX) return false;
    return strpbrk(path, "\t\r\n") == NULL;
}

static int find_entry(const char * list_path) {
    for (int i = 0; i < entry_count; i++)
        if (strcmp(entries[i].list_path, list_path) == 0) return i;
    return -1;
}

/* Line format, tab separated:
 *   A <list path>                                   the active list
 *   P <list path> <track index> <seconds> <track path>  one saved list */
static void parse_line(char * line) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (line[0] == 'A' && line[1] == '\t') {
        if (path_storable(line + 2)) snprintf(active_list, sizeof(active_list), "%s", line + 2);
        return;
    }
    if (line[0] != 'P' || line[1] != '\t' || entry_count >= LIST_POSITIONS_MAX) return;
    char * fields[4];
    char * cursor = line + 2;
    for (int i = 0; i < 3; i++) {
        fields[i] = cursor;
        char * tab = strchr(cursor, '\t');
        if (!tab) return;
        *tab = '\0';
        cursor = tab + 1;
    }
    fields[3] = cursor;
    if (!path_storable(fields[0])) return;
    if (fields[3][0] && !path_storable(fields[3])) return;
    char * end = NULL;
    long index = strtol(fields[1], &end, 10);
    if (!end || *end || index < -1 || index > 1000000) return;
    double position = strtod(fields[2], &end);
    if (!end || *end || !isfinite(position) || position < 0) position = 0;
    if (find_entry(fields[0]) >= 0) return;
    list_position_t * e = &entries[entry_count++];
    snprintf(e->list_path, sizeof(e->list_path), "%s", fields[0]);
    snprintf(e->track_path, sizeof(e->track_path), "%s", fields[3]);
    e->track_index = (int) index;
    e->position = position;
}

void list_positions_load(void) {
    if (loaded) return;
    loaded = true;
    FILE * f = fopen(LIST_POSITIONS_FILE, "r");
    if (!f) return;
    char line[LIST_POSITIONS_PATH_MAX * 2 + 64];
    while (fgets(line, sizeof(line), f)) parse_line(line);
    fclose(f);
}

bool list_positions_get(const char * list_path, list_position_t * out) {
    list_positions_load();
    int i = list_path ? find_entry(list_path) : -1;
    if (i < 0) return false;
    if (out) *out = entries[i];
    return true;
}

bool list_positions_set(const char * list_path, int track_index, const char * track_path, double position) {
    list_positions_load();
    if (!path_storable(list_path)) return false;
    if (!track_path || !path_storable(track_path)) track_path = "";
    if (!isfinite(position) || position < 0) position = 0;
    position = floor(position * 10.0) / 10.0;

    int i = find_entry(list_path);
    if (i == 0 && entries[0].track_index == track_index && entries[0].position == position &&
        strcmp(entries[0].track_path, track_path) == 0)
        return false;

    list_position_t e;
    snprintf(e.list_path, sizeof(e.list_path), "%s", list_path);
    snprintf(e.track_path, sizeof(e.track_path), "%s", track_path);
    e.track_index = track_index;
    e.position = position;

    int shift = i >= 0 ? i : (entry_count < LIST_POSITIONS_MAX ? entry_count : LIST_POSITIONS_MAX - 1);
    memmove(&entries[1], &entries[0], (size_t) shift * sizeof(entries[0]));
    entries[0] = e;
    if (i < 0 && entry_count < LIST_POSITIONS_MAX) entry_count++;
    return true;
}

const char * list_positions_active(void) {
    list_positions_load();
    return active_list;
}

void list_positions_set_active(const char * list_path) {
    list_positions_load();
    if (!list_path || !path_storable(list_path)) list_path = "";
    snprintf(active_list, sizeof(active_list), "%s", list_path);
}

bool list_positions_save(void) {
    list_positions_load();
    mkdir(INTERNAL_COMPAS_DIR, 0755);
    FILE * f = fopen(LIST_POSITIONS_TMP, "w");
    if (!f) return false;
    bool ok = true;
    if (active_list[0]) ok = fprintf(f, "A\t%s\n", active_list) > 0;
    for (int i = 0; ok && i < entry_count; i++) {
        const list_position_t * e = &entries[i];
        ok = fprintf(f, "P\t%s\t%d\t%.1f\t%s\n", e->list_path, e->track_index, e->position, e->track_path) > 0;
    }
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(LIST_POSITIONS_TMP, LIST_POSITIONS_FILE) != 0) {
        unlink(LIST_POSITIONS_TMP);
        return false;
    }
    return true;
}
