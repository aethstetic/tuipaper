#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

namespace wallpaper {

enum class Backend {
    NATIVE,
    SWAY,
    FEH,
    NONE,
};

enum class FitMode { FILL, COVER, CONTAIN, CENTER, TILE };

inline std::string fit_mode_name(FitMode f) {
    switch (f) {
        case FitMode::FILL: return "fill";
        case FitMode::COVER: return "cover";
        case FitMode::CONTAIN: return "contain";
        case FitMode::CENTER: return "center";
        case FitMode::TILE: return "tile";
    }
    return "fill";
}

inline FitMode fit_mode_from_name(const std::string& name) {
    if (name == "cover") return FitMode::COVER;
    if (name == "contain") return FitMode::CONTAIN;
    if (name == "center") return FitMode::CENTER;
    if (name == "tile") return FitMode::TILE;
    return FitMode::FILL;
}

inline bool backend_supports_fit(Backend b, FitMode f) {
    (void)f;
    switch (b) {
        case Backend::NATIVE: return true;
        case Backend::SWAY:   return true;
        case Backend::FEH:    return true;
        case Backend::NONE:   return true;
    }
    return true;
}

inline FitMode next_fit_mode(FitMode current, Backend backend) {
    constexpr FitMode modes[] = {FitMode::FILL, FitMode::COVER, FitMode::CONTAIN, FitMode::CENTER, FitMode::TILE};
    constexpr int n = 5;
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (modes[i] == current) { idx = i; break; }
    }
    for (int i = 1; i <= n; i++) {
        FitMode next = modes[(idx + i) % n];
        if (backend_supports_fit(backend, next)) return next;
    }
    return current;
}

inline std::string backend_name(Backend b) {
    switch (b) {
        case Backend::NATIVE: return "native";
        case Backend::SWAY:   return "swaymsg";
        case Backend::FEH:    return "feh";
        case Backend::NONE:   return "none";
    }
    return "unknown";
}

inline bool command_exists(const std::string& cmd) {
    std::string path_env = getenv("PATH") ? getenv("PATH") : "";
    std::string::size_type start = 0;
    while (start < path_env.size()) {
        auto end = path_env.find(':', start);
        if (end == std::string::npos) end = path_env.size();
        std::string dir = path_env.substr(start, end - start);
        std::string full = dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}

inline bool run_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::vector<char*> c_args;
        for (auto& a : args) {
            c_args.push_back(const_cast<char*>(a.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline std::string run_command_capture(const std::vector<std::string>& args) {
    if (args.empty()) return "";

    int pipefd[2];
    if (pipe(pipefd) < 0) return "";

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::vector<char*> c_args;
        for (auto& a : args) {
            c_args.push_back(const_cast<char*>(a.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(c_args[0], c_args.data());
        _exit(127);
    }

    close(pipefd[1]);

    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        output.append(buf, n);
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return output;
}

struct Monitor {
    std::string name;
};

inline std::vector<Monitor> detect_monitors(Backend backend) {
    std::vector<Monitor> monitors;

    switch (backend) {
        case Backend::NATIVE: {
            if (getenv("HYPRLAND_INSTANCE_SIGNATURE") && command_exists("hyprctl")) {
                std::string out = run_command_capture({"hyprctl", "monitors"});
                std::string::size_type pos = 0;
                while ((pos = out.find("Monitor ", pos)) != std::string::npos) {
                    pos += 8;
                    auto end = out.find_first_of(" (\n", pos);
                    if (end == std::string::npos) break;
                    std::string name = out.substr(pos, end - pos);
                    if (!name.empty()) {
                        monitors.push_back({name});
                    }
                    pos = end;
                }
            }
            break;
        }
        case Backend::SWAY: {
            std::string out = run_command_capture({"swaymsg", "-t", "get_outputs", "-r"});
            std::string::size_type pos = 0;
            while ((pos = out.find("\"name\"", pos)) != std::string::npos) {
                pos = out.find(':', pos);
                if (pos == std::string::npos) break;
                pos = out.find('"', pos + 1);
                if (pos == std::string::npos) break;
                pos++;
                auto end = out.find('"', pos);
                if (end == std::string::npos) break;
                std::string name = out.substr(pos, end - pos);
                if (!name.empty()) {
                    monitors.push_back({name});
                }
                pos = end + 1;
            }
            break;
        }
        case Backend::FEH:
        case Backend::NONE:
            break;
    }

    return monitors;
}

inline std::string bg_socket_path() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0]) {
        base = std::string(xdg) + "/tuipaper";
    } else {
        const char* home = getenv("HOME");
        if (!home) return "";
        base = std::string(home) + "/.config/tuipaper";
    }
    return base + "/bg.sock";
}

inline std::string bg_pid_path() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0]) {
        base = std::string(xdg) + "/tuipaper";
    } else {
        const char* home = getenv("HOME");
        if (!home) return "";
        base = std::string(home) + "/.config/tuipaper";
    }
    return base + "/bg.pid";
}

