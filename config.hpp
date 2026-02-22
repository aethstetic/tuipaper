#pragma once

#include <string>
#include <map>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace config {

struct Config {
    std::string last_directory;
    std::string backend_override;
    std::string fit_mode;
    std::string monitor;
    std::map<int, std::string> workspace_wallpapers;
    std::string workspace_fit_mode;
    std::string wallpaper_mode;
    int cycle_interval = 5;
};

inline std::string config_dir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        return std::string(xdg) + "/tuipaper";
    }
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    return std::string(home) + "/.config/tuipaper";
}

inline std::string config_path() {
    return config_dir() + "/config";
}

inline void mkdirp(const std::string& path) {
    // Simple recursive mkdir
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
}

inline Config load() {
    Config cfg;
    std::ifstream f(config_path());
    if (!f.is_open()) return cfg;

    std::string line;
    while (std::getline(f, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim whitespace
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());

        if (key == "last_directory") cfg.last_directory = val;
        else if (key == "backend") {
            // Migrate old backend values
            if (val == "hyprpaper" || val == "swww") val = "native";
            cfg.backend_override = val;
        }
        else if (key == "fit_mode") cfg.fit_mode = val;
        else if (key == "monitor") cfg.monitor = val;
        else if (key == "workspace_fit") cfg.workspace_fit_mode = val;
        else if (key == "wallpaper_mode") cfg.wallpaper_mode = val;
        else if (key == "cycle_interval") {
            int v = std::atoi(val.c_str());
            if (v > 0) cfg.cycle_interval = v;
        }
        else if (key.substr(0, 10) == "workspace_") {
            int ws_id = std::atoi(key.substr(10).c_str());
            if (ws_id > 0 && ws_id <= 10 && !val.empty()) {
                cfg.workspace_wallpapers[ws_id] = val;
            }
        }
    }
    return cfg;
}

inline void save(const Config& cfg) {
    mkdirp(config_dir());
    std::ofstream f(config_path());
    if (!f.is_open()) return;

    if (!cfg.last_directory.empty())
        f << "last_directory=" << cfg.last_directory << "\n";
    if (!cfg.backend_override.empty())
        f << "backend=" << cfg.backend_override << "\n";
    if (!cfg.fit_mode.empty())
        f << "fit_mode=" << cfg.fit_mode << "\n";
    if (!cfg.monitor.empty())
        f << "monitor=" << cfg.monitor << "\n";
    for (auto& [ws_id, path] : cfg.workspace_wallpapers) {
        f << "workspace_" << ws_id << "=" << path << "\n";
    }
    if (!cfg.workspace_fit_mode.empty())
        f << "workspace_fit=" << cfg.workspace_fit_mode << "\n";
    if (!cfg.wallpaper_mode.empty())
        f << "wallpaper_mode=" << cfg.wallpaper_mode << "\n";
    f << "cycle_interval=" << cfg.cycle_interval << "\n";
}

} // namespace config
