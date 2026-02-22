#pragma once

#include <cstdio>
#include <string>
#include <functional>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace tui {

// Key constants
enum Key {
    KEY_NONE = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_ESCAPE,
};

static termios orig_termios;
static std::function<void()> resize_callback;

inline void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

inline void apply_raw_mode() {
    termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms timeout
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

inline void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    apply_raw_mode();
}

// Temporarily restore original terminal for subprocess use
inline void suspend_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

inline void resume_raw_mode() {
    apply_raw_mode();
}

struct TermSize {
    int rows;
    int cols;
};

struct CellPixelSize {
    int width;
    int height;
};

inline TermSize get_terminal_size() {
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return {24, 80};
    }
    return {ws.ws_row, ws.ws_col};
}

inline CellPixelSize get_cell_pixel_size() {
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0
        && ws.ws_xpixel > 0 && ws.ws_ypixel > 0
        && ws.ws_col > 0 && ws.ws_row > 0) {
        return {(int)ws.ws_xpixel / ws.ws_col, (int)ws.ws_ypixel / ws.ws_row};
    }
    return {8, 16}; // common default
}

static volatile sig_atomic_t resized = 0;

inline void sigwinch_handler(int) {
    resized = 1;
}

inline void install_resize_handler(std::function<void()> cb) {
    resize_callback = std::move(cb);
    struct sigaction sa;
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, nullptr);
}

inline bool check_resize() {
    if (resized) {
        resized = 0;
        if (resize_callback) resize_callback();
        return true;
    }
    return false;
}

// Output helpers — write directly to stdout
inline void write(const std::string& s) {
    ::write(STDOUT_FILENO, s.data(), s.size());
}

inline void move_cursor(int row, int col) {
    char buf[32];
    snprintf(buf, sizeof(buf), "\033[%d;%dH", row, col);
    write(buf);
}

inline void clear_screen() {
    write("\033[2J\033[H");
}

inline void hide_cursor() {
    write("\033[?25l");
}

inline void show_cursor() {
    write("\033[?25h");
}

inline void enter_alt_screen() {
    write("\033[?1049h");
}

inline void exit_alt_screen() {
    write("\033[?1049l");
}

// Color mode state
static bool color_disabled = false;
static std::string accent_seq;

inline void init_color_mode(bool disable_color, const std::string& accent_hex) {
    color_disabled = disable_color;
    accent_seq.clear();
    if (!disable_color && accent_hex.size() == 7 && accent_hex[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        char buf[32];
        r = (unsigned int)strtol(accent_hex.substr(1, 2).c_str(), nullptr, 16);
        g = (unsigned int)strtol(accent_hex.substr(3, 2).c_str(), nullptr, 16);
        b = (unsigned int)strtol(accent_hex.substr(5, 2).c_str(), nullptr, 16);
        snprintf(buf, sizeof(buf), "\033[38;2;%u;%u;%um", r, g, b);
        accent_seq = buf;
    }
}

inline void reset_style() {
    write("\033[0m");
}

inline void set_bold() {
    write("\033[1m");
}

inline void set_dim() {
    write("\033[2m");
}

inline void set_reverse() {
    write("\033[7m");
}

// Standard ANSI colors (0-7), add 60 for bright variants
// 0=black 1=red 2=green 3=yellow 4=blue 5=magenta 6=cyan 7=white
inline void set_fg(int color) {
    if (color_disabled) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "\033[%dm", 30 + color);
    write(buf);
}

inline void set_accent() {
    if (color_disabled) return;
    if (!accent_seq.empty()) {
        write(accent_seq);
    } else {
        set_fg(3);
    }
}

inline void set_default_fg() {
    write("\033[39m");
}

// Box drawing with Unicode characters
inline void draw_box(int x, int y, int w, int h) {
    // Top border
    move_cursor(y, x);
    write("\u250c");
    for (int i = 0; i < w - 2; i++) write("\u2500");
    write("\u2510");

    // Side borders
    for (int i = 1; i < h - 1; i++) {
        move_cursor(y + i, x);
        write("\u2502");
        move_cursor(y + i, x + w - 1);
        write("\u2502");
    }

    // Bottom border
    move_cursor(y + h - 1, x);
    write("\u2514");
    for (int i = 0; i < w - 2; i++) write("\u2500");
    write("\u2518");
}

// Read a single keypress, decoding escape sequences
inline int read_key() {
    char c;
    int nread = ::read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return KEY_NONE;

    if (c == '\x1b') {
        char seq[3];
        if (::read(STDIN_FILENO, &seq[0], 1) != 1) return KEY_ESCAPE;
        if (::read(STDIN_FILENO, &seq[1], 1) != 1) return KEY_ESCAPE;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return KEY_ESCAPE;
    }

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 127 || c == '\b') return KEY_BACKSPACE;

    return c;
}

} // namespace tui
