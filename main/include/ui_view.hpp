/*
 * UI View
 * Draws the app around the terminal: the title bar (page title, SSH / WiFi / battery icons),
 * menu and prompt pages (ui_page.hpp) and the shared colours. Plain LVGL objects with a few
 * shared styles, symbol-font icons, no images. Objects live in PSRAM (lv_mem_psram.c). All
 * calls are made with the display lock held.
 */

#ifndef UI_VIEW_HPP
#define UI_VIEW_HPP

#include "lvgl.h"
#include "ui_page.hpp"
#include <functional>
#include <string>

namespace theme
{
    static const uint32_t BG = 0x0D1117;        // Screen
    static const uint32_t SURFACE = 0x161B22;   // Title bar, input field, notice card
    static const uint32_t DIVIDER = 0x21262D;   // Row separators, ChatGPT's bubbles
    static const uint32_t TEXT = 0xE6EDF3;
    static const uint32_t DIM = 0x8B949E;       // Details, hints, notes
    static const uint32_t ACCENT = 0x2F81F7;    // Highlighted row, the user's bubbles
    static const uint32_t DANGER = 0xF85149;
    static const uint32_t OK = 0x3FB950;
    static const uint32_t WARN = 0xD29922;
}

class UiView
{
public:
    // Title bar across the top `bar_h` pixels of `screen`; pages fill the rest (built hidden)
    void create(lv_obj_t* screen, int32_t bar_h);

    void set_title(const std::string& title);
    // battery_percent < 0: unknown (no icon)
    void set_status(bool wifi, bool ssh, int battery_percent);
    // Shows the back arrow in the title bar (tapping it calls on_back)
    void set_back(bool shown);

    // Builds and shows a page in the area between the title bar and `bottom` pixels above the
    // screen's lower edge. Menus highlight `selected`.
    void show_page(const ui::Page& page, const std::string& notice, int selected, int32_t bottom);
    void hide_page();
    bool page_shown() const { return page_obj && !lv_obj_has_flag(page_obj, LV_OBJ_FLAG_HIDDEN); }
    void set_notice(const std::string& notice);   // New messages while a page is shown
    void select(int row);                         // Highlight and scroll into view
    // Scrolls the page by a few lines (direction > 0: back toward the top); false at the end
    bool scroll(int direction);
    // Prompts: the input field is placed here, under the question (NULL for a menu)
    lv_obj_t* field_slot() const { return field_slot_obj; }

    // A row was tapped / the back arrow was tapped. Called from LVGL's task with the display
    // lock held: only record the choice and post an input event (keypad_task does the work).
    std::function<void(int row)> on_tap;
    std::function<void()> on_back;

private:
    lv_obj_t* bar = NULL;
    lv_obj_t* back_label = NULL;
    lv_obj_t* title_label = NULL;
    lv_obj_t* ssh_label = NULL;
    lv_obj_t* wifi_label = NULL;
    lv_obj_t* battery_label = NULL;
    lv_obj_t* page_obj = NULL;
    lv_obj_t* notice_label = NULL;
    lv_obj_t* field_slot_obj = NULL;
    lv_obj_t* first_row = NULL;
    int row_count = 0;
    int selected_row = -1;
    int32_t bar_height = 0;

    lv_obj_t* row_obj(int row) const;
    void paint_row(int row, bool selected);
    static void row_clicked_cb(lv_event_t* e);
    static void back_clicked_cb(lv_event_t* e);
};

#endif
