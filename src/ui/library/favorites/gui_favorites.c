#include "gui_favorites.h"

#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../player/screen/gui_player.h"
#include "../../player/queue/player_queue_model.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/metadata/scan/library_scan_storage.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "../../../library/playlists/list_positions.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../../library/artwork/albumart/albumart_info.h"
#include "../../../library/artwork/cache/image_thumb.h"
#include "../../../library/artwork/decode/library_artwork_internal.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../network/services/discworld_sync.h"
#include "../../../core/process/worker_thread.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define FAVORITES_PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
/* Either spelling of the favourites folder inside Playlists. */
static const char * const favorites_dirs[] = {
    FAVORITES_PLAYLISTS_DIR "/Favorites",
    FAVORITES_PLAYLISTS_DIR "/Favourites",
};

#define FAVORITES_ROW_HEIGHT BOARD_SCALE_PX(120)
#define FAVORITES_PERSON_ROW_HEIGHT BOARD_SCALE_PX(150)
#define FAVORITES_COVER_PX ALBUM_THUMBNAIL_PX
#define FAVORITES_MAX_PEOPLE 8
#define FAVORITES_POLL_MS 1000
/* While playing, the place is written at most this often; a pause, a track
 * change or the end of the list is written at once. */
#define FAVORITES_WRITE_INTERVAL_MS 30000
/* Polls in a row with nothing playing or paused before a list that reached
 * its last track counts as finished (track changes can briefly stop). */
#define FAVORITES_STOPPED_POLLS 3

/* One list screen: the top one (people, then loose playlists) or one
 * person's playlists. Each playlist row has a cover, filled in by a worker. */
typedef struct {
    lv_obj_t * screen;
    lv_obj_t * title;
    lv_obj_t * list;
    char ** paths;
    int count;
    lv_obj_t ** images;     /* each playlist row's cover image */
    uint16_t ** pixels;     /* shown covers, owned */
    lv_image_dsc_t * dscs;
    struct cover_job * job;
} fav_view_t;

enum { VIEW_TOP, VIEW_PERSON, VIEW_COUNT };
static fav_view_t views[VIEW_COUNT];
static char people[FAVORITES_MAX_PEOPLE][64];
static int people_count;

static void apply_covers(fav_view_t * v);

/* The favourite list the player queue is currently playing, if any. */
static struct {
    bool on;
    char list[PATH_MAX];
    int count;
    char first[PATH_MAX];
    char last[PATH_MAX];
    int index;
    double position;
    bool paused;
    uint32_t last_write;
    int stopped_polls;
    bool dirty;
} tracker;

/* Boot: the restored queue is matched against the list that was playing at
 * most once per queue revision. */
static uint64_t adopt_checked_revision;

static void format_time(double seconds, char * out, size_t size) {
    long s = seconds > 0 && isfinite(seconds) ? (long) seconds : 0;
    if (s >= 3600) snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}

/* Display name: the file name without its .m3u/.m3u8 extension. */
static void list_display_name(const char * path, char * out, size_t size) {
    snprintf(out, size, "%s", basename_of(path));
    char * dot = strrchr(out, '.');
    if (dot && dot != out && (strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)) *dot = '\0';
}

static void tracker_write(void) {
    if (!tracker.dirty) return;
    if (!list_positions_save()) fprintf(stderr, "[favorites] could not save list positions\n");
    tracker.dirty = false;
    tracker.last_write = lv_tick_get();
}

static void tracker_stop(void) {
    if (!tracker.on) return;
    tracker.on = false;
    list_positions_set_active("");
    tracker.dirty = true;
    tracker_write();
}

static bool queue_matches_tracker(void) {
    int n = player_queue_model_count();
    return n == tracker.count && n > 0 &&
           strcmp(playlist_path_at(0), tracker.first) == 0 &&
           strcmp(playlist_path_at(n - 1), tracker.last) == 0;
}

