#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "../text/rendering/gui_text_input.h"
#include "../widgets/builders/screen_builders.h"
#include "../library/browse/gui_library.h"
#include "../../core/persistence/settings.h"
#include "../../core/process/worker_notify.h"
#include "../../core/process/worker_thread.h"
#include "../../core/system/logging/debug_log.h"
#include "../../hardware/power/battery.h"
#include "../../library/browser/paths/library_paths.h"
#include "../../network/services/discworld_sync.h"
#include "../../network/wifi/wifi_control.h"
#include "gui_network_discworld.h"
#include "gui_network_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern player_settings_t current_settings;

/* An automatic sync runs at most this often after one that finished, and is
 * tried again this soon after one that failed (Discworld off, Wi-Fi flaky). */
#define DISCWORLD_AUTO_EVERY_S (6 * 60 * 60)
#define DISCWORLD_AUTO_RETRY_S (15 * 60)
/* How often the home-network check runs while auto sync is on (it asks
 * wpa_supplicant which network this is, so not every housekeeping tick). */
#define DISCWORLD_AUTO_CHECK_MS (2 * 60 * 1000)
#define DISCWORLD_WORKER_STACK (1024 * 1024)

typedef enum { DW_JOB_SYNC, DW_JOB_AUTO_SYNC, DW_JOB_READ_SSID } dw_job_kind_t;

typedef struct {
    dw_job_kind_t kind;
    discworld_config_t config;
    char auto_ssid[WIFI_MAX_SSID_LEN];
    bool charging_only;
} dw_job_t;

/* ---- worker state: written by the worker, read by the UI under the mutex ---- */
static pthread_mutex_t dw_mutex = PTHREAD_MUTEX_INITIALIZER;
static discworld_progress_t dw_progress;
static bool dw_progress_valid;
static discworld_result_t dw_result;
static bool dw_not_home;          /* auto job: this network is not the home one */
static char dw_ssid[WIFI_MAX_SSID_LEN];
static bool dw_ssid_ok;

static dw_job_t dw_job;
static pthread_t dw_thread;
static bool dw_thread_active;
static atomic_bool dw_done;
static http_cancel_token_t dw_cancel;
static atomic_bool dw_unplugged;  /* the charger was pulled with "only while charging" on */

/* ---- UI state ---- */
static lv_obj_t * dw_screen;
static lv_obj_t * dw_list;
static lv_obj_t * dw_status_label;
static char dw_last_summary[200];  /* what the last sync did, shown under the rows */
static uint32_t dw_last_auto_check;
static time_t dw_auto_retry_after;

static void dw_populate(void);

/* Only the screen in front is redrawn; gui_network_discworld_row_cb() fills it on the way in. */
static bool dw_visible(void) {
    return dw_screen && lv_screen_active() == dw_screen;
}

/* ---- power ---- */

static bool dw_on_power(void) {
    return battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED || battery_is_charging();
}

/* Mid-sync, only a definite "unplugged" stops it: an unreadable state is a
 * passing sysfs hiccup, not the cable coming out. */
static bool dw_keep_going(void * user) {
    const dw_job_t * job = user;
    if (!job->charging_only) return true;
    if (atomic_load(&dw_unplugged)) return false;
    /* Called for every chunk downloaded: read the charger at most once a second. */
    static time_t last_look; /* worker thread only */
    time_t now = time(NULL);
    if (now == last_look) return true;
    last_look = now;
    if (battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_DISCONNECTED && !battery_is_charging()) {
        atomic_store(&dw_unplugged, true);
        return false;
    }
    return true;
}

static void dw_on_progress(const discworld_progress_t * progress, void * user) {
    (void) user;
    pthread_mutex_lock(&dw_mutex);
    dw_progress = *progress;
    dw_progress_valid = true;
    pthread_mutex_unlock(&dw_mutex);
    worker_notify_ui();
}

