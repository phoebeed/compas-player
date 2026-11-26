#ifndef GUI_FAVORITES_H
#define GUI_FAVORITES_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

/* Home's Favorites tile. With people set (Settings favorites_people, "Rachel,
 * Phoebe, Amelie"), a large button for each opens that person's playlists,
 * from Playlists/Favourites/<name> (Discworld sync writes them there); any
 * playlists straight in Playlists/Favourites (or Favorites) follow the
 * buttons. With nobody set, those playlists, falling back to every playlist
 * in Playlists when that folder has none. Each playlist shows a cover: a
 * picture of the same name beside it (.jpg/.png), else its first song's
 * album cover. Rows are large for use
 * while driving. A tap plays the list from where it was last left; a long
 * press plays it from the start. Each list's place is saved as it plays
 * (library/playlists/list_positions.h). */
bool gui_favorites_init(void);
void gui_favorites_teardown(void);
void gui_favorites_home_tile_cb(lv_event_t * e);
/* Called every UI loop; it throttles itself. Saves the playing list's place. */
void gui_favorites_poll(void);
/* Saves the playing list's place now (before a power off). */
void gui_favorites_checkpoint(void);

#endif