static void tracker_start(const char * list, char ** songs, int count) {
    memset(&tracker, 0, sizeof(tracker));
    tracker.on = true;
    snprintf(tracker.list, sizeof(tracker.list), "%s", list);
    tracker.count = count;
    snprintf(tracker.first, sizeof(tracker.first), "%s", songs[0]);
    snprintf(tracker.last, sizeof(tracker.last), "%s", songs[count - 1]);
    tracker.index = -1;
    tracker.last_write = lv_tick_get();
    list_positions_set_active(list);
    tracker.dirty = true;
}

/* Reads the current place into memory and writes it when it is due. */
static void tracker_record(bool force_write) {
    int current = player_queue_model_current();
    if (current < 0) return;
    bool playing = audio_is_playing();
    bool paused = audio_is_paused();
    if (!playing && !paused) return;
    double position = audio_get_resume_position_seconds();
    bool track_changed = current != tracker.index;
    bool pause_changed = paused != tracker.paused;
    tracker.index = current;
    tracker.position = position;
    tracker.paused = paused;
    if (list_positions_set(tracker.list, current, playlist_path_at(current), position)) tracker.dirty = true;
    if (force_write || track_changed || pause_changed ||
        lv_tick_elaps(tracker.last_write) >= FAVORITES_WRITE_INTERVAL_MS)
        tracker_write();
}

/* After a reboot the restored queue is the list that was playing: keep
 * saving into it. Reads that playlist once per queue revision. */
static void try_adopt_restored_queue(void) {
    const char * active = list_positions_active();
    if (!active[0] || player_queue_model_count() <= 0) return;
    uint64_t revision = gui_player_queue_revision();
    if (revision == adopt_checked_revision) return;
    adopt_checked_revision = revision;
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(active, &songs, &count) || count <= 0) {
        for (int k = 0; k < count; k++) free(songs[k]);
        free(songs);
        return;
    }
    char list[PATH_MAX];
    snprintf(list, sizeof(list), "%s", active);
    tracker_start(list, songs, count);
    tracker.dirty = false;
    if (!queue_matches_tracker()) tracker.on = false;
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
}

void gui_favorites_poll(void) {
    for (int i = 0; i < VIEW_COUNT; i++) apply_covers(&views[i]);
    static uint32_t last_poll;
    if (lv_tick_elaps(last_poll) < FAVORITES_POLL_MS) return;
    last_poll = lv_tick_get();

    if (!tracker.on) {
        try_adopt_restored_queue();
        if (!tracker.on) return;
    }

    /* Another queue (a different length or ends) stops the list's saving;
     * shuffle and playing from the queue screen do not. */
    if (!queue_matches_tracker()) {
        tracker_stop();
        return;
    }

    if (audio_is_playing() || audio_is_paused()) {
        tracker.stopped_polls = 0;
        tracker_record(false);
        return;
    }
    if (deferred_resume_pending) return; /* loaded at boot, not yet started */
    if (++tracker.stopped_polls < FAVORITES_STOPPED_POLLS) return;
    if (tracker.index == tracker.count - 1) {
        /* Played to the end: the next start begins the list again. */
        if (list_positions_set(tracker.list, -1, "", 0)) tracker.dirty = true;
        tracker_stop();
    }
}

void gui_favorites_checkpoint(void) {
    if (tracker.on) tracker_record(true);
}

/* ---- covers ---------------------------------------------------------------
 * A playlist's own picture ("<name>.jpg" or .jpeg/.png beside it), else the
 * album cover of its first song. Decoded on a worker, shown as they land. */

typedef struct cover_job {
    pthread_t thread;
    atomic_bool cancel;
    atomic_bool done;
    atomic_int ready;        /* covers [0, ready) are decided */
    int applied;
    int count;
    char ** paths;
    uint16_t ** pixels;
    int * w;
    int * h;
} cover_job_t;

static bool cover_cancel_cb(void * user) {
    return atomic_load(&((cover_job_t *) user)->cancel);
}

