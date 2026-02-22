#include <string>
#include <cstdlib>
#include <random>

#include "tui.hpp"
#include "browser.hpp"
#include "preview.hpp"
#include "wallpaper.hpp"
#include "config.hpp"

enum class WallpaperMode { STATIC, CYCLE, RANDOM };

static bool needs_redraw = true;
static std::string status_message;
static int status_timeout = 0;
static WallpaperMode wp_mode = WallpaperMode::STATIC;

static const char* wallpaper_mode_name(WallpaperMode m) {
    switch (m) {
        case WallpaperMode::STATIC: return "static";
        case WallpaperMode::CYCLE:  return "cycle";
        case WallpaperMode::RANDOM: return "random";
    }
    return "static";
}

static int cycle_timer = 0;
static bool interval_input = false;
static std::string interval_buf;

static bool popup_open = false;
static int popup_selected = 0;
static std::vector<wallpaper::Workspace> popup_workspaces;

void draw_popup(const browser::BrowserState& state, const tui::TermSize& size,
               const config::Config& cfg) {
    int w = size.cols;
    int h = size.rows;
    int num_ws = (int)popup_workspaces.size();
    int popup_w = 60;
    int popup_h = num_ws + 6;
    int popup_x = (w - popup_w) / 2;
    int popup_y = (h - popup_h) / 2;
    if (popup_x < 1) popup_x = 1;
    if (popup_y < 1) popup_y = 1;

    tui::reset_style();
    for (int row = popup_y; row < popup_y + popup_h; row++) {
        tui::move_cursor(row, popup_x);
        for (int c = 0; c < popup_w; c++) tui::write(" ");
    }

    tui::set_dim();
    tui::draw_box(popup_x, popup_y, popup_w, popup_h);
    tui::reset_style();

    std::string ptitle = " Assign to Workspace ";
    int title_x = popup_x + (popup_w - (int)ptitle.size()) / 2;
    tui::move_cursor(popup_y, title_x);
    tui::set_bold();
    tui::set_accent();
    tui::write(ptitle);
    tui::reset_style();

    {
        std::string sel_name = state.selected_path();
        auto slash = sel_name.rfind('/');
        if (slash != std::string::npos) sel_name = sel_name.substr(slash + 1);
        std::string img_line = "Image: " + sel_name;
        int max_img = popup_w - 4;
        if ((int)img_line.size() > max_img)
            img_line = img_line.substr(0, max_img - 3) + "...";
        tui::move_cursor(popup_y + 1, popup_x + 2);
        tui::set_dim();
        tui::write(img_line);
        tui::reset_style();
    }

    for (int i = 0; i < num_ws; i++) {
        int row = popup_y + 3 + i;
        tui::move_cursor(row, popup_x + 1);

        for (int c = 0; c < popup_w - 2; c++) tui::write(" ");
        tui::move_cursor(row, popup_x + 2);

        bool is_sel = (i == popup_selected);
        if (is_sel) {
            tui::set_reverse();
            tui::set_bold();
            tui::set_accent();
        }

        auto& ws = popup_workspaces[i];
        std::string prefix = "  [" + std::to_string(ws.id) + "] ";

        auto it = cfg.workspace_wallpapers.find(ws.id);
        std::string assigned;
        if (it != cfg.workspace_wallpapers.end()) {
            assigned = it->second;
            auto sl = assigned.rfind('/');
            if (sl != std::string::npos) assigned = assigned.substr(sl + 1);
        }

        int content_w = popup_w - 4;
        std::string label = prefix;
        if (!assigned.empty()) {
            int remaining = content_w - (int)prefix.size() - 2;
            if ((int)assigned.size() > remaining)
                assigned = assigned.substr(0, remaining - 3) + "...";
            label += assigned;
        } else {
            label += ws.name;
        }

        while ((int)label.size() < content_w) label += " ";

        tui::write(label);
        tui::reset_style();
    }

    int hint_row = popup_y + 3 + num_ws;
    tui::move_cursor(hint_row, popup_x + 2);
    tui::set_dim();
    tui::write("[enter] assign  [d] unassign  [esc] close");
    tui::reset_style();
}

