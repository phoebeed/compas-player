#include "gui_accent.h"
#include "../settings/gui_settings.h"
#include "../../core/persistence/settings.h"
#include "gui_theme.h"
#include "gui_color_picker.h"
#include "../widgets/builders/screen_builders.h"
#include "../shell/gui_navigation.h"
#include "../shell/gui_shell.h"
#include "../assets/assets.h"
#include "../text/i18n/i18n.h"
#include "../shell/gui_notifications.h"
#include "../../core/ui/display/startup_splash.h"
#include "../../core/persistence/storage_paths.h"
#include <stdio.h>
#include <stdint.h>

static lv_obj_t * accent_color_screen;

/* ---- Accent Color ----
 * The picker and accent switches share one scrollable page. Dragging previews
 * locally; the accent is applied app wide (styles,
 * decoded play/drawer art, settings file) once, on release. */
static lv_obj_t * accent_dynamic_switch;
static lv_obj_t * accent_picker;

/* Mirrors the live accent state into the screen. */
static void accent_screen_sync(void) {
    if (!accent_picker) return;
    bool dynamic = current_settings.accent_dynamic;
    uint32_t cover = 0;
    bool has_cover = gui_theme_cover_accent(&cover);
    const char * source = !dynamic ? TR("Custom color")
                        : has_cover ? TR("From album art")
                        : TR("From album art (no cover, using custom)");
    gui_color_picker_set_rgb(accent_picker, gui_theme_accent_rgb());
    gui_color_picker_set_info(accent_picker, source);
    gui_color_picker_set_active(accent_picker, !dynamic);

    if (accent_dynamic_switch) {
        if (dynamic) lv_obj_add_state(accent_dynamic_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(accent_dynamic_switch, LV_STATE_CHECKED);
    }
}

static void accent_picker_change_cb(uint32_t rgb, bool final, void * user_data) {
    (void) user_data;
    if (final) gui_theme_apply_accent(rgb);
    else gui_color_picker_set_info(accent_picker, TR("Custom color"));
}

static void accent_dynamic_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_theme_set_accent_dynamic(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void accent_startup_tint_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_splash_tint = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

/* Custom startup image: the loader draws /usr/data/.compas/startup.bin when it
 * is present (see startup_splash.h). This copies a ready-made, screen-sized
 * RGB565 LVGL image from the SD card there, or removes it to restore the
 * default splash. Nothing outside the player's own data folder is touched. */
#define STARTUP_IMAGE_SD_PATH SD_COMPAS_ROOT "/startup.bin"

static lv_obj_t * startup_image_restore_card;

static void startup_image_sync(void) {
    if (!startup_image_restore_card) return;
    if (startup_splash_configured()) lv_obj_remove_flag(startup_image_restore_card, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(startup_image_restore_card, LV_OBJ_FLAG_HIDDEN);
}

static void startup_image_load_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * reason = NULL;
    if (startup_splash_install(STARTUP_IMAGE_SD_PATH, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, &reason))
        show_info_toast(TR("Startup image saved"));
    else
        show_error_toast(TR(reason ? reason : "Unable to save startup image"));
    startup_image_sync();
}

static void startup_image_restore_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * reason = NULL;
    if (startup_splash_install(NULL, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, &reason))
        show_info_toast(TR("Default startup image restored"));
    else
        show_error_toast(TR(reason ? reason : "Unable to save startup image"));
    startup_image_sync();
}

static lv_obj_t * add_startup_image_card(lv_obj_t * body, int32_t card_w, const char * title,
                                         const char * hint, lv_event_cb_t cb) {
    lv_obj_t * card = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(4), 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    gui_card_text(card, title, GUI_FONT_ROLE_ROW, false);
    lv_obj_t * hint_label = gui_card_text(card, hint, GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(hint_label, lv_pct(100));
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
    return card;
}

static void accent_titlebar_tint_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_theme_set_titlebar_tint(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

/* Accent switches follow the color picker on the same page. */
static void add_accent_options(lv_obj_t * body, int32_t card_w) {
    /* Match album art */
    lv_obj_t * dyn = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(dyn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dyn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dyn, BOARD_SCALE_PX(12), 0);
    lv_obj_t * dyn_text = gui_plain_obj_create(dyn);
    lv_obj_set_flex_grow(dyn_text, 1);
    lv_obj_set_height(dyn_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dyn_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dyn_text, BOARD_SCALE_PX(4), 0);
    gui_card_text(dyn_text, TR("Match album art"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * dyn_hint = gui_card_text(dyn_text, TR("Takes its color from the cover of the playing track"),
                                      GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(dyn_hint, lv_pct(100));
    lv_label_set_long_mode(dyn_hint, LV_LABEL_LONG_WRAP);
    accent_dynamic_switch = lv_switch_create(dyn);
    lv_obj_add_style(accent_dynamic_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(accent_dynamic_switch, accent_dynamic_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Built-in splash tint is an independent choice from album-art matching.
     * Custom startup images keep their original colors. */
    lv_obj_t * splash_tint = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(splash_tint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(splash_tint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(splash_tint, BOARD_SCALE_PX(12), 0);
    lv_obj_t * splash_tint_text = gui_plain_obj_create(splash_tint);
    lv_obj_set_flex_grow(splash_tint_text, 1);
    lv_obj_set_height(splash_tint_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(splash_tint_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(splash_tint_text, BOARD_SCALE_PX(4), 0);
    gui_card_text(splash_tint_text, TR("Tint startup image"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * splash_tint_hint = gui_card_text(splash_tint_text,
        TR("Uses the accent color for the default splash."), GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(splash_tint_hint, lv_pct(100));
    lv_label_set_long_mode(splash_tint_hint, LV_LABEL_LONG_WRAP);
    lv_obj_t * startup_tint_switch = lv_switch_create(splash_tint);
    lv_obj_add_style(startup_tint_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_tint_switch, accent_startup_tint_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (current_settings.startup_splash_tint)
        lv_obj_add_state(startup_tint_switch, LV_STATE_CHECKED);

    add_startup_image_card(body, card_w, TR("Load startup image from SD card"),
        TR("Copies .compas/startup.bin from the SD card. It must be a screen-sized RGB565 LVGL image."),
        startup_image_load_event_cb);
    startup_image_restore_card = add_startup_image_card(body, card_w, TR("Restore default startup image"),
        TR("Removes the custom startup image."), startup_image_restore_event_cb);
    startup_image_sync();

    /* Breeze title-bar icons follow the text color unless tinted here. */
    lv_obj_t * titlebar_tint = gui_card_create(body, card_w);
    lv_obj_set_flex_flow(titlebar_tint, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titlebar_tint, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(titlebar_tint, BOARD_SCALE_PX(12), 0);
    lv_obj_t * titlebar_tint_text = gui_plain_obj_create(titlebar_tint);
    lv_obj_set_flex_grow(titlebar_tint_text, 1);
    lv_obj_set_height(titlebar_tint_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(titlebar_tint_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(titlebar_tint_text, BOARD_SCALE_PX(4), 0);
    gui_card_text(titlebar_tint_text, TR("Tint title bar icons"), GUI_FONT_ROLE_ROW, false);
    lv_obj_t * titlebar_tint_hint = gui_card_text(titlebar_tint_text,
        TR("Uses the accent color for the icons next to screen titles."), GUI_FONT_ROLE_SUBTEXT, true);
    lv_obj_set_width(titlebar_tint_hint, lv_pct(100));
    lv_label_set_long_mode(titlebar_tint_hint, LV_LABEL_LONG_WRAP);
    lv_obj_t * titlebar_tint_switch = lv_switch_create(titlebar_tint);
    lv_obj_add_style(titlebar_tint_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(titlebar_tint_switch, accent_titlebar_tint_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (current_settings.titlebar_icon_tint)
        lv_obj_add_state(titlebar_tint_switch, LV_STATE_CHECKED);

}

static lv_obj_t * build_accent_color_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_screen_header(scr, TR("Accent color"), generic_back_cb, NULL, NULL);

    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t card_w = BOARD_SCREEN_WIDTH - 2 * BOARD_SCALE_PX(20);

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT - top);
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(body, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_bottom(body, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(14), 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    accent_picker = gui_color_picker_create(body, card_w, current_settings.accent_color, accent_palette,
                                            ACCENT_PALETTE_COUNT, accent_picker_change_cb, NULL);

    add_accent_options(body, card_w);
    accent_screen_sync();
    finalize_screen_navigation(scr);
    return scr;
}

void gui_accent_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(accent_color_screen);
}

void gui_accent_build_screens(void) {
    accent_color_screen = build_accent_color_screen();
}

void gui_accent_teardown(void) {
    if (accent_color_screen) { lv_obj_delete(accent_color_screen); accent_color_screen = NULL; }
    accent_dynamic_switch = NULL;
    accent_picker = NULL; /* accent_screen_sync() is a no-op until the screen is rebuilt */
    startup_image_restore_card = NULL;
}

void gui_accent_sync(void) {
    accent_screen_sync();
}