static bool cover_from_file(cover_job_t * job, const char * file, int i) {
    uint8_t * data = NULL;
    uint32_t size = 0;
    if (albumart_load_file_ex(file, &data, &size, ALBUMART_SIDECAR_MAX_BYTES, ARTWORK_PRIO_THUMBNAIL) !=
        ALBUMART_LOAD_OK) return false;
    bool ok = image_thumb_decode(data, size, FAVORITES_COVER_PX, FAVORITES_COVER_PX, ARTWORK_PRIO_THUMBNAIL,
                                 cover_cancel_cb, job, &job->pixels[i], &job->w[i], &job->h[i], NULL);
    free(data);
    return ok && job->pixels[i];
}

static void cover_find(cover_job_t * job, int i) {
    const char * list = job->paths[i];
    /* The playlist's own picture. */
    static const char * const exts[] = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG" };
    char stem[PATH_MAX], file[PATH_MAX + 8];
    snprintf(stem, sizeof(stem), "%s", list);
    char * dot = strrchr(stem, '.');
    char * slash = strrchr(stem, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        snprintf(file, sizeof(file), "%s%s", stem, exts[e]);
        if (access(file, R_OK) == 0 && cover_from_file(job, file, i)) return;
        if (atomic_load(&job->cancel)) return;
    }
    /* Else the first song's album cover. */
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(list, &songs, &count) || count <= 0) goto out;
    song_row_t row;
    if (metadata_db_get_song_by_path(songs[0], &row)) {
        uint16_t * px = NULL;
        if (album_thumbnail_load_or_decode_ex(&row, ARTWORK_PRIO_THUMBNAIL, cover_cancel_cb, job, &px) && px) {
            job->pixels[i] = px;
            job->w[i] = job->h[i] = ALBUM_THUMBNAIL_PX;
            goto out;
        }
        free(px);
    }
    /* Not in the library yet (just synced): its folder's cover file. */
    if (!atomic_load(&job->cancel)) {
        albumart_info_t info;
        albumart_info_from_path_tags(songs[0], "", "", "", &info);
        if (albumart_search_source_files(&info, "", file, sizeof(file))) cover_from_file(job, file, i);
    }
out:
    for (int k = 0; k < count; k++) free(songs[k]);
    free(songs);
}

static void * cover_thread(void * arg) {
    cover_job_t * job = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    for (int i = 0; i < job->count && !atomic_load(&job->cancel); i++) {
        cover_find(job, i);
        atomic_store(&job->ready, i + 1);
    }
    atomic_store(&job->done, true);
    return NULL;
}

static void cover_job_free(cover_job_t * job) {
    if (!job) return;
    atomic_store(&job->cancel, true);
    pthread_join(job->thread, NULL);
    for (int i = 0; i < job->count; i++) { free(job->paths[i]); free(job->pixels[i]); }
    free(job->paths);
    free(job->pixels);
    free(job->w);
    free(job->h);
    free(job);
}

static void cover_job_start(fav_view_t * v) {
    if (!v->count) return;
    cover_job_t * job = calloc(1, sizeof(*job));
    if (!job) return;
    job->count = v->count;
    job->paths = calloc((size_t) v->count, sizeof(char *));
    job->pixels = calloc((size_t) v->count, sizeof(uint16_t *));
    job->w = calloc((size_t) v->count, sizeof(int));
    job->h = calloc((size_t) v->count, sizeof(int));
    bool ok = job->paths && job->pixels && job->w && job->h;
    for (int i = 0; ok && i < v->count; i++) ok = (job->paths[i] = strdup(v->paths[i])) != NULL;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (ok && pthread_create(&job->thread, &attr, cover_thread, job) == 0) {
        v->job = job;
    } else {
        for (int i = 0; i < job->count; i++) if (job->paths) free(job->paths[i]);
        free(job->paths); free(job->pixels); free(job->w); free(job->h); free(job);
    }
    pthread_attr_destroy(&attr);
}

