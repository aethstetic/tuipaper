#pragma once

#include <string>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>

#include "tui.hpp"

namespace preview {

enum class Terminal {
    KITTY,
    WEZTERM,
    OTHER, // try raw protocol
};

inline Terminal detect_terminal() {
    if (getenv("KITTY_WINDOW_ID")) return Terminal::KITTY;
    const char* term_prog = getenv("TERM_PROGRAM");
    if (term_prog && std::string(term_prog) == "WezTerm") return Terminal::WEZTERM;
    if (getenv("WEZTERM_EXECUTABLE")) return Terminal::WEZTERM;
    return Terminal::OTHER;
}

static Terminal current_terminal = detect_terminal();

// --- Raw Kitty graphics protocol (WezTerm, Ghostty, etc.) ---

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode(const std::string& input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(input.data());
    size_t len = input.size();
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = ((unsigned int)p[i]) << 16;
        if (i + 1 < len) n |= ((unsigned int)p[i + 1]) << 8;
        if (i + 2 < len) n |= (unsigned int)p[i + 2];
        out += b64[(n >> 18) & 0x3F];
        out += b64[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? b64[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64[n & 0x3F] : '=';
    }
    return out;
}

inline void raw_clear() {
    const char cmd[] = "\033_Ga=d,d=A,q=2\033\\";
    ::write(STDOUT_FILENO, cmd, sizeof(cmd) - 1);
}

inline void raw_display(const std::string& filepath, int row, int col, int cols, int rows) {
    std::string b64path = base64_encode(filepath);
    std::string buf;
    buf.reserve(256 + b64path.size());
    buf += "\033[";
    buf += std::to_string(row);
    buf += ';';
    buf += std::to_string(col);
    buf += 'H';
    buf += "\033_Ga=T,t=f,c=";
    buf += std::to_string(cols);
    buf += ",r=";
    buf += std::to_string(rows);
    buf += ",q=2;";
    buf += b64path;
    buf += "\033\\";
    ::write(STDOUT_FILENO, buf.data(), buf.size());
}

// --- kitten icat (Kitty) ---

inline void kitten_display(const std::string& filepath, int row, int col, int cols, int rows) {
    std::string place = std::to_string(cols) + "x" + std::to_string(rows)
                      + "@" + std::to_string(col - 1) + "x" + std::to_string(row - 1);

    // Log debug info
    {
        int dbg = open("/tmp/kitten_cmd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dbg >= 0) {
            std::string log = "place=" + place + "\nfile=" + filepath + "\n";
            ::write(dbg, log.data(), log.size());
            close(dbg);
        }
    }

    // Temporarily restore terminal so kitten can query cell size
    tui::suspend_raw_mode();

    pid_t pid = fork();
    if (pid == 0) {
        int errlog = open("/tmp/kitten_debug.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (errlog >= 0) { dup2(errlog, STDERR_FILENO); close(errlog); }
        execlp("kitten", "kitten", "icat",
               "--stdin=no",
               "--place", place.c_str(),
               filepath.c_str(), nullptr);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);

    tui::resume_raw_mode();
}

// --- wezterm imgcat (WezTerm) ---

inline void wezterm_display(const std::string& filepath, int row, int col, int cols, int rows) {
    // Position cursor then use wezterm imgcat
    char pos[32];
    snprintf(pos, sizeof(pos), "\033[%d;%dH", row, col);
    ::write(STDOUT_FILENO, pos, strlen(pos));

    std::string width = std::to_string(cols);
    std::string height = std::to_string(rows);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        int devnull_r = open("/dev/null", O_RDONLY);
        if (devnull_r >= 0) { dup2(devnull_r, STDIN_FILENO); close(devnull_r); }
        execlp("wezterm", "wezterm", "imgcat",
               "--width", width.c_str(),
               "--height", height.c_str(),
               filepath.c_str(), nullptr);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

// --- Image dimensions ---

struct ImageDimensions {
    int width = 0;
    int height = 0;
};

inline ImageDimensions get_image_dimensions(const std::string& filepath) {
    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) return {};

    unsigned char header[30];
    size_t nread = fread(header, 1, 30, f);
    if (nread < 24) { fclose(f); return {}; }

    // PNG: bytes 0-7 are signature, 16-19 width, 20-23 height (big-endian)
    if (header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G') {
        int w = (header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19];
        int h = (header[20] << 24) | (header[21] << 16) | (header[22] << 8) | header[23];
        fclose(f);
        return {w, h};
    }

    // JPEG: starts with 0xFFD8
    if (header[0] == 0xFF && header[1] == 0xD8) {
        fseek(f, 2, SEEK_SET);
        unsigned char buf[2];
        while (fread(buf, 1, 2, f) == 2) {
            if (buf[0] != 0xFF) break;
            unsigned char marker = buf[1];
            // Skip padding bytes
            if (marker == 0xFF) { fseek(f, -1, SEEK_CUR); continue; }
            // SOF0 or SOF2
            if (marker == 0xC0 || marker == 0xC2) {
                unsigned char sof[7];
                if (fread(sof, 1, 7, f) == 7) {
                    int h = (sof[3] << 8) | sof[4];
                    int w = (sof[5] << 8) | sof[6];
                    fclose(f);
                    return {w, h};
                }
                break;
            }
            // Read segment length and skip
            unsigned char len_buf[2];
            if (fread(len_buf, 1, 2, f) != 2) break;
            int seg_len = (len_buf[0] << 8) | len_buf[1];
            if (seg_len < 2) break;
            fseek(f, seg_len - 2, SEEK_CUR);
        }
        fclose(f);
        return {};
    }

    // WebP: "RIFF" + size + "WEBP" + chunk type
    if (nread >= 30
        && header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F'
        && header[8] == 'W' && header[9] == 'E' && header[10] == 'B' && header[11] == 'P') {
        // VP8X (extended): canvas size at bytes 24-29
        if (header[12] == 'V' && header[13] == 'P' && header[14] == '8' && header[15] == 'X') {
            int w = 1 + (header[24] | (header[25] << 8) | (header[26] << 16));
            int h = 1 + (header[27] | (header[28] << 8) | (header[29] << 16));
            fclose(f);
            return {w, h};
        }
        // VP8L (lossless): 14-bit fields packed in a 32-bit LE word at byte 21
        if (header[12] == 'V' && header[13] == 'P' && header[14] == '8' && header[15] == 'L'
            && header[20] == 0x2F) {
            unsigned int bits = header[21] | (header[22] << 8)
                              | (header[23] << 16) | (header[24] << 24);
            int w = 1 + (int)(bits & 0x3FFF);
            int h = 1 + (int)((bits >> 14) & 0x3FFF);
            fclose(f);
            return {w, h};
        }
        // VP8 (lossy): start code 0x9D 0x01 0x2A then 16-bit LE width/height
        if (header[12] == 'V' && header[13] == 'P' && header[14] == '8' && header[15] == ' '
            && header[23] == 0x9D && header[24] == 0x01 && header[25] == 0x2A) {
            int w = (header[26] | (header[27] << 8)) & 0x3FFF;
            int h = (header[28] | (header[29] << 8)) & 0x3FFF;
            fclose(f);
            return {w, h};
        }
        fclose(f);
        return {};
    }

    // GIF: "GIF87a" or "GIF89a", width/height at bytes 6-9 (16-bit LE)
    if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F') {
        int w = header[6] | (header[7] << 8);
        int h = header[8] | (header[9] << 8);
        fclose(f);
        return {w, h};
    }

    // JPEG XL: bare codestream (0xFF 0x0A) or ISOBMFF container
    {
        const unsigned char* cs = nullptr;
        size_t cs_len = 0;
        unsigned char jxl_buf[64];

        // Bare codestream
        if (nread >= 2 && header[0] == 0xFF && header[1] == 0x0A) {
            cs = header + 2;
            cs_len = nread - 2;
        }
        // ISOBMFF container: 00 00 00 0C 4A 58 4C 20 0D 0A 87 0A
        else if (nread >= 12
                 && header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x00 && header[3] == 0x0C
                 && header[4] == 0x4A && header[5] == 0x58 && header[6] == 0x4C && header[7] == 0x20
                 && header[8] == 0x0D && header[9] == 0x0A && header[10] == 0x87 && header[11] == 0x0A) {
            // Scan boxes for jxlc (0x6A786C63)
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            long pos = 0;
            unsigned char box_hdr[8];
            while (pos + 8 <= file_size) {
                fseek(f, pos, SEEK_SET);
                if (fread(box_hdr, 1, 8, f) != 8) break;
                uint32_t box_size = ((uint32_t)box_hdr[0] << 24) | ((uint32_t)box_hdr[1] << 16)
                                  | ((uint32_t)box_hdr[2] << 8) | (uint32_t)box_hdr[3];
                bool is_jxlc = box_hdr[4] == 'j' && box_hdr[5] == 'x'
                             && box_hdr[6] == 'l' && box_hdr[7] == 'c';
                bool is_jxlp = box_hdr[4] == 'j' && box_hdr[5] == 'x'
                             && box_hdr[6] == 'l' && box_hdr[7] == 'p';
                if (is_jxlc || is_jxlp) {
                    size_t skip = is_jxlp ? 4 : 0; // jxlp has 4-byte index
                    fseek(f, pos + 8 + (long)skip, SEEK_SET);
                    size_t rd = fread(jxl_buf, 1, sizeof(jxl_buf), f);
                    if (rd >= 4 && jxl_buf[0] == 0xFF && jxl_buf[1] == 0x0A) {
                        cs = jxl_buf + 2;
                        cs_len = rd - 2;
                    }
                    break;
                }
                if (box_size < 8) break;
                pos += box_size;
            }
        }

        // Parse SizeHeader from codestream (LSB-first bitstream)
        if (cs && cs_len >= 4) {
            size_t byte_pos = 0;
            int bit_pos = 0;

            auto read_bits = [&](int n) -> uint32_t {
                uint32_t val = 0;
                for (int i = 0; i < n; i++) {
                    if (byte_pos >= cs_len) return val;
                    val |= (uint32_t)((cs[byte_pos] >> bit_pos) & 1) << i;
                    if (++bit_pos == 8) { bit_pos = 0; byte_pos++; }
                }
                return val;
            };

            auto read_u32 = [&]() -> uint32_t {
                static const int bits[] = {9, 13, 18, 30};
                uint32_t sel = read_bits(2);
                return read_bits(bits[sel]);
            };

            uint32_t small = read_bits(1);
            uint32_t h, w;

            if (small) {
                h = (read_bits(5) + 1) * 8;
            } else {
                h = 1 + read_u32();
            }

            uint32_t ratio = read_bits(3);
            if (ratio != 0) {
                static const int num[] = {0, 1, 12, 4, 3, 16, 5, 2};
                static const int den[] = {0, 1, 10, 3, 2,  9, 4, 1};
                w = (uint32_t)((uint64_t)h * num[ratio] / den[ratio]);
            } else if (small) {
                w = (read_bits(5) + 1) * 8;
            } else {
                w = 1 + read_u32();
            }

            fclose(f);
            return {(int)w, (int)h};
        }
    }

    fclose(f);
    return {};
}

// --- Image fitting ---

struct FittedSize {
    int cols;
    int rows;
};

inline FittedSize fit_image_to_cells(int img_w, int img_h,
                                     int avail_cols, int avail_rows,
                                     int cell_px_w, int cell_px_h) {
    if (img_w <= 0 || img_h <= 0 || avail_cols <= 0 || avail_rows <= 0)
        return {avail_cols, avail_rows};

    long max_px_w = (long)avail_cols * cell_px_w;
    long max_px_h = (long)avail_rows * cell_px_h;

    int fit_cols, fit_rows;
    if ((long)img_w * max_px_h >= (long)img_h * max_px_w) {
        // Width-limited
        fit_cols = avail_cols;
        fit_rows = (int)(((long)img_h * avail_cols * cell_px_w
                        + (long)img_w * cell_px_h - 1)
                       / ((long)img_w * cell_px_h));
    } else {
        // Height-limited
        fit_rows = avail_rows;
        fit_cols = (int)(((long)img_w * avail_rows * cell_px_h
                        + (long)img_h * cell_px_w - 1)
                       / ((long)img_h * cell_px_w));
    }

    if (fit_cols > avail_cols) fit_cols = avail_cols;
    if (fit_rows > avail_rows) fit_rows = avail_rows;
    if (fit_cols < 1) fit_cols = 1;
    if (fit_rows < 1) fit_rows = 1;

    return {fit_cols, fit_rows};
}

// --- Public API ---

inline void clear_images() {
    raw_clear(); // works for all Kitty-protocol terminals
}

inline void display_image(const std::string& filepath, int row, int col, int cols, int rows) {
    switch (current_terminal) {
        case Terminal::KITTY:
            kitten_display(filepath, row, col, cols, rows);
            break;
        case Terminal::WEZTERM:
            wezterm_display(filepath, row, col, cols, rows);
            break;
        case Terminal::OTHER:
            // Try raw Kitty graphics protocol (Ghostty, etc.)
            raw_display(filepath, row, col, cols, rows);
            break;
    }
}

} // namespace preview
