#ifndef LIST_POSITIONS_H
#define LIST_POSITIONS_H

#include <stdbool.h>
#include <stddef.h>

/* Where each favourite playlist was last left: the track (by index and
 * path, so an edited playlist still finds it) and the seconds into it.
 * Kept in INTERNAL_COMPAS_DIR, separate from settings.txt, most recently
 * used first, capped at LIST_POSITIONS_MAX lists. No LVGL dependency.
 *
 * track_index -1 means the list was played to its end: the next start
 * begins at the first track again. */
#define LIST_POSITIONS_MAX 32
#define LIST_POSITIONS_PATH_MAX 512

typedef struct {
    char list_path[LIST_POSITIONS_PATH_MAX];
    char track_path[LIST_POSITIONS_PATH_MAX];
    int track_index;
    double position;
} list_position_t;

/* Loads the file once; later calls are no-ops. Safe to call often. */
void list_positions_load(void);
/* Copies the saved position of list_path into out. False when none. */
bool list_positions_get(const char * list_path, list_position_t * out);
/* Records a position in memory (moving the list to the front). Returns true
 * when the value changed. Call list_positions_save() to write it. */
bool list_positions_set(const char * list_path, int track_index, const char * track_path, double position);
/* The list that was playing when the file was last written ("" if none),
 * so a queue restored at boot keeps saving into the right list. */
const char * list_positions_active(void);
void list_positions_set_active(const char * list_path);
/* Writes the file (temp file + rename). False on I/O failure. */
bool list_positions_save(void);

#endif