static void * dw_worker(void * arg) {
    (void) arg;
    worker_lower_priority(5);
    dw_job_t * job = &dw_job;
    wifi_info_t info;
    bool joined = false;
    if (job->kind != DW_JOB_SYNC) {
        memset(&info, 0, sizeof(info));
        joined = wifi_control_get_info(&info);
    }
    if (job->kind == DW_JOB_READ_SSID) {
        pthread_mutex_lock(&dw_mutex);
        dw_ssid_ok = joined && info.ssid[0];
        snprintf(dw_ssid, sizeof(dw_ssid), "%s", joined ? info.ssid : "");
        pthread_mutex_unlock(&dw_mutex);
    } else if (job->kind == DW_JOB_AUTO_SYNC && (!joined || strcmp(info.ssid, job->auto_ssid) != 0)) {
        pthread_mutex_lock(&dw_mutex);
        dw_not_home = true;
        pthread_mutex_unlock(&dw_mutex);
    } else {
        discworld_result_t result;
        discworld_sync_run(&job->config, &dw_cancel, dw_keep_going, dw_on_progress, job, &result);
        pthread_mutex_lock(&dw_mutex);
        dw_result = result;
        pthread_mutex_unlock(&dw_mutex);
    }
    atomic_store_explicit(&dw_done, true, memory_order_release);
    worker_notify_ui();
    return NULL;
}

static bool dw_start(dw_job_kind_t kind) {
    if (dw_thread_active) return false;
    memset(&dw_job, 0, sizeof(dw_job));
    dw_job.kind = kind;
    discworld_normalize_url(current_settings.discworld_url, dw_job.config.base_url, sizeof(dw_job.config.base_url));
    snprintf(dw_job.config.password, sizeof(dw_job.config.password), "%s", current_settings.discworld_password);
    snprintf(dw_job.config.profile, sizeof(dw_job.config.profile), "%s", current_settings.discworld_profile);
    snprintf(dw_job.config.card_root, sizeof(dw_job.config.card_root), "%s", MUSIC_ROOT_DIR);
#ifndef HOST_BUILD
    dw_job.config.require_mounted = true;
#endif
    snprintf(dw_job.auto_ssid, sizeof(dw_job.auto_ssid), "%s", current_settings.discworld_auto_ssid);
    dw_job.charging_only = current_settings.discworld_charging_only;

    pthread_mutex_lock(&dw_mutex);
    dw_progress_valid = false;
    dw_not_home = false;
    memset(&dw_result, 0, sizeof(dw_result));
    pthread_mutex_unlock(&dw_mutex);
    atomic_store(&dw_done, false);
    atomic_store(&dw_unplugged, false);
    http_cancel_token_init(&dw_cancel);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, DISCWORLD_WORKER_STACK);
    int rc = pthread_create(&dw_thread, &attr, dw_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        http_cancel_token_destroy(&dw_cancel);
        return false;
    }
    dw_thread_active = true;
    return true;
}

/* ---- text ---- */

static void dw_format_bytes(uint64_t bytes, char * out, size_t size) {
    if (bytes >= (uint64_t) 1 << 30) snprintf(out, size, "%.1f GB", (double) bytes / (1 << 30));
    else snprintf(out, size, "%.0f MB", (double) bytes / (1 << 20));
}

static void dw_format_when(time_t when, char * out, size_t size) {
    struct tm tm;
    localtime_r(&when, &tm);
    strftime(out, size, "%d %b %H:%M", &tm);
}

/* "12 new songs, 3 updated, 2 removed" from a result; "Up to date" when nothing moved. */
static void dw_format_counts(const discworld_result_t * r, char * out, size_t size) {
    out[0] = '\0';
    size_t used = 0;
    struct { int n; const char * one; const char * many; } parts[] = {
        { r->copied, N_("%d new file"), N_("%d new files") },
        { r->updated, N_("%d updated"), N_("%d updated") },
        { r->removed, N_("%d removed"), N_("%d removed") },
        { r->failed, N_("%d skipped"), N_("%d skipped") },
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (!parts[i].n) continue;
        int n = snprintf(out + used, size - used, used ? ", " : "");
        if (n > 0 && (size_t) n < size - used) used += (size_t) n;
        n = snprintf(out + used, size - used, TR(parts[i].n == 1 ? parts[i].one : parts[i].many), parts[i].n);
        if (n > 0 && (size_t) n < size - used) used += (size_t) n;
    }
    if (!used) snprintf(out, size, "%s", TR("Up to date"));
}

