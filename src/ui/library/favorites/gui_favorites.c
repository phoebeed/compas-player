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
#include "../../../library/playlists/list_positions.h"
#include "../../../library/playlists/playlist_files.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FAVORITES_PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
/* Either spelling of the favourites folder inside Playlists. */
static const char * const favorites_dirs[] = {
    FAVORITES_PLAYLISTS_DIR "/Favorites",
    FAVORITES_PLAYLISTS_DIR "/Favourites",
};

#define FAVORITES_ROW_HEIGHT BOARD_SCALE_PX(120)
#define FAVORITES_POLL_MS 1000
/* While playing, the place is written at most this often; a pause, a track
 * change or the end of the list is written at once. */
#define FAVORITES_WRITE_INTERVAL_MS 30000
/* Polls in a row with nothing playing or paused before a list that reached
 * its last track counts as finished (track changes can briefly stop). */
#define FAVORITES_STOPPED_POLLS 3

static lv_obj_t * favorites_screen;
static lv_obj_t * favorites_list;
static char ** favorites_paths;
static int favorites_count;

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

static void free_favorites_paths(void) {
    for (int i = 0; i < favorites_count; i++) free(favorites_paths[i]);
    free(favorites_paths);
    favorites_paths = NULL;
    favorites_count = 0;
}

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

/* Starts list i: from its saved place, or from the top. */
static void play_favorite(int i, bool from_start) {
    if (i < 0 || i >= favorites_count) return;
    char list[PATH_MAX];
    snprintf(list, sizeof(list), "%s", favorites_paths[i]);
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
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (code == LV_EVENT_SHORT_CLICKED) play_favorite(i, false);
    else if (code == LV_EVENT_LONG_PRESSED) play_favorite(i, true);
}

static void add_favorite_row(int i) {
    char name[256];
    list_display_name(favorites_paths[i], name, sizeof(name));

    char detail[160];
    list_position_t saved;
    if (list_positions_get(favorites_paths[i], &saved) && saved.track_index >= 0) {
        char time[32];
        format_time(saved.position, time, sizeof(time));
        snprintf(detail, sizeof(detail), TR("Resume track %d at %s"), saved.track_index + 1, time);
    } else {
        snprintf(detail, sizeof(detail), "%s", TR("Play from the start"));
    }

    lv_obj_t * row = lv_obj_create(favorites_list);
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, FAVORITES_ROW_HEIGHT);
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, favorite_row_cb, LV_EVENT_SHORT_CLICKED, (void *) (intptr_t) i);
    lv_obj_add_event_cb(row, favorite_row_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);

    int32_t text_w = LIST_ROW_WIDTH_WIDE - 2 * GUI_TEXT_INSET;
    lv_obj_t * title = lv_label_create(row);
    lv_label_set_text(title, name);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, text_w);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, -BOARD_SCALE_PX(16));

    lv_obj_t * sub = lv_label_create(row);
    lv_label_set_text(sub, detail);
    lv_obj_add_style(sub, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(sub, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, text_w);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, BOARD_SCALE_PX(24));
}

static void populate_favorites_screen(void) {
    lv_obj_clean(favorites_list);
    free_favorites_paths();

    if (!sd_card_root_is_mounted()) {
        build_list_message(favorites_list, TR("SD card not available"), "");
        return;
    }
    for (size_t d = 0; d < sizeof(favorites_dirs) / sizeof(favorites_dirs[0]) && !favorites_count; d++)
        if (!playlist_files_scan(favorites_dirs[d], &favorites_paths, &favorites_count)) favorites_count = 0;
    if (!favorites_count && !playlist_files_scan(FAVORITES_PLAYLISTS_DIR, &favorites_paths, &favorites_count))
        favorites_count = 0;

    if (!favorites_count) {
        build_list_message(favorites_list, TR("No playlists"),
                           TR("Put playlists in the SD card's Playlists/Favourites folder."));
        return;
    }
    for (int i = 0; i < favorites_count; i++) add_favorite_row(i);
}

static lv_obj_t * build_favorites_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Favorites"), &title_label, &favorites_list);
    lv_obj_set_flex_align(favorites_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(favorites_list, BOARD_SCALE_PX(12), 0);
    return scr;
}

void gui_favorites_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !favorites_screen) return;
    populate_favorites_screen();
    lv_obj_scroll_to_y(favorites_list, 0, LV_ANIM_OFF);
    nav_push(favorites_screen);
}

bool gui_favorites_init(void) {
    list_positions_load();
    favorites_screen = build_favorites_screen();
    return favorites_screen != NULL;
}

void gui_favorites_teardown(void) {
    if (tracker.on) tracker_record(true);
    free_favorites_paths();
    if (favorites_screen) { lv_obj_delete(favorites_screen); favorites_screen = NULL; }
    favorites_list = NULL;
}