static void apply_covers(fav_view_t * v) {
    cover_job_t * job = v->job;
    if (!job) return;
    int ready = atomic_load(&job->ready);
    for (; job->applied < ready; job->applied++) {
        int i = job->applied;
        if (!job->pixels[i] || !v->images || !v->images[i]) continue;
        v->pixels[i] = job->pixels[i];
        job->pixels[i] = NULL;
        lv_image_dsc_t * d = &v->dscs[i];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = (uint32_t) job->w[i];
        d->header.h = (uint32_t) job->h[i];
        d->header.stride = (uint32_t) job->w[i] * 2;
        d->data_size = (uint32_t) job->w[i] * (uint32_t) job->h[i] * 2;
        d->data = (const uint8_t *) v->pixels[i];
        lv_image_set_src(v->images[i], d);
    }
    if (atomic_load(&job->done) && job->applied >= job->count) {
        cover_job_free(job);
        v->job = NULL;
    }
}

/* Drops a view's rows' data (the rows themselves go with lv_obj_clean). */
static void view_clear(fav_view_t * v) {
    cover_job_free(v->job);
    v->job = NULL;
    for (int i = 0; i < v->count; i++) {
        if (v->images && v->images[i]) lv_image_set_src(v->images[i], NULL);
        if (v->pixels && v->pixels[i]) {
            lv_image_cache_drop(&v->dscs[i]);
            free(v->pixels[i]);
        }
        free(v->paths[i]);
    }
    free(v->paths);
    free(v->images);
    free(v->pixels);
    free(v->dscs);
    v->paths = NULL;
    v->images = NULL;
    v->pixels = NULL;
    v->dscs = NULL;
    v->count = 0;
    if (v->list) lv_obj_clean(v->list);
}

/* ---- playing --------------------------------------------------------------- */

/* Starts a list: from its saved place, or from the top. */
static void play_favorite(const char * path, bool from_start) {
    char list[PATH_MAX];
    snprintf(list, sizeof(list), "%s", path);
    char ** songs = NULL;
    int count = 0;
    bool read = playlist_files_read(list, &songs, &count);
    if (!read || count <= 0) {
        for (int k = 0; k < count; k++) free(songs[k]);
        free(songs);
        show_error_toast(read ? TR("This playlist is empty") : TR("Playlist unavailable or unreadable"));
        return;
    }

    int start = 0;
    double position = 0;
    list_position_t saved;
    if (!from_start && list_positions_get(list, &saved) && saved.track_index >= 0) {
        if (saved.track_index < count && strcmp(songs[saved.track_index], saved.track_path) == 0) {
            start = saved.track_index;
        } else {
            /* The playlist was edited: find the track by its path. */
            start = -1;
            for (int k = 0; k < count && start < 0; k++)
                if (strcmp(songs[k], saved.track_path) == 0) start = k;
        }
        if (start >= 0) position = saved.position;
        else start = 0;
    }

    /* Leave saving for the previous list first, at its final place. */
    if (tracker.on) {
        tracker_record(true);
        tracker.on = false;
    }
    clear_player_source();
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
    tracker_start(list, songs, count);
    if (list_positions_set(list, start, songs[start], position)) tracker.dirty = true;
    tracker.index = start;
    tracker_write();
    /* Takes ownership of songs, and opens the player screen. */
    on_file_selected_at(songs, count, start, position);
    adopt_checked_revision = gui_player_queue_revision();
    if (!queue_matches_tracker()) tracker_stop(); /* playback was refused */
}

static void favorite_row_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    intptr_t tag = (intptr_t) lv_event_get_user_data(e);
    fav_view_t * v = &views[tag >> 16];
    int i = (int) (tag & 0xffff);
    if (i < 0 || i >= v->count) return;
    if (code == LV_EVENT_SHORT_CLICKED) play_favorite(v->paths[i], false);
    else if (code == LV_EVENT_LONG_PRESSED) play_favorite(v->paths[i], true);
}