static void dw_status_text(char * out, size_t size) {
    if (dw_thread_active && dw_job.kind != DW_JOB_READ_SSID) {
        pthread_mutex_lock(&dw_mutex);
        discworld_progress_t p = dw_progress;
        bool valid = dw_progress_valid;
        pthread_mutex_unlock(&dw_mutex);
        if (!valid || p.phase == DISCWORLD_PHASE_CONNECTING) {
            snprintf(out, size, "%s", TR("Asking Discworld..."));
        } else if (p.phase == DISCWORLD_PHASE_LISTING) {
            snprintf(out, size, TR("Discworld is listing Music (%d of %d)"), p.list_done, p.list_total);
        } else if (p.phase == DISCWORLD_PHASE_PLANNING) {
            snprintf(out, size, "%s", TR("Working out what changed..."));
        } else if (p.phase == DISCWORLD_PHASE_REMOVING) {
            snprintf(out, size, TR("Removing %d of %d"), p.files_done, p.files_total);
        } else if (p.phase == DISCWORLD_PHASE_COPYING) {
            char done[24], total[24];
            dw_format_bytes(p.bytes_done, done, sizeof(done));
            dw_format_bytes(p.bytes_total, total, sizeof(total));
            snprintf(out, size, TR("Copying %d of %d (%s of %s)\n%s"), p.files_done + 1 > p.files_total ? p.files_total : p.files_done + 1,
                     p.files_total, done, total, p.current);
        } else {
            snprintf(out, size, "%s", TR("Writing playlists..."));
        }
        return;
    }
    if (dw_last_summary[0]) { snprintf(out, size, "%s", dw_last_summary); return; }
    if (current_settings.discworld_last_sync > 0) {
        char when[32];
        dw_format_when((time_t) current_settings.discworld_last_sync, when, sizeof(when));
        snprintf(out, size, TR("Last synced %s"), when);
        return;
    }
    snprintf(out, size, "%s", TR("Not synced yet"));
}

static void dw_refresh_status(void) {
    if (!dw_status_label) return;
    char text[256];
    dw_status_text(text, sizeof(text));
    if (strcmp(lv_label_get_text(dw_status_label), text) != 0) lv_label_set_text(dw_status_label, text);
}

/* ---- finishing a job ---- */

static void dw_finish_sync(bool automatic) {
    pthread_mutex_lock(&dw_mutex);
    discworld_result_t r = dw_result;
    pthread_mutex_unlock(&dw_mutex);
    char counts[120], when[32];
    dw_format_counts(&r, counts, sizeof(counts));
    time_t now = time(NULL);
    dw_format_when(now, when, sizeof(when));

    if (r.status == DISCWORLD_OK) {
        current_settings.discworld_last_sync = (int) now;
        settings_save_async(&current_settings);
        snprintf(dw_last_summary, sizeof(dw_last_summary), TR("Synced %s: %s"), when, counts);
        dw_auto_retry_after = 0;
        if (!automatic || r.changed) {
            char toast[160];
            snprintf(toast, sizeof(toast), TR("Discworld: %s"), counts);
            show_info_toast(toast);
        }
    } else {
        const char * why;
        if (r.status == DISCWORLD_STOPPED)
            why = atomic_load(&dw_unplugged) ? TR("Paused: unplugged from power") : TR("Stopped");
        else if (r.status == DISCWORLD_ERR_PASSWORD) why = TR("Discworld wants its password (set it above)");
        else if (r.status == DISCWORLD_ERR_CONNECT) why = TR("Could not reach Discworld");
        else if (r.status == DISCWORLD_ERR_FULL) why = TR("The SD card is full");
        else if (r.status == DISCWORLD_ERR_CARD) why = TR("Could not write to the SD card");
        else why = TR("Sync failed");
        bool partial = r.copied || r.updated || r.removed;
        if (partial)
            snprintf(dw_last_summary, sizeof(dw_last_summary), "%s %s (%s, %d left)", why, when, counts, r.left);
        else
            snprintf(dw_last_summary, sizeof(dw_last_summary), "%s %s", why, when);
        if (r.detail[0] && r.status != DISCWORLD_STOPPED) {
            size_t used = strlen(dw_last_summary);
            snprintf(dw_last_summary + used, sizeof(dw_last_summary) - used, "\n%s", r.detail);
        }
        DBG_LOG("discworld: sync ended status=%d copied=%d removed=%d failed=%d detail=%s\n",
                r.status, r.copied, r.removed, r.failed, r.detail);
        dw_auto_retry_after = now + DISCWORLD_AUTO_RETRY_S;
        /* Unplugged: carry on as soon as the charger is back, not after the retry wait. */
        if (r.status == DISCWORLD_STOPPED && atomic_load(&dw_unplugged)) dw_auto_retry_after = 0;
        if (!automatic && r.status != DISCWORLD_STOPPED) show_error_toast(why);
    }
    /* New songs and playlists only show once the library has looked again. */
    if (r.changed && gui_library_auto_rescan_enabled()) start_library_auto_rescan();
}

