#ifndef GUI_NETWORK_DISCWORLD_H
#define GUI_NETWORK_DISCWORLD_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

/* Wireless > Wi-Fi > Discworld sync: copies Music and playlists from the
 * Discworld server over Wi-Fi (network/services/discworld_sync.h), on request
 * or by itself while joined to the home network, optionally only while the
 * player is on its charger. */
void gui_network_discworld_build(void);
void gui_network_discworld_teardown(void);
void gui_network_discworld_row_cb(lv_event_t * e);
/* "Discworld sync now" in the Music menu: starts a sync and shows its progress. */
void gui_network_discworld_sync_now_cb(lv_event_t * e);
/* UI thread, from the housekeeping poll: reaps a finished sync, refreshes
 * the screen, and starts an automatic one when it is due. */
void gui_network_discworld_poll(void);
bool gui_network_discworld_has_work(void);
void gui_network_discworld_cancel(void);

#endif