void draw_ui(const browser::BrowserState& state, const tui::TermSize& size,
            wallpaper::Backend backend, wallpaper::FitMode fit_mode,
            const std::string& monitor_display, bool redraw_image,
            const config::Config& cfg, int cycle_interval) {
    int w = size.cols;
    int h = size.rows;

    if (w < 40 || h < 10) {
        tui::clear_screen();
        tui::move_cursor(1, 1);
        tui::write("Terminal too small");
        return;
    }

    int max_name_len = 0;
    for (auto& entry : state.filtered) {
        int len = (int)entry.name.size() + (entry.is_dir ? 1 : 0);
        if (len > max_name_len) max_name_len = len;
    }
    int split_col = max_name_len + 6;
    if (split_col < 20) split_col = 20;
    if (split_col > w / 2) split_col = w / 2;
    int box_h = h;

    int preview_x = split_col + 2;
    int preview_w = w - split_col - 2;
    int preview_y = 2;
    int preview_h = box_h - 2;

    bool show_file_info = !state.filtered.empty() && !state.selected_is_dir();

    int img_box_w = preview_w - 1;
    int img_box_h = preview_h;
    if (show_file_info && img_box_h > 6) {
        img_box_h -= 2;
    }

    preview::ImageDimensions img_dims;
    if (show_file_info) {
        img_dims = preview::get_image_dimensions(state.selected_path());
        if (img_dims.width > 0 && img_dims.height > 0) {
            auto cell_px = tui::get_cell_pixel_size();
            auto fitted = preview::fit_image_to_cells(
                img_dims.width, img_dims.height,
                img_box_w - 2, img_box_h - 2,
                cell_px.width, cell_px.height);
            img_box_w = fitted.cols + 2;
            img_box_h = fitted.rows + 2;
        }
    }

    if (popup_open && !popup_workspaces.empty()) {
        tui::hide_cursor();
        draw_popup(state, size, cfg);
        return;
    }

    tui::hide_cursor();
    tui::clear_screen();

    tui::reset_style();
    tui::set_dim();
    tui::draw_box(1, 1, w, box_h);
    for (int y = 2; y < box_h; y++) {
        tui::move_cursor(y, split_col);
        tui::write("\u2502");
    }
    tui::move_cursor(box_h, split_col);
    tui::write("\u2534");
    tui::reset_style();

    tui::move_cursor(1, 1);
    std::string dir_display = browser::shorten_home(state.current_dir);
    std::string backend_display = wallpaper::backend_name(backend);
    std::string fit_display = wallpaper::fit_mode_name(fit_mode);
    std::string ws_indicator;
    if (!cfg.workspace_wallpapers.empty()) {
        ws_indicator = " [W:" + std::to_string(cfg.workspace_wallpapers.size()) + "]";
    }
    std::string mode_display = wallpaper_mode_name(wp_mode);
    if (wp_mode == WallpaperMode::CYCLE)
        mode_display += " " + std::to_string(cycle_interval) + "m";
    std::string title_text = " tuipaper \u2500\u2500 " + dir_display
                           + " \u2500\u2500 [" + backend_display + "] ["
                           + fit_display + "] [" + monitor_display + "] ["
                           + mode_display + "]"
                           + ws_indicator + " ";
    std::string title = "\u250c\u2500" + title_text;

    int title_len = 0;
    for (size_t i = 0; i < title.size(); i++) {
        if ((title[i] & 0xC0) != 0x80) title_len++;
    }
    while (title_len < w - 1) {
        title += "\u2500";
        title_len++;
    }
    title += "\u2510";

    tui::set_dim();
    tui::write(title);
    tui::reset_style();

    int list_x = 2;
    int list_w = split_col - 2;
    int list_y = 2;
    int list_h = box_h - 2;

    int scroll = state.scroll_offset;
    int sel = state.selected;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + list_h) scroll = sel - list_h + 1;

    if (state.dir_missing) {
        int mid_y = list_y + list_h / 2 - 1;
        int mid_x = (w / 2);

        std::string msg1 = "Directory does not exist:";
        std::string msg2 = browser::shorten_home(state.current_dir);
        std::string msg3 = "Press 'c' to create it";

        tui::move_cursor(mid_y, mid_x - (int)msg1.size() / 2);
        tui::set_dim();
        tui::write(msg1);
        tui::reset_style();

        tui::move_cursor(mid_y + 1, mid_x - (int)msg2.size() / 2);
        tui::set_bold();
        tui::write(msg2);
        tui::reset_style();

        tui::move_cursor(mid_y + 3, mid_x - (int)msg3.size() / 2);
        tui::set_accent();
        tui::set_bold();
        tui::write(msg3);
        tui::reset_style();
    } else {
        for (int i = 0; i < list_h; i++) {
            int idx = scroll + i;
            tui::move_cursor(list_y + i, list_x);

            for (int c = 0; c < list_w; c++) tui::write(" ");
            tui::move_cursor(list_y + i, list_x);

            if (idx < (int)state.filtered.size()) {
                bool is_selected = (idx == sel);
                auto& entry = state.filtered[idx];

                if (is_selected) {
                    tui::set_reverse();
                    tui::set_bold();
                }

                if (is_selected) {
                    tui::set_accent();
                    tui::write(" > ");
                } else {
                    tui::write("   ");
                }

                if (entry.is_dir) {
                    tui::set_accent();
                    std::string display = entry.name + "/";
                    if ((int)display.size() > list_w - 4)
                        display = display.substr(0, list_w - 7) + "...";
                    tui::write(display);
                } else {
                    tui::set_default_fg();
                    std::string display = entry.name;
                    if ((int)display.size() > list_w - 4)
                        display = display.substr(0, list_w - 7) + "...";
                    tui::write(display);
                }

                tui::reset_style();
            }
        }

        if (state.filtered.empty() || state.selected_is_dir()) {
            std::string msg = state.filtered.empty() ? "[no images]" : "[directory]";
            int cx = preview_x + (preview_w - 1 - (int)msg.size()) / 2;
            tui::move_cursor(preview_y + preview_h / 2, cx);
            tui::set_dim();
            tui::write(msg);
            tui::reset_style();
        }
        if (show_file_info && img_box_w >= 3 && img_box_h >= 3) {
            tui::set_dim();
            tui::set_accent();
            tui::draw_box(preview_x, preview_y, img_box_w, img_box_h);
            tui::reset_style();

            int info_row = preview_y + img_box_h;
            int info_w = img_box_w - 2;

            std::string fullpath = state.selected_path();
            std::string basename = fullpath;
            auto slash = basename.rfind('/');
            if (slash != std::string::npos) basename = basename.substr(slash + 1);
            if ((int)basename.size() > info_w)
                basename = basename.substr(0, info_w - 3) + "...";

            tui::move_cursor(info_row, preview_x + 1);
            tui::set_dim();
            tui::write(basename);
            tui::reset_style();

            if (img_dims.width > 0 && img_dims.height > 0) {
                std::string res = std::to_string(img_dims.width) + "x" + std::to_string(img_dims.height);
                tui::move_cursor(info_row + 1, preview_x + 1);
                tui::set_dim();
                tui::write(res);
                tui::reset_style();
            }
        }
    }

    {
        std::string status_text;
        bool use_accent = false;
        bool use_green = false;
        bool use_dim = false;

        if (interval_input) {
            status_text = "interval (min): " + interval_buf + "\u2588";
            use_accent = true;
        } else if (state.searching) {
            status_text = "/" + state.search_query + "\u2588";
            use_accent = true;
        } else if (!status_message.empty()) {
            status_text = status_message;
            use_green = true;
        } else {
            std::string base = "[q] quit  [enter] set  [/] search  [h/l] navigate  [g] mode  [f] fit  [m] monitor  [w] workspace";
            if (wp_mode == WallpaperMode::CYCLE)
                base += "  [i] interval";
            status_text = state.dir_missing
                ? "[q] quit  [c] create directory"
                : base;
            use_dim = true;
        }

        if ((int)status_text.size() > w - 6)
            status_text = status_text.substr(0, w - 9) + "...";

        std::string padded = " " + status_text + " ";
        tui::move_cursor(h, 3);

        if (use_accent) { tui::set_bold(); tui::set_accent(); }
        else if (use_green) { tui::set_bold(); tui::set_fg(2); }
        else if (use_dim) { tui::set_dim(); }

        tui::write(padded);
        tui::reset_style();
    }

    /* kitten icat overlay: image MUST render after clear_screen() */
    if (redraw_image && !state.dir_missing) {
        preview::clear_images();

        if (!state.filtered.empty() && !state.selected_is_dir()
            && img_box_w >= 3 && img_box_h >= 3) {
            std::string path = state.selected_path();
            preview::display_image(path, preview_y + 1, preview_x + 1,
                                  img_box_w - 2, img_box_h - 2);
        }
    }
}

