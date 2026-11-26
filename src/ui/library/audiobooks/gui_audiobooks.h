#ifndef GUI_AUDIOBOOKS_H
#define GUI_AUDIOBOOKS_H

#include <stdbool.h>
#include "lvgl/lvgl.h"

/* Home's Audiobooks tile: every audiobook in the SD card's Audiobooks folder
 * (where Discworld sync puts them, as Author/Book title/ or
 * Author/Series/03 Title/), listed with cover art in the same row format as
 * Music > Albums: the book's title, then its author and how far in you are.
 *
 * A book is a folder of audio files (CD1/Disc 2 style subfolders are joined
 * in order), a folder of several .m4b files is one book per file, and a
 * loose audio file in Audiobooks is a book of its own. The cover is the
 * folder's cover/folder picture, else the art inside the first file.
 *
 * A tap plays the book from where it was left; a long press plays it from
 * the start. Each book's place (chapter and seconds) is saved as it plays,
 * in the same store as the Favourites playlists
 * (library/playlists/list_positions.h), keyed by the book's first file. */
bool gui_audiobooks_init(void);
void gui_audiobooks_teardown(void);
void gui_audiobooks_home_tile_cb(lv_event_t * e);
/* Called every UI loop; it throttles itself. Saves the playing book's place
 * and shows covers as they are decoded. */
void gui_audiobooks_poll(void);
/* Saves the playing book's place now (before a power off). */
void gui_audiobooks_checkpoint(void);

#endif