inline bool is_bg_running() {
    std::string pidpath = bg_pid_path();
    if (pidpath.empty()) return false;

    FILE* f = fopen(pidpath.c_str(), "r");
    if (!f) return false;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) { fclose(f); return false; }
    fclose(f);

    if (pid <= 0) return false;
    return kill(pid, 0) == 0;
}

inline std::string bg_binary_path() {
    char self[1024];
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len > 0) {
        self[len] = '\0';
        std::string dir(self);
        auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            std::string candidate = dir.substr(0, slash + 1) + "tuipaper-bg";
            if (access(candidate.c_str(), X_OK) == 0)
                return candidate;
        }
    }
    if (command_exists("tuipaper-bg"))
        return "tuipaper-bg";
    return "";
}

inline bool start_bg_process() {
    if (is_bg_running()) return true;

    std::string bin = bg_binary_path();
    if (bin.empty()) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl(bin.c_str(), "tuipaper-bg", nullptr);
        _exit(127);
    }

    std::string sockpath = bg_socket_path();
    for (int i = 0; i < 50; i++) {
        usleep(50000);
        if (access(sockpath.c_str(), F_OK) == 0)
            return true;
    }
    return false;
}

inline std::string send_bg_command(const std::string& cmd) {
    std::string sockpath = bg_socket_path();
    if (sockpath.empty()) return "error no socket path";

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "error socket creation failed";

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return "error connect failed";
    }

    std::string msg = cmd + "\n";
    ssize_t w = ::write(fd, msg.c_str(), msg.size());
    if (w < 0) {
        close(fd);
        return "error write failed";
    }

    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    std::string response;
    if (poll(&pfd, 1, 5000) > 0) {
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            response = buf;
            while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
                response.pop_back();
        }
    }

    close(fd);
    return response.empty() ? "error no response" : response;
}

inline Backend detect_backend() {
    if (getenv("WAYLAND_DISPLAY")) {
        std::string bg = bg_binary_path();
        if (!bg.empty()) return Backend::NATIVE;
    }

    if (getenv("SWAYSOCK")) {
        if (command_exists("swaymsg")) return Backend::SWAY;
    }

    if (getenv("DISPLAY")) {
        if (command_exists("feh")) return Backend::FEH;
    }

    return Backend::NONE;
}

struct SetResult {
    bool success;
    std::string message;
};

inline SetResult set_wallpaper(const std::string& path, Backend backend,
                              FitMode fit, const std::string& monitor) {
    std::string target = (monitor.empty() || monitor == "*") ? "*" : monitor;

    switch (backend) {
        case Backend::NATIVE: {
            if (!start_bg_process())
                return {false, "Failed to start tuipaper-bg"};

            std::string cmd = "set " + path + " " + fit_mode_name(fit);
            std::string resp = send_bg_command(cmd);
            if (resp == "ok")
                return {true, "Wallpaper set via tuipaper-bg"};
            return {false, "tuipaper-bg: " + resp};
        }

        case Backend::SWAY: {
            std::string mode;
            switch (fit) {
                case FitMode::FILL: mode = "fill"; break;
                case FitMode::COVER: mode = "fill"; break;
                case FitMode::CONTAIN: mode = "fit"; break;
                case FitMode::CENTER: mode = "center"; break;
                case FitMode::TILE: mode = "tile"; break;
            }
            if (run_command({"swaymsg", "output", target, "bg", path, mode})) {
                return {true, "Wallpaper set via swaymsg"};
            }
            return {false, "swaymsg failed to set wallpaper"};
        }

        case Backend::FEH: {
            std::string flag;
            switch (fit) {
                case FitMode::FILL: flag = "--bg-fill"; break;
                case FitMode::COVER: flag = "--bg-fill"; break;
                case FitMode::CONTAIN: flag = "--bg-scale"; break;
                case FitMode::CENTER: flag = "--bg-center"; break;
                case FitMode::TILE: flag = "--bg-tile"; break;
            }
            if (run_command({"feh", flag, path})) {
                return {true, "Wallpaper set via feh"};
            }
            return {false, "feh failed to set wallpaper"};
        }

        case Backend::NONE:
            return {false, "No wallpaper backend detected. Install tuipaper-bg, swaymsg, or feh."};
    }
    return {false, "Unknown backend"};
}