static void dw_finish_ssid(void) {
    pthread_mutex_lock(&dw_mutex);
    bool ok = dw_ssid_ok;
    char ssid[WIFI_MAX_SSID_LEN];
    snprintf(ssid, sizeof(ssid), "%s", dw_ssid);
    pthread_mutex_unlock(&dw_mutex);
    if (!ok) {
        show_error_toast(TR("Join your home Wi-Fi first"));
        return;
    }
    snprintf(current_settings.discworld_auto_ssid, sizeof(current_settings.discworld_auto_ssid), "%s", ssid);
    settings_save_async(&current_settings);
    dw_last_auto_check = 0;
    dw_auto_retry_after = 0;
}

static void dw_reap(void) {
    if (!dw_thread_active || !atomic_load_explicit(&dw_done, memory_order_acquire)) return;
    pthread_join(dw_thread, NULL);
    dw_thread_active = false;
    http_cancel_token_destroy(&dw_cancel);
    if (dw_job.kind == DW_JOB_READ_SSID) dw_finish_ssid();
    else {
        pthread_mutex_lock(&dw_mutex);
        bool not_home = dw_not_home;
        pthread_mutex_unlock(&dw_mutex);
        if (!not_home) dw_finish_sync(dw_job.kind == DW_JOB_AUTO_SYNC);
    }
    if (dw_visible()) dw_populate();
}

/* ---- automatic sync ---- */

static bool dw_auto_due(void) {
    if (!current_settings.discworld_auto_ssid[0] || dw_thread_active) return false;
    if (!gui_shell_wifi_effective_enabled() || gui_library_scan_active()) return false;
    if (current_settings.discworld_charging_only && !dw_on_power()) return false;
    time_t now = time(NULL);
    if (dw_auto_retry_after && now < dw_auto_retry_after) return false;
    time_t last = (time_t) current_settings.discworld_last_sync;
    /* A clock set back (no network time yet) must not stall it for good. */
    if (last > 0 && now >= last && now - last < DISCWORLD_AUTO_EVERY_S) return false;
    return true;
}

void gui_network_discworld_poll(void) {
    dw_reap();
    if (dw_thread_active && dw_visible()) dw_refresh_status();
    if (dw_last_auto_check && lv_tick_elaps(dw_last_auto_check) < DISCWORLD_AUTO_CHECK_MS) return;
    dw_last_auto_check = lv_tick_get() | 1;
    if (dw_auto_due() && dw_start(DW_JOB_AUTO_SYNC) && dw_visible()) dw_populate();
}

bool gui_network_discworld_has_work(void) {
    return dw_thread_active;
}

void gui_network_discworld_cancel(void) {
    if (!dw_thread_active) return;
    http_cancel_token_cancel(&dw_cancel);
    pthread_join(dw_thread, NULL);
    dw_thread_active = false;
    http_cancel_token_destroy(&dw_cancel);
}

/* ---- the screen ---- */

static void dw_url_done(const char * text, void * user) {
    (void) user;
    char url[sizeof(current_settings.discworld_url)];
    discworld_normalize_url(text ? text : "", url, sizeof(url));
    snprintf(current_settings.discworld_url, sizeof(current_settings.discworld_url), "%s", url);
    settings_save_async(&current_settings);
    dw_populate();
}

static void dw_password_done(const char * text, void * user) {
    (void) user;
    snprintf(current_settings.discworld_password, sizeof(current_settings.discworld_password), "%s", text ? text : "");
    settings_save_async(&current_settings);
    dw_populate();
}

static void dw_profile_done(const char * text, void * user) {
    (void) user;
    snprintf(current_settings.discworld_profile, sizeof(current_settings.discworld_profile), "%s", text ? text : "");
    settings_save_async(&current_settings);
    dw_populate();
}