int main(int argc, char* argv[]) {
    auto cfg = config::load();

    bool disable_color = false;
    std::string accent_hex;
    std::string start_dir;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--disable-color") {
            disable_color = true;
        } else if (arg == "--accent" && i + 1 < argc) {
            accent_hex = argv[++i];
        } else if (arg == "--interval" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            if (v > 0) cfg.cycle_interval = v;
        } else if (arg[0] != '-') {
            start_dir = browser::expand_home(arg);
        }
    }

    tui::init_color_mode(disable_color, accent_hex);

    if (start_dir.empty()) {
        if (!cfg.last_directory.empty()) {
            start_dir = cfg.last_directory;
        } else {
            start_dir = browser::expand_home("~/wallpapers");
        }
    }

    wallpaper::Backend backend = wallpaper::detect_backend();
    if (!cfg.backend_override.empty()) {
        if (cfg.backend_override == "native") backend = wallpaper::Backend::NATIVE;
        else if (cfg.backend_override == "sway") backend = wallpaper::Backend::SWAY;
        else if (cfg.backend_override == "feh") backend = wallpaper::Backend::FEH;
    }

    wallpaper::FitMode current_fit = wallpaper::FitMode::FILL;
    if (!cfg.fit_mode.empty()) {
        current_fit = wallpaper::fit_mode_from_name(cfg.fit_mode);
    }
    if (!wallpaper::backend_supports_fit(backend, current_fit)) {
        current_fit = wallpaper::FitMode::FILL;
    }

    if (cfg.wallpaper_mode == "cycle") wp_mode = WallpaperMode::CYCLE;
    else if (cfg.wallpaper_mode == "random") wp_mode = WallpaperMode::RANDOM;

    auto monitors = wallpaper::detect_monitors(backend);
    int monitor_index = 0;
    if (!cfg.monitor.empty() && !monitors.empty()) {
        for (int i = 0; i < (int)monitors.size(); i++) {
            if (monitors[i].name == cfg.monitor) {
                monitor_index = i + 1;
                break;
            }
        }
    }

    wallpaper::kill_daemon();

    browser::BrowserState state;
    state.load(start_dir);

    std::string previewed_path;
    bool image_changed = true;

    cycle_timer = cfg.cycle_interval * 60 * 10;

    tui::enable_raw_mode();
    tui::enter_alt_screen();
    tui::hide_cursor();

    auto size = tui::get_terminal_size();

    tui::install_resize_handler([&]() {
        needs_redraw = true;
        image_changed = true;
    });

    needs_redraw = true;

    bool running = true;
    while (running) {
        if (tui::check_resize()) {
            size = tui::get_terminal_size();
            preview::clear_images();
            previewed_path.clear();
            image_changed = true;
            needs_redraw = true;
        }

        if (needs_redraw) {
            int list_h = size.rows - 2;
            state.adjust_scroll(list_h);

            std::string current_path = state.selected_path();
            if (current_path != previewed_path) {
                previewed_path = current_path;
                image_changed = true;
            }

            std::string mon_display = (monitor_index == 0) ? "All"
                : monitors[monitor_index - 1].name;
            draw_ui(state, size, backend, current_fit, mon_display, image_changed, cfg, cfg.cycle_interval);
            image_changed = false;
            needs_redraw = false;
        }

        if (status_timeout > 0) {
            status_timeout--;
            if (status_timeout == 0) {
                status_message.clear();
                needs_redraw = true;
            }
        }

        if (wp_mode == WallpaperMode::CYCLE && !state.filtered.empty()) {
            cycle_timer--;
            if (cycle_timer <= 0) {
                int n = (int)state.filtered.size();
                int start = state.selected;
                for (int j = 1; j <= n; j++) {
                    int idx = (start + j) % n;
                    if (!state.filtered[idx].is_dir) {
                        state.selected = idx;
                        break;
                    }
                }
                std::string path = state.selected_path();
                std::string mon = (monitor_index == 0) ? "*"
                    : monitors[monitor_index - 1].name;
                wallpaper::set_wallpaper(path, backend, current_fit, mon);
                cycle_timer = cfg.cycle_interval * 60 * 10;
                image_changed = true;
                needs_redraw = true;
            }
        }

        int key = tui::read_key();
        if (key == tui::KEY_NONE) continue;

        if (interval_input) {
            if (key == tui::KEY_ESCAPE) {
                interval_input = false;
                interval_buf.clear();
                needs_redraw = true;
            } else if (key == tui::KEY_ENTER) {
                int v = std::atoi(interval_buf.c_str());
                if (v > 0) {
                    cfg.cycle_interval = v;
                    cycle_timer = v * 60 * 10;
                    status_message = "Interval: " + std::to_string(v) + "m";
                } else {
                    status_message = "Invalid interval";
                }
                status_timeout = 30;
                interval_input = false;
                interval_buf.clear();
                needs_redraw = true;
            } else if (key == tui::KEY_BACKSPACE) {
                if (!interval_buf.empty()) interval_buf.pop_back();
                needs_redraw = true;
            } else if (key >= '0' && key <= '9') {
                interval_buf += (char)key;
                needs_redraw = true;
            }
            continue;
        }

        if (state.searching) {
            if (key == tui::KEY_ESCAPE || key == tui::KEY_ENTER) {
                state.searching = false;
                if (key == tui::KEY_ESCAPE) {
                    state.search_query.clear();
                    state.apply_filter();
                    state.selected = 0;
                }
                needs_redraw = true;
            } else if (key == tui::KEY_BACKSPACE) {
                if (!state.search_query.empty()) {
                    state.search_query.pop_back();
                    state.apply_filter();
                    state.selected = 0;
                }
                needs_redraw = true;
            } else if (key >= 32 && key < 127) {
                state.search_query += (char)key;
                state.apply_filter();
                state.selected = 0;
                needs_redraw = true;
            }
            continue;
        }

        if (popup_open) {
            switch (key) {
                case 'j':
                case tui::KEY_DOWN:
                    if (popup_selected < (int)popup_workspaces.size() - 1)
                        popup_selected++;
                    needs_redraw = true;
                    break;
                case 'k':
                case tui::KEY_UP:
                    if (popup_selected > 0)
                        popup_selected--;
                    needs_redraw = true;
                    break;
                case tui::KEY_ENTER: {
                    int ws_id = popup_workspaces[popup_selected].id;
                    std::string path = state.selected_path();
                    cfg.workspace_wallpapers[ws_id] = path;
                    cfg.workspace_fit_mode = wallpaper::fit_mode_name(current_fit);
                    status_message = "Workspace " + std::to_string(ws_id) + " assigned";
                    status_timeout = 30;
                    int active_ws = wallpaper::get_active_workspace_id();
                    if (active_ws == ws_id) {
                        wallpaper::set_wallpaper(path, backend, current_fit, "*");
                    }
                    image_changed = true;
                    needs_redraw = true;
                    break;
                }
                case tui::KEY_ESCAPE:
                case 'w':
                    popup_open = false;
                    image_changed = true;
                    needs_redraw = true;
                    break;
                case 'd': {
                    int ws_id = popup_workspaces[popup_selected].id;
                    cfg.workspace_wallpapers.erase(ws_id);
                    status_message = "Workspace " + std::to_string(ws_id) + " unassigned";
                    status_timeout = 30;
                    needs_redraw = true;
                    break;
                }
            }
            continue;
        }

        switch (key) {
            case 'q':
            case tui::KEY_ESCAPE:
                running = false;
                break;

            case 'j':
            case tui::KEY_DOWN:
                state.move_down();
                needs_redraw = true;
                break;

            case 'k':
            case tui::KEY_UP:
                state.move_up();
                needs_redraw = true;
                break;

            case 'g': {
                if (wp_mode == WallpaperMode::STATIC)
                    wp_mode = WallpaperMode::CYCLE;
                else if (wp_mode == WallpaperMode::CYCLE)
                    wp_mode = WallpaperMode::RANDOM;
                else
                    wp_mode = WallpaperMode::STATIC;
                if (wp_mode == WallpaperMode::CYCLE)
                    cycle_timer = cfg.cycle_interval * 60 * 10;
                status_message = std::string("Mode: ") + wallpaper_mode_name(wp_mode);
                status_timeout = 30;
                needs_redraw = true;
                break;
            }

            case 'l':
            case tui::KEY_RIGHT:
                if (state.selected_is_dir()) {
                    state.enter_selected();
                    preview::clear_images();
                    previewed_path.clear();
                    image_changed = true;
                    needs_redraw = true;
                }
                break;

            case 'h':
            case tui::KEY_LEFT:
            case tui::KEY_BACKSPACE:
                state.go_up();
                preview::clear_images();
                previewed_path.clear();
                image_changed = true;
                needs_redraw = true;
                break;

            case tui::KEY_ENTER:
                if (state.selected_is_dir()) {
                    state.enter_selected();
                    preview::clear_images();
                    previewed_path.clear();
                    image_changed = true;
                    needs_redraw = true;
                } else if (!state.filtered.empty()) {
                    if (wp_mode == WallpaperMode::CYCLE) {
                        int n = (int)state.filtered.size();
                        int start = state.selected;
                        for (int i = 1; i <= n; i++) {
                            int idx = (start + i) % n;
                            if (!state.filtered[idx].is_dir) {
                                state.selected = idx;
                                break;
                            }
                        }
                        cycle_timer = cfg.cycle_interval * 60 * 10;
                    } else if (wp_mode == WallpaperMode::RANDOM) {
                        std::vector<int> candidates;
                        for (int i = 0; i < (int)state.filtered.size(); i++) {
                            if (!state.filtered[i].is_dir) candidates.push_back(i);
                        }
                        if (!candidates.empty()) {
                            static std::mt19937 rng(std::random_device{}());
                            std::uniform_int_distribution<int> dist(0, (int)candidates.size() - 1);
                            state.selected = candidates[dist(rng)];
                        }
                    }
                    std::string path = state.selected_path();
                    std::string mon = (monitor_index == 0) ? "*"
                        : monitors[monitor_index - 1].name;
                    auto result = wallpaper::set_wallpaper(path, backend, current_fit, mon);
                    status_message = result.message;
                    status_timeout = 30;
                    image_changed = true;
                    needs_redraw = true;
                }
                break;

            case 'f':
                current_fit = wallpaper::next_fit_mode(current_fit, backend);
                image_changed = true;
                needs_redraw = true;
                break;

            case 'm':
                if (monitors.empty()) {
                    monitor_index = 0;
                } else {
                    monitor_index = (monitor_index + 1) % ((int)monitors.size() + 1);
                }
                image_changed = true;
                needs_redraw = true;
                break;

            case '/':
                state.searching = true;
                state.search_query.clear();
                needs_redraw = true;
                break;

            case 'c':
                if (state.dir_missing) {
                    if (state.create_current_dir()) {
                        status_message = "Created " + browser::shorten_home(state.current_dir);
                    } else {
                        status_message = "Failed to create directory";
                    }
                    status_timeout = 30;
                    needs_redraw = true;
                }
                break;

            case 'w':
                if (!state.filtered.empty() && !state.selected_is_dir() &&
                    backend == wallpaper::Backend::NATIVE &&
                    getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
                    popup_workspaces = wallpaper::get_workspaces();
                    popup_selected = 0;
                    popup_open = true;
                    preview::clear_images();
                    needs_redraw = true;
                }
                break;

            case 'i':
                if (wp_mode == WallpaperMode::CYCLE) {
                    interval_input = true;
                    interval_buf.clear();
                    needs_redraw = true;
                }
                break;
        }
    }

    cfg.last_directory = state.current_dir;
    cfg.fit_mode = wallpaper::fit_mode_name(current_fit);
    cfg.monitor = (monitor_index > 0 && monitor_index <= (int)monitors.size())
        ? monitors[monitor_index - 1].name : "";
    cfg.wallpaper_mode = wallpaper_mode_name(wp_mode);
    config::save(cfg);

    bool need_daemon = (wp_mode == WallpaperMode::CYCLE) ||
        (!cfg.workspace_wallpapers.empty() &&
         backend == wallpaper::Backend::NATIVE &&
         getenv("HYPRLAND_INSTANCE_SIGNATURE"));

    if (need_daemon) {
        wallpaper::spawn_workspace_daemon(backend);
    } else {
        wallpaper::kill_daemon();
    }

    preview::clear_images();
    tui::show_cursor();
    tui::exit_alt_screen();
    tui::disable_raw_mode();

    return 0;
}