struct Workspace {
    int id;
    std::string name;
};

inline int json_int(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return -1;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    bool neg = false;
    if (pos < json.size() && json[pos] == '-') { neg = true; pos++; }
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return neg ? -val : val;
}

inline std::string json_str(const std::string& json, const std::string& key, size_t start = 0) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle, start);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

inline std::vector<Workspace> get_workspaces() {
    std::vector<Workspace> result;
    std::string out = run_command_capture({"hyprctl", "workspaces", "-j"});
    if (out.empty()) return result;

    size_t pos = 0;
    while ((pos = out.find('{', pos)) != std::string::npos) {
        auto end = out.find('}', pos);
        if (end == std::string::npos) break;
        std::string obj = out.substr(pos, end - pos + 1);

        int id = json_int(obj, "id");
        std::string name = json_str(obj, "name");
        if (id > 0 && id <= 10) {
            result.push_back({id, name});
        }
        pos = end + 1;
    }

    std::sort(result.begin(), result.end(),
              [](const Workspace& a, const Workspace& b) { return a.id < b.id; });

    std::vector<Workspace> full;
    for (int i = 1; i <= 10; i++) {
        bool found = false;
        for (auto& w : result) {
            if (w.id == i) {
                full.push_back(w);
                found = true;
                break;
            }
        }
        if (!found) {
            full.push_back({i, std::to_string(i)});
        }
    }
    return full;
}

inline int get_active_workspace_id() {
    std::string out = run_command_capture({"hyprctl", "activeworkspace", "-j"});
    if (out.empty()) return -1;
    return json_int(out, "id");
}

inline std::string hypr_event_socket_path() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!xdg || !sig) return "";
    return std::string(xdg) + "/hypr/" + sig + "/.socket2.sock";
}

inline std::string daemon_pid_path() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0]) {
        base = std::string(xdg) + "/tuipaper";
    } else {
        const char* home = getenv("HOME");
        if (!home) return "";
        base = std::string(home) + "/.config/tuipaper";
    }
    return base + "/daemon.pid";
}

inline void kill_daemon() {
    std::string pidpath = daemon_pid_path();
    if (pidpath.empty()) return;

    FILE* f = fopen(pidpath.c_str(), "r");
    if (!f) return;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) { fclose(f); return; }
    fclose(f);

    if (pid > 0) {
        kill(pid, SIGTERM);
        unlink(pidpath.c_str());
    }
}

inline void daemon_set_wallpaper(const std::string& imgpath, Backend backend,
                               FitMode fit, const std::string& monitor = "*") {
    if (backend == Backend::NATIVE) {
        send_bg_command("set " + imgpath + " " + fit_mode_name(fit));
    } else if (backend == Backend::SWAY) {
        std::string mon = monitor.empty() ? "*" : monitor;
        std::string mode;
        switch (fit) {
            case FitMode::FILL: mode = "fill"; break;
            case FitMode::COVER: mode = "fill"; break;
            case FitMode::CONTAIN: mode = "fit"; break;
            case FitMode::CENTER: mode = "center"; break;
            case FitMode::TILE: mode = "tile"; break;
        }
        run_command({"swaymsg", "output", mon, "bg", imgpath, mode});
    } else if (backend == Backend::FEH) {
        std::string flag;
        switch (fit) {
            case FitMode::FILL: flag = "--bg-fill"; break;
            case FitMode::COVER: flag = "--bg-fill"; break;
            case FitMode::CONTAIN: flag = "--bg-scale"; break;
            case FitMode::CENTER: flag = "--bg-center"; break;
            case FitMode::TILE: flag = "--bg-tile"; break;
        }
        run_command({"feh", flag, imgpath});
    }
}

