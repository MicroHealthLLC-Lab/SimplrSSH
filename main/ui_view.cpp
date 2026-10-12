/*
 * UI View
 * The title bar and the menu / prompt pages (ui_page.hpp), drawn with plain LVGL objects
 * and a handful of shared styles: dark background, one accent colour for the highlighted
 * row, icons from LVGL's symbol font. Sizes and fonts come from board::ui(), so the same
 * code fits the T-Deck's 320x240 and the Tab5's 1280x720 screen.
 */

#include "ui_view.hpp"
#include "board.hpp"
#include <cstdint>

static lv_style_t s_row;          // A menu row; highlighted (CHECKED) and touched (PRESSED)
static lv_style_t s_row_selected;
static lv_style_t s_row_pressed;
static lv_style_t s_mark;         // Checkbox / radio / on-off marks at a row's end
static bool s_styles_ready = false;

static const uint32_t SELECTED_TEXT = 0xFFFFFF;
static const uint32_t SELECTED_DIM = 0xD0DEF5;

static void init_styles()
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;
    const board::Ui& ui = board::ui();

    lv_style_init(&s_row);
    lv_style_set_width(&s_row, lv_pct(100));
    lv_style_set_height(&s_row, LV_SIZE_CONTENT);
    lv_style_set_min_height(&s_row, ui.row_h);
    lv_style_set_pad_hor(&s_row, ui.gap * 2);
    lv_style_set_pad_ver(&s_row, ui.gap);
    lv_style_set_pad_column(&s_row, ui.gap * 2);
    lv_style_set_pad_row(&s_row, 0);
    lv_style_set_border_side(&s_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_border_width(&s_row, 1);
    lv_style_set_border_color(&s_row, lv_color_hex(theme::DIVIDER));
    lv_style_set_layout(&s_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&s_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_style_set_flex_cross_place(&s_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&s_row, LV_FLEX_ALIGN_CENTER);

    lv_style_init(&s_row_selected);
    lv_style_set_bg_color(&s_row_selected, lv_color_hex(theme::ACCENT));
    lv_style_set_bg_opa(&s_row_selected, LV_OPA_COVER);
    lv_style_set_radius(&s_row_selected, ui.gap * 3 / 2);
    lv_style_set_border_color(&s_row_selected, lv_color_hex(theme::ACCENT));   // Same height as unselected

    lv_style_init(&s_row_pressed);
    lv_style_set_bg_color(&s_row_pressed, lv_color_hex(theme::DIVIDER));
    lv_style_set_bg_opa(&s_row_pressed, LV_OPA_COVER);

    lv_style_init(&s_mark);
    lv_style_set_border_width(&s_mark, 2);
    lv_style_set_text_align(&s_mark, LV_TEXT_ALIGN_CENTER);
    lv_style_set_text_font(&s_mark, ui.small);
    lv_style_set_bg_opa(&s_mark, LV_OPA_COVER);
}

static lv_obj_t* plain_obj(lv_obj_t* parent)
{
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t* text_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static int32_t text_width(const char* text, const lv_font_t* font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

void UiView::create(lv_obj_t* screen, int32_t bar_h)
{
    init_styles();
    const board::Ui& ui = board::ui();
    bar_height = bar_h;

    bar = plain_obj(screen);
    lv_obj_set_size(bar, lv_pct(100), bar_h);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, ui.gap * 2, 0);
    lv_obj_set_style_pad_column(bar, ui.gap * 2, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    back_label = text_label(bar, ui.title, theme::TEXT, LV_SYMBOL_LEFT);
    lv_obj_add_flag(back_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back_label, bar_h / 2);
    lv_obj_add_event_cb(back_label, back_clicked_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(back_label, LV_OBJ_FLAG_HIDDEN);

    title_label = text_label(bar, ui.title, theme::TEXT, "");
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(title_label, 1);

    ssh_label = text_label(bar, ui.input, theme::OK, "SSH");
    lv_obj_add_flag(ssh_label, LV_OBJ_FLAG_HIDDEN);
    wifi_label = text_label(bar, ui.input, theme::DIM, LV_SYMBOL_WIFI);
    battery_label = text_label(bar, ui.input, theme::TEXT, "");
    lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);

    page_obj = plain_obj(screen);
    lv_obj_add_flag(page_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_scroll_dir(page_obj, LV_DIR_VER);
    lv_obj_remove_flag(page_obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(page_obj, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(page_obj, lv_color_hex(theme::DIM), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(page_obj, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(page_obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(page_obj, lv_color_hex(theme::BG), 0);
    lv_obj_set_style_bg_opa(page_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page_obj, ui.gap, 0);
    lv_obj_set_style_pad_row(page_obj, ui.gap, 0);
    lv_obj_set_flex_flow(page_obj, LV_FLEX_FLOW_COLUMN);
}

void UiView::set_title(const std::string& title)
{
    if (title_label && title != lv_label_get_text(title_label)) {
        lv_label_set_text(title_label, title.c_str());
    }
}

void UiView::set_status(bool wifi, bool ssh, int battery_percent)
{
    if (!bar) {
        return;
    }
    if (ssh) {
        lv_obj_remove_flag(ssh_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ssh_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(wifi ? theme::TEXT : theme::DIM), 0);

    static const char* const LEVELS[] = {LV_SYMBOL_BATTERY_EMPTY, LV_SYMBOL_BATTERY_1, LV_SYMBOL_BATTERY_2,
                                         LV_SYMBOL_BATTERY_3, LV_SYMBOL_BATTERY_FULL};
    int level = ui::battery_level(battery_percent);
    if (level < 0) {
        lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text_fmt(battery_label, "%s %d%%", LEVELS[level], battery_percent > 100 ? 100 : battery_percent);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(level == 0 ? theme::DANGER : theme::TEXT), 0);
    lv_obj_remove_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
}

void UiView::set_back(bool shown)
{
    if (!back_label || shown != lv_obj_has_flag(back_label, LV_OBJ_FLAG_HIDDEN)) {
        return;   // Unchanged (called often: no needless redraw)
    }
    if (shown) {
        lv_obj_remove_flag(back_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(back_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// The mark at a row's end: a checkbox, a radio button or an on/off pill, as one styled label
static void paint_mark(lv_obj_t* mark_label, ui::Mark mark, bool selected)
{
    bool on = mark == ui::Mark::Checked || mark == ui::Mark::Chosen || mark == ui::Mark::On;
    uint32_t fill = selected ? SELECTED_TEXT : theme::ACCENT;
    uint32_t edge = on ? fill : (selected ? SELECTED_DIM : theme::DIM);
    lv_obj_set_style_border_color(mark_label, lv_color_hex(edge), 0);
    if (mark == ui::Mark::On || mark == ui::Mark::Off) {
        lv_obj_set_style_bg_color(mark_label, lv_color_hex(on ? fill : theme::DIVIDER), 0);
        lv_obj_set_style_text_color(mark_label, lv_color_hex(on ? (selected ? theme::ACCENT : SELECTED_TEXT) : theme::DIM), 0);
    } else {
        lv_obj_set_style_bg_color(mark_label, lv_color_hex(on ? fill : theme::BG), 0);
        lv_obj_set_style_bg_opa(mark_label, on || !selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(mark_label, lv_color_hex(selected ? theme::ACCENT : SELECTED_TEXT), 0);
    }
}

static lv_obj_t* create_mark(lv_obj_t* row, ui::Mark mark)
{
    const board::Ui& ui = board::ui();
    lv_obj_t* label = lv_label_create(row);
    lv_obj_add_style(label, &s_mark, 0);
    int32_t line_h = lv_font_get_line_height(ui.small);
    if (mark == ui::Mark::On || mark == ui::Mark::Off) {
        lv_label_set_text(label, mark == ui::Mark::On ? "ON" : "OFF");
        lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(label, ui.gap * 2, 0);
        lv_obj_set_style_pad_ver(label, 1, 0);
    } else {
        int32_t side = line_h + 4;
        lv_label_set_text(label, mark == ui::Mark::Checked ? LV_SYMBOL_OK : "");
        lv_obj_set_size(label, side, side);
        lv_obj_set_style_radius(label, mark == ui::Mark::Chosen || mark == ui::Mark::NotChosen ? LV_RADIUS_CIRCLE : 3, 0);
    }
    lv_obj_set_user_data(label, (void*)(intptr_t)mark);
    lv_obj_add_flag(label, LV_OBJ_FLAG_USER_1);   // Tells paint_row() it's the mark
    return label;
}

void UiView::show_page(const ui::Page& page, const std::string& notice, int selected, int32_t bottom)
{
    if (!page_obj) {
        return;
    }
    const board::Ui& ui = board::ui();
    lv_obj_clean(page_obj);
    notice_label = NULL;
    field_slot_obj = NULL;
    first_row = NULL;
    row_count = 0;
    selected_row = -1;

    lv_obj_t* screen = lv_obj_get_parent(page_obj);
    lv_obj_set_size(page_obj, lv_obj_get_width(screen), lv_obj_get_height(screen) - bar_height - bottom);
    lv_obj_align(page_obj, LV_ALIGN_TOP_LEFT, 0, bar_height);

    if (!page.info.empty()) {
        lv_obj_t* info = text_label(page_obj, ui.input, theme::DIM, page.info.c_str());
        lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(info, lv_pct(100));
        lv_obj_set_style_pad_hor(info, ui.gap, 0);
    }

    // Messages since the last answer: a card with an accent edge
    notice_label = text_label(page_obj, ui.input, theme::TEXT, "");
    lv_label_set_long_mode(notice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notice_label, lv_pct(100));
    lv_obj_set_style_bg_color(notice_label, lv_color_hex(theme::SURFACE), 0);
    lv_obj_set_style_bg_opa(notice_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(notice_label, ui.gap, 0);
    lv_obj_set_style_pad_all(notice_label, ui.gap, 0);
    lv_obj_set_style_pad_left(notice_label, ui.gap * 2, 0);
    lv_obj_set_style_border_side(notice_label, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(notice_label, 3, 0);
    lv_obj_set_style_border_color(notice_label, lv_color_hex(theme::ACCENT), 0);
    set_notice(notice);

    if (!page.question.empty()) {
        lv_obj_t* question = text_label(page_obj, ui.title, theme::TEXT, page.question.c_str());
        lv_label_set_long_mode(question, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(question, lv_pct(100));
        lv_obj_set_style_pad_top(question, ui.gap, 0);
        lv_obj_set_style_pad_hor(question, ui.gap, 0);
        field_slot_obj = plain_obj(page_obj);
        lv_obj_set_size(field_slot_obj, lv_pct(100), LV_SIZE_CONTENT);
        if (!page.hint.empty()) {
            lv_obj_t* hint = text_label(page_obj, ui.input, theme::DIM, page.hint.c_str());
            lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(hint, lv_pct(100));
            lv_obj_set_style_pad_hor(hint, ui.gap, 0);
        }
    } else if (!page.hint.empty()) {
        lv_obj_t* hint = text_label(page_obj, ui.input, theme::DIM, page.hint.c_str());
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_pad_hor(hint, ui.gap, 0);
    }

    // Rows: key, icon + label, detail (under the label when it doesn't fit beside it), mark
    lv_obj_update_layout(page_obj);
    int32_t row_w = lv_obj_get_content_width(page_obj) - ui.gap * 4;
    int32_t key_w = text_width("00", ui.input);
    for (size_t i = 0; i < page.rows.size(); i++) {
        const ui::Row& r = page.rows[i];
        lv_obj_t* row = plain_obj(page_obj);
        lv_obj_add_style(row, &s_row, 0);
        lv_obj_add_style(row, &s_row_selected, LV_STATE_CHECKED);
        lv_obj_add_style(row, &s_row_pressed, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, this);
        if (i == 0) {
            first_row = row;
        }

        // Numbers are shortcuts worth showing; letter answers (y/n) are just noise
        bool number = !r.key.empty() && r.key.find_first_not_of("0123456789") == std::string::npos;
        lv_obj_t* key = text_label(row, ui.input, theme::DIM, number ? r.key.c_str() : "");
        lv_obj_set_width(key, key_w);

        std::string text = r.icon ? std::string(r.icon) + "  " + r.label : r.label;
        uint32_t color = r.tone == ui::Tone::Danger ? theme::DANGER : r.tone == ui::Tone::Back ? theme::DIM : theme::TEXT;
        lv_obj_t* label = text_label(row, ui.list, color, text.c_str());
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_user_data(label, (void*)(intptr_t)color);

        std::string detail = r.detail;
        if (r.mark == ui::Mark::More) {
            detail += detail.empty() ? LV_SYMBOL_RIGHT : "  " LV_SYMBOL_RIGHT;
        }
        // The mark stays on the label's line; the detail goes beside it if it fits, else under it
        bool marked = r.mark != ui::Mark::None && r.mark != ui::Mark::More;
        if (marked) {
            create_mark(row, r.mark);
        }
        if (!detail.empty()) {
            int32_t mark_w = marked ? lv_font_get_line_height(ui.small) + ui.gap * 6 : 0;
            int32_t beside = row_w - key_w - text_width(text.c_str(), ui.list) - mark_w - ui.gap * 6;
            lv_obj_t* d = text_label(row, ui.input, theme::DIM, detail.c_str());
            if (text_width(detail.c_str(), ui.input) > beside) {
                // Its own line under the label, lined up with it
                lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
                lv_obj_add_flag(d, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
                lv_obj_set_width(d, lv_pct(100));
                lv_obj_set_style_pad_left(d, key_w + ui.gap * 2, 0);
            } else if (marked) {
                lv_obj_move_to_index(d, 2);   // Key, label, detail, then the mark at the row's end
            }
        }
        row_count++;
        paint_row(i, false);
    }

    lv_obj_remove_flag(page_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(page_obj);
    // The first highlight doesn't scroll: the page opens at its top, so its text is read first
    lv_obj_scroll_to_y(page_obj, 0, LV_ANIM_OFF);
    selected_row = row_obj(selected) ? selected : -1;
    paint_row(selected_row, true);
}

void UiView::hide_page()
{
    if (!page_obj) {
        return;
    }
    lv_obj_add_flag(page_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(page_obj);   // Frees its objects until the next page
    notice_label = NULL;
    field_slot_obj = NULL;
    first_row = NULL;
    row_count = 0;
    selected_row = -1;
}

void UiView::set_notice(const std::string& notice)
{
    if (!notice_label) {
        return;
    }
    if (notice.empty()) {
        lv_obj_add_flag(notice_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(notice_label, notice.c_str());
    lv_obj_remove_flag(notice_label, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* UiView::row_obj(int row) const
{
    if (!first_row || row < 0 || row >= row_count) {
        return NULL;
    }
    return lv_obj_get_child(page_obj, lv_obj_get_index(first_row) + row);
}

// Colours of a row's labels and mark, white on the accent bar while highlighted
void UiView::paint_row(int row, bool selected)
{
    lv_obj_t* obj = row_obj(row);
    if (!obj) {
        return;
    }
    if (selected) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_USER_1)) {
            paint_mark(child, (ui::Mark)(intptr_t)lv_obj_get_user_data(child), selected);
        } else if (i == 1) {
            uint32_t color = (uint32_t)(uintptr_t)lv_obj_get_user_data(child);
            lv_obj_set_style_text_color(child, lv_color_hex(selected ? SELECTED_TEXT : color), 0);
        } else {
            lv_obj_set_style_text_color(child, lv_color_hex(selected ? SELECTED_DIM : theme::DIM), 0);
        }
    }
}

void UiView::select(int row)
{
    if (row == selected_row && row_obj(row)) {
        return;
    }
    paint_row(selected_row, false);
    selected_row = row_obj(row) ? row : -1;
    lv_obj_t* obj = row_obj(selected_row);
    if (obj) {
        paint_row(selected_row, true);
        lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
    }
}

bool UiView::scroll(int direction)
{
    if (!page_shown() || direction == 0) {
        return false;
    }
    int32_t before = lv_obj_get_scroll_y(page_obj);
    int32_t step = 3 * lv_font_get_line_height(board::ui().input);
    lv_obj_scroll_by_bounded(page_obj, 0, direction > 0 ? step : -step, LV_ANIM_OFF);
    return lv_obj_get_scroll_y(page_obj) != before;
}

void UiView::row_clicked_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    lv_obj_t* row = (lv_obj_t*)lv_event_get_current_target(e);
    if (view && view->on_tap) {
        view->on_tap((int)(intptr_t)lv_obj_get_user_data(row));
    }
}

void UiView::back_clicked_cb(lv_event_t* e)
{
    UiView* view = (UiView*)lv_event_get_user_data(e);
    if (view && view->on_back) {
        view->on_back();
    }
}