static lv_obj_t * add_big_row(lv_obj_t * list, int32_t height) {
    lv_obj_t * row = lv_obj_create(list);
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, height);
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static void add_row_text(lv_obj_t * row, const char * name, const char * detail, int32_t left, int font_role) {
    int32_t text_w = LIST_ROW_WIDTH_WIDE - left - GUI_TEXT_INSET;
    lv_obj_t * title = lv_label_create(row);
    lv_label_set_text(title, name);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(font_role), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, text_w);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, left, -BOARD_SCALE_PX(16));

    lv_obj_t * sub = lv_label_create(row);
    lv_label_set_text(sub, detail);
    lv_obj_add_style(sub, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(sub, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, text_w);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, left, BOARD_SCALE_PX(24));
}

static void add_favorite_row(int view, int i) {
    fav_view_t * v = &views[view];
    char name[256];
    list_display_name(v->paths[i], name, sizeof(name));

    char detail[160];
    list_position_t saved;
    if (list_positions_get(v->paths[i], &saved) && saved.track_index >= 0) {
        char time[32];
        format_time(saved.position, time, sizeof(time));
        snprintf(detail, sizeof(detail), TR("Resume track %d at %s"), saved.track_index + 1, time);
    } else {
        snprintf(detail, sizeof(detail), "%s", TR("Play from the start"));
    }

    lv_obj_t * row = add_big_row(v->list, FAVORITES_ROW_HEIGHT);
    void * tag = (void *) (((intptr_t) view << 16) | i);
    lv_obj_add_event_cb(row, favorite_row_cb, LV_EVENT_SHORT_CLICKED, tag);
    lv_obj_add_event_cb(row, favorite_row_cb, LV_EVENT_LONG_PRESSED, tag);

    /* The cover's place, a quiet square until the picture lands. */
    int32_t box = FAVORITES_COVER_PX;
    lv_obj_t * frame = lv_obj_create(row);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, box, box);
    lv_obj_set_style_radius(frame, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x808080), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_20, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(frame, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, 0);
    lv_obj_t * image = lv_image_create(frame);
    lv_obj_center(image);
    v->images[i] = image;

    add_row_text(row, name, detail, GUI_TEXT_INSET + box + BOARD_SCALE_PX(20), GUI_FONT_ROLE_TITLE);
}

/* Fills a view with the playlists found in `dirs` (first that has any). */
static void view_fill(int view, const char * const * dirs, int dir_count) {
    fav_view_t * v = &views[view];
    for (int d = 0; d < dir_count && !v->count; d++)
        if (!playlist_files_scan(dirs[d], &v->paths, &v->count)) { v->paths = NULL; v->count = 0; }
}

static bool view_alloc_rows(fav_view_t * v) {
    if (!v->count) return true;
    v->images = calloc((size_t) v->count, sizeof(lv_obj_t *));
    v->pixels = calloc((size_t) v->count, sizeof(uint16_t *));
    v->dscs = calloc((size_t) v->count, sizeof(lv_image_dsc_t));
    return v->images && v->pixels && v->dscs;
}

static void view_add_rows(int view) {
    fav_view_t * v = &views[view];
    if (!view_alloc_rows(v)) return;
    for (int i = 0; i < v->count; i++) add_favorite_row(view, i);
    cover_job_start(v);
}

static void person_dirs(const char * name, char out[2][PATH_MAX]) {
    for (int d = 0; d < 2; d++) snprintf(out[d], PATH_MAX, "%s/%s", favorites_dirs[d], name);
}

static void populate_person_screen(int p) {
    fav_view_t * v = &views[VIEW_PERSON];
    view_clear(v);
    lv_label_set_text(v->title, people[p]);
    char dirs[2][PATH_MAX];
    person_dirs(people[p], dirs);
    const char * list[2] = { dirs[0], dirs[1] };
    view_fill(VIEW_PERSON, list, 2);
    if (!v->count) {
        char detail[200];
        snprintf(detail, sizeof(detail), TR("Discworld sync puts %s's playlists here."), people[p]);
        build_list_message(v->list, TR("No playlists yet"), detail);
        return;
    }
    view_add_rows(VIEW_PERSON);
}