inline void spawn_workspace_daemon(Backend backend) {
    kill_daemon();

    std::string pidpath = daemon_pid_path();
    if (pidpath.empty()) return;

    const char* xdg = getenv("XDG_CONFIG_HOME");
    std::string config_path;
    if (xdg && xdg[0]) {
        config_path = std::string(xdg) + "/tuipaper/config";
    } else {
        const char* home = getenv("HOME");
        if (!home) return;
        config_path = std::string(home) + "/.config/tuipaper/config";
    }

    std::string hypr_sock_path = hypr_event_socket_path();

    /* double-fork to detach */
    pid_t pid1 = fork();
    if (pid1 < 0) return;
    if (pid1 > 0) {
        waitpid(pid1, nullptr, 0);
        return;
    }

    setsid();
    pid_t pid2 = fork();
    if (pid2 < 0) _exit(1);
    if (pid2 > 0) _exit(0);

    signal(SIGCHLD, SIG_IGN);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    {
        FILE* pf = fopen(pidpath.c_str(), "w");
        if (pf) {
            fprintf(pf, "%d\n", getpid());
            fclose(pf);
        }
    }

    struct DaemonConfig {
        std::map<int, std::string> workspace_mappings;
        std::string workspace_fit;
        std::string directory;
        int cycle_interval;
        std::string fit;
        std::string monitor;
        std::string wallpaper_mode;
    };

    auto readConfig = [&config_path]() -> DaemonConfig {
        DaemonConfig dc;
        dc.cycle_interval = 5;
        dc.fit = "fill";
        dc.workspace_fit = "fill";
        dc.monitor = "*";

        FILE* cf = fopen(config_path.c_str(), "r");
        if (!cf) return dc;

        char line[4096];
        while (fgets(line, sizeof(line), cf)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            auto eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string key = l.substr(0, eq);
            std::string val = l.substr(eq + 1);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());

            if (key == "last_directory") dc.directory = val;
            else if (key == "cycle_interval") {
                int v = atoi(val.c_str());
                if (v > 0) dc.cycle_interval = v;
            }
            else if (key == "fit_mode") dc.fit = val;
            else if (key == "monitor" && !val.empty()) dc.monitor = val;
            else if (key == "wallpaper_mode") dc.wallpaper_mode = val;
            else if (key == "workspace_fit") dc.workspace_fit = val;
            else if (key.substr(0, 10) == "workspace_") {
                int ws_id = atoi(key.substr(10).c_str());
                if (ws_id > 0 && !val.empty()) {
                    dc.workspace_mappings[ws_id] = val;
                }
            }
        }
        fclose(cf);
        return dc;
    };

    auto isImage = [](const std::string& name) -> bool {
        static const char* exts[] = {
            ".png", ".jpg", ".jpeg", ".bmp", ".webp",
            ".gif", ".tiff", ".tif", ".avif", ".svg"
        };
        std::string lower = name;
        for (auto& c : lower) c = tolower(c);
        for (auto& ext : exts) {
            size_t elen = strlen(ext);
            if (lower.size() >= elen &&
                lower.compare(lower.size() - elen, elen, ext) == 0)
                return true;
        }
        return false;
    };

    if (backend == Backend::NATIVE) {
        start_bg_process();
    }

    auto dc = readConfig();
    struct stat config_st;
    time_t last_mtime = 0;
    if (stat(config_path.c_str(), &config_st) == 0)
        last_mtime = config_st.st_mtime;

    bool cycle_active = (dc.wallpaper_mode == "cycle");
    int cycle_interval_ms = dc.cycle_interval * 60 * 1000;
    int file_index = 0;

    int hypr_fd = -1;
    if (!hypr_sock_path.empty() && !dc.workspace_mappings.empty()) {
        hypr_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (hypr_fd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, hypr_sock_path.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(hypr_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(hypr_fd);
                hypr_fd = -1;
            }
        }
    }

    if (hypr_fd >= 0 && !dc.workspace_mappings.empty()) {
        std::string out = run_command_capture({"hyprctl", "activeworkspace", "-j"});
        int current_ws = json_int(out, "id");
        if (current_ws > 0) {
            auto it = dc.workspace_mappings.find(current_ws);
            if (it != dc.workspace_mappings.end()) {
                FitMode fit = fit_mode_from_name(dc.workspace_fit);
                daemon_set_wallpaper(it->second, backend, fit, dc.monitor);
            }
        }
    }

    if (cycle_active && !dc.directory.empty()) {
        std::vector<std::string> images;
        DIR* dir = opendir(dc.directory.c_str());
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                if (!name.empty() && name[0] == '.') continue;
                if (isImage(name)) images.push_back(dc.directory + "/" + name);
            }
            closedir(dir);
        }
        if (!images.empty()) {
            std::sort(images.begin(), images.end());
            FitMode fit = fit_mode_from_name(dc.fit);
            daemon_set_wallpaper(images[0], backend, fit, dc.monitor);
            file_index = 1;
        }
    }

    struct timespec cycle_start;
    clock_gettime(CLOCK_MONOTONIC, &cycle_start);

    std::string buffer;
    char buf[4096];

    while (true) {
        if (stat(config_path.c_str(), &config_st) == 0 &&
            config_st.st_mtime != last_mtime) {
            dc = readConfig();
            last_mtime = config_st.st_mtime;
            cycle_active = (dc.wallpaper_mode == "cycle");
            cycle_interval_ms = dc.cycle_interval * 60 * 1000;

            bool need_hypr = !hypr_sock_path.empty() && !dc.workspace_mappings.empty();
            if (need_hypr && hypr_fd < 0) {
                hypr_fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (hypr_fd >= 0) {
                    struct sockaddr_un addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sun_family = AF_UNIX;
                    strncpy(addr.sun_path, hypr_sock_path.c_str(), sizeof(addr.sun_path) - 1);
                    if (connect(hypr_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                        close(hypr_fd);
                        hypr_fd = -1;
                    }
                }
            } else if (!need_hypr && hypr_fd >= 0) {
                close(hypr_fd);
                hypr_fd = -1;
            }

            if (!cycle_active && dc.workspace_mappings.empty()) {
                if (hypr_fd >= 0) close(hypr_fd);
                unlink(pidpath.c_str());
                _exit(0);
            }
        }

        int timeout_ms = -1;
        if (cycle_active) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - cycle_start.tv_sec) * 1000 +
                             (now.tv_nsec - cycle_start.tv_nsec) / 1000000;
            int remaining = cycle_interval_ms - (int)elapsed_ms;
            timeout_ms = (remaining > 0) ? remaining : 0;
        }

        if (hypr_fd < 0 && !cycle_active) {
            unlink(pidpath.c_str());
            _exit(0);
        }

        struct pollfd pfd = {hypr_fd, POLLIN, 0};
        int nfds = (hypr_fd >= 0) ? 1 : 0;
        int ret = poll(nfds ? &pfd : nullptr, nfds, timeout_ms);

        if (ret > 0 && hypr_fd >= 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(hypr_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                close(hypr_fd);
                hypr_fd = -1;
                if (!cycle_active) {
                    unlink(pidpath.c_str());
                    _exit(0);
                }
                continue;
            }

            buf[n] = '\0';
            buffer += buf;

            size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);

                if (line.substr(0, 11) == "workspace>>") {
                    std::string ws_str = line.substr(11);
                    int ws_id = atoi(ws_str.c_str());
                    if (ws_id <= 0) continue;

                    auto it = dc.workspace_mappings.find(ws_id);
                    if (it != dc.workspace_mappings.end()) {
                        FitMode fit = fit_mode_from_name(dc.workspace_fit);
                        daemon_set_wallpaper(it->second, backend, fit, dc.monitor);
                    }
                }
            }
        }

        if (ret > 0 && hypr_fd >= 0 && (pfd.revents & (POLLERR | POLLHUP))) {
            close(hypr_fd);
            hypr_fd = -1;
            if (!cycle_active) {
                unlink(pidpath.c_str());
                _exit(0);
            }
        }

        if (cycle_active) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - cycle_start.tv_sec) * 1000 +
                             (now.tv_nsec - cycle_start.tv_nsec) / 1000000;

            if (elapsed_ms >= cycle_interval_ms) {
                if (!dc.directory.empty()) {
                    std::vector<std::string> images;
                    DIR* dir = opendir(dc.directory.c_str());
                    if (dir) {
                        struct dirent* ent;
                        while ((ent = readdir(dir)) != nullptr) {
                            std::string name = ent->d_name;
                            if (name == "." || name == "..") continue;
                            if (!name.empty() && name[0] == '.') continue;
                            if (isImage(name)) images.push_back(dc.directory + "/" + name);
                        }
                        closedir(dir);
                    }

                    if (!images.empty()) {
                        std::sort(images.begin(), images.end());
                        if (file_index >= (int)images.size()) file_index = 0;
                        FitMode fit = fit_mode_from_name(dc.fit);
                        daemon_set_wallpaper(images[file_index], backend, fit, dc.monitor);
                        file_index++;
                    }
                }

                clock_gettime(CLOCK_MONOTONIC, &cycle_start);
            }
        }
    }
}

} // namespace wallpaper
