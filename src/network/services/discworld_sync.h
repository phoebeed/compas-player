#ifndef DISCWORLD_SYNC_H
#define DISCWORLD_SYNC_H

/* Wi-Fi sync with a Discworld server (the home media app on jarvis).
 *
 * Discworld's own "Sync with HiBy" page copies Music onto the card over USB
 * (its static/js/hibysync-worker.js); this does the same job over Wi-Fi with
 * the same three calls and the same record on the card, so the two never
 * fight over files:
 *
 *   GET /api/hiby/manifest   every song, cover and lyrics file in Music, each
 *                            with the path it goes to on the card (under
 *                            Music/) and a version. 202 while it is listed.
 *   GET /api/hiby/file?p=    one file (a song without art gets its folder's
 *                            cover put inside the copy it is sent as).
 *   GET /api/hiby/playlists  the profile's playlists as .m3u8 text whose
 *                            entries are "../Music/..." paths.
 *
 *   <card>/.discworld-sync.json  {files: {card path: {p, v}}, playlists: [..],
 *                                 compas_playlists: [..], at}
 *
 * Only files that record lists are ever deleted, so music put on the card by
 * hand is never touched. New and changed files are downloaded to a temporary
 * file that replaces the real one only once complete, and the record is
 * saved every 100 files, so a sync cut short (Wi-Fi gone, charger pulled,
 * Stop) carries on from where it got to next time. Playlists go to
 * <card>/Playlists, where compás reads them (the USB page writes
 * playlist_data/ for HiBy's own OS); compas_playlists lists the ones this
 * wrote, and a playlist of the user's own with the same name is never
 * replaced.
 *
 * Worker-thread only: everything here blocks. No UI code. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "../http/client/http_client.h"

#define DISCWORLD_RECORD_NAME ".discworld-sync.json"
#define DISCWORLD_PLAYLISTS_DIR "Playlists"

typedef struct {
    char base_url[256];   /* "http://jarvis.local:8080", no trailing slash */
    char password[128];   /* Discworld's APP_PASSWORD; empty when it has none */
    char profile[64];     /* whose playlists ("who"); empty: the shared ones */
    char card_root[256];  /* the SD card's mount point */
    bool require_mounted; /* refuse when card_root is only the empty mount point (no card in) */
} discworld_config_t;

typedef enum {
    DISCWORLD_PHASE_CONNECTING,
    DISCWORLD_PHASE_LISTING,   /* Discworld is still reading Music: list_done of list_total */
    DISCWORLD_PHASE_PLANNING,
    DISCWORLD_PHASE_REMOVING,
    DISCWORLD_PHASE_COPYING,
    DISCWORLD_PHASE_PLAYLISTS,
} discworld_phase_t;

typedef struct {
    discworld_phase_t phase;
    int list_done, list_total;
    int files_done, files_total;   /* this phase's files */
    uint64_t bytes_done, bytes_total;
    char current[160];             /* the song being copied, card path without Music/ */
} discworld_progress_t;

typedef enum {
    DISCWORLD_OK = 0,
    DISCWORLD_STOPPED,       /* cancelled, or keep_going() said stop */
    DISCWORLD_ERR_ARGUMENT,
    DISCWORLD_ERR_CONNECT,   /* no answer from the server */
    DISCWORLD_ERR_PASSWORD,  /* it wants a password, or refused the one set */
    DISCWORLD_ERR_SERVER,    /* an answer that was an error, or not what Discworld sends */
    DISCWORLD_ERR_CARD,      /* the card could not be read or written */
    DISCWORLD_ERR_FULL,      /* the card ran out of room */
    DISCWORLD_ERR_MEMORY,
} discworld_status_t;

typedef struct {
    discworld_status_t status;
    int copied, updated, removed, failed, adopted, playlists;
    int left;                /* files still to copy when the sync stopped early */
    uint64_t bytes;          /* bytes downloaded */
    bool changed;            /* anything on the card changed: rescan the library */
    char detail[160];        /* safe diagnostic; never holds the password or cookie */
} discworld_result_t;

/* Called between files and while downloading; false stops the sync (the
 * charger was pulled with "only while charging" on). NULL: always go on. */
typedef bool (*discworld_keep_going_fn)(void * user);
/* Progress as it happens; called on the worker thread. NULL: none. */
typedef void (*discworld_progress_fn)(const discworld_progress_t * progress, void * user);

void discworld_sync_run(const discworld_config_t * config, http_cancel_token_t * cancel,
                        discworld_keep_going_fn keep_going, discworld_progress_fn progress, void * user,
                        discworld_result_t * result);

/* Trims spaces and trailing slashes, and adds http:// when no scheme is
 * given ("jarvis.local:8080" -> "http://jarvis.local:8080"). */
void discworld_normalize_url(const char * input, char * out, size_t size);

/* A card path the server gave is safe to write or delete: under Music/,
 * relative, no "." or ".." parts, no backslash or control characters.
 * Exposed for tests. */
bool discworld_card_path_ok(const char * path);

#endif