static void person_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int p = (int) (intptr_t) lv_event_get_user_data(e);
    if (p < 0 || p >= people_count || !views[VIEW_PERSON].screen) return;
    populate_person_screen(p);
    lv_obj_scroll_to_y(views[VIEW_PERSON].list, 0, LV_ANIM_OFF);
    nav_push(views[VIEW_PERSON].screen);
}

static void add_person(const char * name, void * user) {
    (void) user;
    if (people_count >= FAVORITES_MAX_PEOPLE) return;
    for (int i = 0; i < people_count; i++) if (strcasecmp(people[i], name) == 0) return;
    snprintf(people[people_count++], sizeof(people[0]), "%s", name);
}

static void add_person_row(int p) {
    char dirs[2][PATH_MAX];
    person_dirs(people[p], dirs);
    char ** paths = NULL;
    int count = 0;
    for (int d = 0; d < 2 && !count; d++)
        if (!playlist_files_scan(dirs[d], &paths, &count)) { paths = NULL; count = 0; }
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);

    char detail[64];
    if (count == 1) snprintf(detail, sizeof(detail), "%s", TR("1 playlist"));
    else if (count) snprintf(detail, sizeof(detail), TR("%d playlists"), count);
    else snprintf(detail, sizeof(detail), "%s", TR("No playlists yet"));

    lv_obj_t * row = add_big_row(views[VIEW_TOP].list, FAVORITES_PERSON_ROW_HEIGHT);
    lv_obj_add_event_cb(row, person_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) p);
    add_row_text(row, people[p], detail, GUI_TEXT_INSET + BOARD_SCALE_PX(8), GUI_FONT_ROLE_TITLE);
}

static void populate_favorites_screen(void) {
    fav_view_t * v = &views[VIEW_TOP];
    view_clear(v);
    people_count = 0;
    discworld_each_person(current_settings.favorites_people, add_person, NULL);

    if (!sd_card_root_is_mounted()) {
        build_list_message(v->list, TR("SD card not available"), "");
        return;
    }
    for (int p = 0; p < people_count; p++) add_person_row(p);

    /* Playlists straight in the favourites folder; with no people set, every
     * playlist in Playlists when that folder has none. */
    view_fill(VIEW_TOP, favorites_dirs, 2);
    if (!v->count && !people_count) {
        const char * all[1] = { FAVORITES_PLAYLISTS_DIR };
        view_fill(VIEW_TOP, all, 1);
    }
    if (!v->count && !people_count) {
        build_list_message(v->list, TR("No playlists"),
                           TR("Put playlists in the SD card's Playlists/Favourites folder."));
        return;
    }
    view_add_rows(VIEW_TOP);
}

static void build_view(int view, const char * title) {
    fav_view_t * v = &views[view];
    v->screen = build_subsonic_list_screen(title, &v->title, &v->list);
    if (!v->screen) return;
    lv_obj_set_flex_align(v->list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(v->list, BOARD_SCALE_PX(12), 0);
}

void gui_favorites_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !views[VIEW_TOP].screen) return;
    populate_favorites_screen();
    lv_obj_scroll_to_y(views[VIEW_TOP].list, 0, LV_ANIM_OFF);
    nav_push(views[VIEW_TOP].screen);
}

bool gui_favorites_init(void) {
    list_positions_load();
    build_view(VIEW_TOP, TR("Favorites"));
    build_view(VIEW_PERSON, TR("Favorites"));
    return views[VIEW_TOP].screen != NULL && views[VIEW_PERSON].screen != NULL;
}

void gui_favorites_teardown(void) {
    if (tracker.on) tracker_record(true);
    for (int i = 0; i < VIEW_COUNT; i++) {
        view_clear(&views[i]);
        if (views[i].screen) lv_obj_delete(views[i].screen);
        memset(&views[i], 0, sizeof(views[i]));
    }
}