static void dw_url_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Discworld address"), current_settings.discworld_url, false, false, dw_url_done, NULL);
}

static void dw_password_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Discworld password"), current_settings.discworld_password, true, false, dw_password_done, NULL);
}

static void dw_profile_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Whose playlists"), current_settings.discworld_profile, false, false, dw_profile_done, NULL);
}

static void dw_sync_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (dw_thread_active) {
        if (dw_job.kind == DW_JOB_READ_SSID) return;
        http_cancel_token_cancel(&dw_cancel); /* reaped by the poll, which keeps what was copied */
        return;
    }
    if (!wifi_feature_guard()) return;
    if (current_settings.discworld_charging_only && !dw_on_power()) {
        show_error_toast(TR("Connect power to sync (Only while charging is on)"));
        return;
    }
    dw_last_summary[0] = '\0';
    if (!dw_start(DW_JOB_SYNC)) show_error_toast(TR("Thread launch failed"));
    dw_populate();
}

static void dw_charging_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.discworld_charging_only = !current_settings.discworld_charging_only;
    settings_save_async(&current_settings);
    dw_populate();
}

static void dw_auto_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (current_settings.discworld_auto_ssid[0]) {
        current_settings.discworld_auto_ssid[0] = '\0';
        settings_save_async(&current_settings);
    } else if (!gui_shell_wifi_effective_enabled()) {
        show_error_toast(TR("Join your home Wi-Fi first"));
    } else if (!dw_start(DW_JOB_READ_SSID)) {
        show_error_toast(TR("Wait for the sync to finish"));
    }
    dw_populate();
}

static void dw_populate(void) {
    if (!dw_list) return;
    lv_coord_t scroll_y = lv_obj_get_scroll_y(dw_list);
    lv_obj_clean(dw_list);
    dw_status_label = NULL;
    char label[320];

    bool syncing = dw_thread_active && dw_job.kind != DW_JOB_READ_SSID;
    add_pill_chevron_row(dw_list, syncing ? TR("Stop sync") : TR("Sync now"), dw_sync_row_cb);
    dw_status_label = add_network_explanation(dw_list, "");
    dw_refresh_status();

    add_section_header(dw_list, TR("When to sync"));
    if (current_settings.discworld_auto_ssid[0])
        snprintf(label, sizeof(label), TR("Sync automatically on %s"), current_settings.discworld_auto_ssid);
    else
        snprintf(label, sizeof(label), "%s", TR("Sync automatically on home Wi-Fi"));
    add_pill_toggle_row(dw_list, label, current_settings.discworld_auto_ssid[0] != '\0', dw_auto_toggle_cb);
    add_pill_toggle_row(dw_list, TR("Only while charging"), current_settings.discworld_charging_only,
                        dw_charging_toggle_cb);
    add_network_explanation(dw_list,
        TR("Turning on automatic sync remembers the network you are on now. The player then syncs by itself "
           "on that network every few hours, copying only new and changed songs."));

    add_section_header(dw_list, TR("Server"));
    snprintf(label, sizeof(label), TR("Address: %s"),
             current_settings.discworld_url[0] ? current_settings.discworld_url : TR("not set"));
    add_pill_chevron_row(dw_list, label, dw_url_row_cb);
    add_pill_chevron_row(dw_list, current_settings.discworld_password[0] ? TR("Password: set") : TR("Password: none"),
                         dw_password_row_cb);
    snprintf(label, sizeof(label), TR("Playlists of: %s"),
             current_settings.discworld_profile[0] ? current_settings.discworld_profile : TR("shared only"));
    add_pill_chevron_row(dw_list, label, dw_profile_row_cb);
    add_network_explanation(dw_list,
        TR("Copies Discworld's Music folder to Music on the SD card, the same way its Sync with HiBy page "
           "does over USB, and your playlists to Playlists. Files you put on the card yourself are never touched."));
    lv_obj_scroll_to_y(dw_list, scroll_y, LV_ANIM_OFF);
}

void gui_network_discworld_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !dw_screen) return;
    dw_populate();
    nav_push(dw_screen);
}

void gui_network_discworld_build(void) {
    dw_screen = network_list_screen(TR("Discworld sync"), &dw_list);
}

void gui_network_discworld_teardown(void) {
    network_delete_screen(&dw_screen);
    dw_list = NULL;
    dw_status_label = NULL;
}
