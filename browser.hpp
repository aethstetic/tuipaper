#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace browser {

struct Entry {
    std::string name;
    bool is_dir;
};

static const std::vector<std::string> IMAGE_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".bmp", ".webp",
    ".gif", ".tiff", ".tif", ".avif", ".svg"
};

inline std::string to_lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = tolower(c);
    return r;
}

inline bool is_image_file(const std::string& name) {
    std::string lower = to_lower(name);
    for (auto& ext : IMAGE_EXTENSIONS) {
        if (lower.size() >= ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

inline std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + path.substr(1);
}

inline std::string resolve_path(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) {
        return std::string(resolved);
    }
    return path;
}

inline std::string parent_dir(const std::string& path) {
    if (path == "/") return "/";
    std::string p = path;
    if (p.back() == '/') p.pop_back();
    auto pos = p.rfind('/');
    if (pos == std::string::npos) return "/";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

inline std::string shorten_home(const std::string& path) {
    const char* home = getenv("HOME");
    if (!home) return path;
    std::string h(home);
    if (path.compare(0, h.size(), h) == 0) {
        return "~" + path.substr(h.size());
    }
    return path;
}

inline std::vector<Entry> list_directory(const std::string& path) {
    std::vector<Entry> entries;
    DIR* dir = opendir(path.c_str());
    if (!dir) return entries;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        // Skip hidden files
        if (!name.empty() && name[0] == '.') continue;

        std::string full = path + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        if (is_dir || is_image_file(name)) {
            entries.push_back({name, is_dir});
        }
    }
    closedir(dir);

    // Sort: directories first, then files, both alphabetical
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return to_lower(a.name) < to_lower(b.name);
    });

    return entries;
}

struct BrowserState {
    std::string current_dir;
    std::vector<Entry> entries;
    std::vector<Entry> filtered; // after search filter
    int selected = 0;
    int scroll_offset = 0;
    bool searching = false;
    std::string search_query;

    bool dir_missing = false;

    void load(const std::string& dir) {
        current_dir = dir;
        // Try to resolve, but keep the raw path if it doesn't exist
        std::string resolved = resolve_path(dir);
        if (!resolved.empty()) current_dir = resolved;

        struct stat st;
        if (stat(current_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            dir_missing = true;
            entries.clear();
            filtered.clear();
            selected = 0;
            scroll_offset = 0;
            return;
        }

        dir_missing = false;
        entries = list_directory(current_dir);
        apply_filter();
        selected = 0;
        scroll_offset = 0;
    }

    bool create_current_dir() {
        if (!dir_missing) return true;
        // Recursive mkdir
        std::string cur;
        for (size_t i = 0; i < current_dir.size(); i++) {
            cur += current_dir[i];
            if (current_dir[i] == '/' || i == current_dir.size() - 1) {
                mkdir(cur.c_str(), 0755);
            }
        }
        struct stat st;
        if (stat(current_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            dir_missing = false;
            entries = list_directory(current_dir);
            apply_filter();
            selected = 0;
            scroll_offset = 0;
            return true;
        }
        return false;
    }

    void apply_filter() {
        if (search_query.empty()) {
            filtered = entries;
        } else {
            filtered.clear();
            std::string q = to_lower(search_query);
            for (auto& e : entries) {
                if (to_lower(e.name).find(q) != std::string::npos) {
                    filtered.push_back(e);
                }
            }
        }
    }

    void move_up() {
        if (selected > 0) selected--;
    }

    void move_down() {
        if (selected < (int)filtered.size() - 1) selected++;
    }

    void enter_selected() {
        if (filtered.empty()) return;
        auto& e = filtered[selected];
        if (e.is_dir) {
            load(current_dir + "/" + e.name);
        }
    }

    void go_up() {
        std::string parent = parent_dir(current_dir);
        if (parent != current_dir) {
            load(parent);
        }
    }

    std::string selected_path() const {
        if (filtered.empty()) return "";
        return current_dir + "/" + filtered[selected].name;
    }

    bool selected_is_dir() const {
        if (filtered.empty()) return false;
        return filtered[selected].is_dir;
    }

    // Ensure selected item is visible within the scroll window
    void adjust_scroll(int visible_rows) {
        if (selected < scroll_offset) {
            scroll_offset = selected;
        }
        if (selected >= scroll_offset + visible_rows) {
            scroll_offset = selected - visible_rows + 1;
        }
    }
};

} // namespace browser
