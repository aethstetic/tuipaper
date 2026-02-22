/*
 * tuipaper-bg — Native Wayland wallpaper setter using wlr-layer-shell
 *
 * Stays running, renders wallpaper on the BACKGROUND layer of every output,
 * and accepts IPC commands via a UNIX socket:
 *   set <path> <mode>\n   (mode: fill/contain/center/tile)
 *   quit\n
 * Response: ok\n or error <msg>\n
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <math.h>
#include <linux/memfd.h>

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Stub: the layer-shell protocol references xdg_popup_interface (for get_popup)
 * but we never use popups, so provide the symbol to satisfy the linker. */
const struct wl_interface xdg_popup_interface = {0};

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "stb_image.h"

/* ─── fit modes ─────────────────────────────────────────────────────── */

enum fit_mode {
    FIT_FILL,
    FIT_COVER,
    FIT_CONTAIN,
    FIT_CENTER,
    FIT_TILE,
};

static enum fit_mode parse_fit_mode(const char *s) {
    if (!s) return FIT_FILL;
    if (strcmp(s, "cover")   == 0) return FIT_COVER;
    if (strcmp(s, "contain") == 0) return FIT_CONTAIN;
    if (strcmp(s, "center")  == 0) return FIT_CENTER;
    if (strcmp(s, "tile")    == 0) return FIT_TILE;
    return FIT_FILL;
}

/* ─── per-output state ──────────────────────────────────────────────── */

struct output {
    struct wl_output *wl_output;
    uint32_t name_id; /* global name from registry */
    char *output_name;
    int32_t width, height;
    int32_t scale;
    bool have_mode;
    bool configured;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t ls_width, ls_height; /* from layer_surface configure */
    struct wl_buffer *buffer;

    struct output *next;
};

/* ─── global state ──────────────────────────────────────────────────── */

static struct {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_shm        *shm;
    struct zwlr_layer_shell_v1 *layer_shell;

    struct output *outputs;

    /* current wallpaper image (decoded pixels) */
    uint8_t *img_pixels;   /* RGBA */
    int      img_w, img_h;
    enum fit_mode fit;

    /* IPC */
    int  ipc_fd;
    char sock_path[512];
    char pid_path[512];
    char state_path[512];

    bool running;
} state;

/* ─── helpers ───────────────────────────────────────────────────────── */

static void config_dir(char *buf, size_t len) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(buf, len, "%s/tuipaper", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(buf, len, "%s/.config/tuipaper", home ? home : "/tmp");
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void init_paths(void) {
    char dir[400];
    config_dir(dir, sizeof(dir));
    mkdir(dir, 0755);
    snprintf(state.sock_path,  sizeof(state.sock_path),  "%s/bg.sock",  dir);
    snprintf(state.pid_path,   sizeof(state.pid_path),   "%s/bg.pid",   dir);
    snprintf(state.state_path, sizeof(state.state_path), "%s/bg.state", dir);
}
#pragma GCC diagnostic pop

static void write_pid_file(void) {
    FILE *f = fopen(state.pid_path, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

static void remove_pid_file(void) {
    unlink(state.pid_path);
}

/* ─── SHM buffer allocation ────────────────────────────────────────── */

struct shm_buf {
    void *data;
    size_t size;
    struct wl_buffer *buffer;
};

static struct shm_buf create_shm_buffer(int width, int height) {
    struct shm_buf buf = {0};
    int stride = width * 4;
    buf.size = (size_t)stride * height;

    int fd = memfd_create("tuipaper-bg", MFD_CLOEXEC);
    if (fd < 0) return buf;
    if (ftruncate(fd, (off_t)buf.size) < 0) { close(fd); return buf; }

    buf.data = mmap(NULL, buf.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf.data == MAP_FAILED) { close(fd); buf.data = NULL; return buf; }

    struct wl_shm_pool *pool = wl_shm_create_pool(state.shm, fd, (int32_t)buf.size);
    buf.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                           WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

/* ─── image rendering ───────────────────────────────────────────────── */

/* bilinear sample from RGBA source */
static uint32_t sample_bilinear(const uint8_t *src, int sw, int sh,
                                float fx, float fy) {
    /* clamp */
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > sw - 1) fx = (float)(sw - 1);
    if (fy > sh - 1) fy = (float)(sh - 1);

    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < sw ? x0 + 1 : x0;
    int y1 = y0 + 1 < sh ? y0 + 1 : y0;
    float dx = fx - x0, dy = fy - y0;

    const uint8_t *p00 = src + (y0 * sw + x0) * 4;
    const uint8_t *p10 = src + (y0 * sw + x1) * 4;
    const uint8_t *p01 = src + (y1 * sw + x0) * 4;
    const uint8_t *p11 = src + (y1 * sw + x1) * 4;

    uint8_t rgba[4];
    for (int c = 0; c < 4; c++) {
        float v = p00[c] * (1-dx) * (1-dy)
                + p10[c] *    dx  * (1-dy)
                + p01[c] * (1-dx) *    dy
                + p11[c] *    dx  *    dy;
        rgba[c] = (uint8_t)(v + 0.5f);
    }
    /* RGBA → ARGB (Wayland SHM format) */
    return ((uint32_t)rgba[3] << 24) | ((uint32_t)rgba[0] << 16) |
           ((uint32_t)rgba[1] << 8)  |  (uint32_t)rgba[2];
}

static void render_fill(uint32_t *dst, int dw, int dh,
                        const uint8_t *src, int sw, int sh) {
    float sx = (float)sw / dw;
    float sy = (float)sh / dh;
    float s  = sx < sy ? sx : sy; /* inverse: use max to crop */
    s = sx > sy ? sx : sy;
    float ow = dw * s, oh = dh * s;
    float ox = (sw - ow) * 0.5f, oy = (sh - oh) * 0.5f;

    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            dst[y * dw + x] = sample_bilinear(src, sw, sh,
                                              ox + x * s, oy + y * s);
}

static void render_cover(uint32_t *dst, int dw, int dh,
                         const uint8_t *src, int sw, int sh) {
    /* scale to fill screen (max zoom), crop excess, preserve aspect ratio */
    float sx = (float)sw / dw;
    float sy = (float)sh / dh;
    float s  = sx < sy ? sx : sy; /* min = zoom in more = crop more */
    float ow = dw * s, oh = dh * s;
    float ox = (sw - ow) * 0.5f, oy = (sh - oh) * 0.5f;

    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            dst[y * dw + x] = sample_bilinear(src, sw, sh,
                                              ox + x * s, oy + y * s);
}

static void render_contain(uint32_t *dst, int dw, int dh,
                           const uint8_t *src, int sw, int sh) {
    /* letterbox: scale image to fit, black bars */
    memset(dst, 0, (size_t)dw * dh * 4);
    float sx = (float)dw / sw;
    float sy = (float)dh / sh;
    float s  = sx < sy ? sx : sy;
    int nw = (int)(sw * s), nh = (int)(sh * s);
    int ox = (dw - nw) / 2, oy = (dh - nh) / 2;

    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            float fx = (float)x / s;
            float fy = (float)y / s;
            dst[(oy + y) * dw + (ox + x)] = sample_bilinear(src, sw, sh, fx, fy);
        }
}

static void render_center(uint32_t *dst, int dw, int dh,
                          const uint8_t *src, int sw, int sh) {
    memset(dst, 0, (size_t)dw * dh * 4);
    int ox = (dw - sw) / 2, oy = (dh - sh) / 2;

    for (int y = 0; y < sh; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dh) continue;
        for (int x = 0; x < sw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= dw) continue;
            const uint8_t *p = src + (y * sw + x) * 4;
            dst[dy * dw + dx] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                                ((uint32_t)p[1] << 8)  |  (uint32_t)p[2];
        }
    }
}

static void render_tile(uint32_t *dst, int dw, int dh,
                        const uint8_t *src, int sw, int sh) {
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int tx = ((x % sw) + sw) % sw;
            int ty = ((y % sh) + sh) % sh;
            const uint8_t *p = src + (ty * sw + tx) * 4;
            dst[y * dw + x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                               ((uint32_t)p[1] << 8)  |  (uint32_t)p[2];
        }
}

static void render_image(struct output *out) {
    if (!out->ls_width || !out->ls_height) return;

    int bw = (int)out->ls_width  * out->scale;
    int bh = (int)out->ls_height * out->scale;

    /* destroy old buffer */
    if (out->buffer) {
        wl_buffer_destroy(out->buffer);
        out->buffer = NULL;
    }

    struct shm_buf buf = create_shm_buffer(bw, bh);
    if (!buf.data) return;

    uint32_t *pixels = (uint32_t *)buf.data;

    if (state.img_pixels) {
        switch (state.fit) {
        case FIT_FILL:    render_fill(pixels, bw, bh, state.img_pixels, state.img_w, state.img_h); break;
        case FIT_COVER:   render_cover(pixels, bw, bh, state.img_pixels, state.img_w, state.img_h); break;
        case FIT_CONTAIN: render_contain(pixels, bw, bh, state.img_pixels, state.img_w, state.img_h); break;
        case FIT_CENTER:  render_center(pixels, bw, bh, state.img_pixels, state.img_w, state.img_h); break;
        case FIT_TILE:    render_tile(pixels, bw, bh, state.img_pixels, state.img_w, state.img_h); break;
        }
    } else {
        memset(pixels, 0, buf.size);
    }

    munmap(buf.data, buf.size);

    out->buffer = buf.buffer;
    wl_surface_set_buffer_scale(out->surface, out->scale);
    wl_surface_attach(out->surface, out->buffer, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, bw, bh);
    wl_surface_commit(out->surface);
}

/* ─── layer surface callbacks ───────────────────────────────────────── */

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial, uint32_t w, uint32_t h) {
    struct output *out = data;
    out->ls_width = w;
    out->ls_height = h;
    out->configured = true;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    render_image(out);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    struct output *out = data;
    (void)ls;
    if (out->layer_surface) {
        zwlr_layer_surface_v1_destroy(out->layer_surface);
        out->layer_surface = NULL;
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
        out->surface = NULL;
    }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* ─── output callbacks ──────────────────────────────────────────────── */

static void output_geometry(void *data, struct wl_output *wl_output,
                            int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make,
                            const char *model, int32_t transform) {
    (void)data; (void)wl_output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *wl_output,
                         uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)wl_output; (void)refresh;
    struct output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->width = w;
        out->height = h;
        out->have_mode = true;
    }
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;
    (void)data;
}

static void output_scale(void *data, struct wl_output *wl_output,
                          int32_t factor) {
    (void)wl_output;
    struct output *out = data;
    out->scale = factor;
}

static void output_name(void *data, struct wl_output *wl_output,
                         const char *name) {
    (void)wl_output;
    struct output *out = data;
    free(out->output_name);
    out->output_name = strdup(name);
}

static void output_description(void *data, struct wl_output *wl_output,
                                const char *desc) {
    (void)data; (void)wl_output; (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry    = output_geometry,
    .mode        = output_mode,
    .done        = output_done,
    .scale       = output_scale,
    .name        = output_name,
    .description = output_description,
};

/* ─── create layer surface for an output ────────────────────────────── */

static void create_layer_surface(struct output *out) {
    if (!state.compositor || !state.layer_shell) return;
    if (out->surface) return; /* already created */

    out->surface = wl_compositor_create_surface(state.compositor);
    out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, out->surface, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "tuipaper");

    zwlr_layer_surface_v1_set_size(out->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(out->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(out->layer_surface,
                                       &layer_surface_listener, out);

    /* initial commit without buffer to trigger configure */
    wl_surface_commit(out->surface);
}

/* ─── registry ──────────────────────────────────────────────────────── */

static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface, uint32_t ver) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        state.compositor = wl_registry_bind(reg, name,
                                            &wl_compositor_interface,
                                            ver < 4 ? ver : 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        state.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        state.layer_shell = wl_registry_bind(reg, name,
                                             &zwlr_layer_shell_v1_interface,
                                             ver < 2 ? ver : 2);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        uint32_t bind_ver = ver < 4 ? ver : 4;
        struct wl_output *wl_out = wl_registry_bind(reg, name,
                                                    &wl_output_interface,
                                                    bind_ver);
        struct output *out = calloc(1, sizeof(*out));
        out->wl_output = wl_out;
        out->name_id = name;
        out->scale = 1;
        out->next = state.outputs;
        state.outputs = out;
        wl_output_add_listener(wl_out, &output_listener, out);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
    (void)data; (void)reg;
    struct output **pp = &state.outputs;
    while (*pp) {
        struct output *out = *pp;
        if (out->name_id == name) {
            if (out->layer_surface)
                zwlr_layer_surface_v1_destroy(out->layer_surface);
            if (out->surface)
                wl_surface_destroy(out->surface);
            if (out->buffer)
                wl_buffer_destroy(out->buffer);
            wl_output_destroy(out->wl_output);
            free(out->output_name);
            *pp = out->next;
            free(out);
            return;
        }
        pp = &out->next;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* ─── IPC ───────────────────────────────────────────────────────────── */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
static int setup_ipc(void) {
    /* remove stale socket */
    unlink(state.sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, state.sock_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

#pragma GCC diagnostic pop

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 4) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

static void ipc_reply(int fd, const char *msg) {
    size_t len = strlen(msg);
    ssize_t r;
    size_t sent = 0;
    while (sent < len) {
        r = write(fd, msg + sent, len - sent);
        if (r <= 0) break;
        sent += (size_t)r;
    }
}

static void save_state(const char *path, const char *mode) {
    FILE *f = fopen(state.state_path, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", path, mode);
        fclose(f);
    }
}

static bool restore_state(void) {
    FILE *f = fopen(state.state_path, "r");
    if (!f) return false;

    char path[4096], mode[32];
    if (!fgets(path, sizeof(path), f)) { fclose(f); return false; }
    if (!fgets(mode, sizeof(mode), f)) { fclose(f); return false; }
    fclose(f);

    /* strip newlines */
    path[strcspn(path, "\n")] = '\0';
    mode[strcspn(mode, "\n")] = '\0';

    if (path[0] == '\0') return false;

    state.fit = parse_fit_mode(mode);
    int w, h, channels;
    state.img_pixels = stbi_load(path, &w, &h, &channels, 4);
    if (state.img_pixels) {
        state.img_w = w;
        state.img_h = h;
        return true;
    }
    return false;
}

static void load_image(const char *path) {
    if (state.img_pixels) {
        stbi_image_free(state.img_pixels);
        state.img_pixels = NULL;
    }
    int w, h, channels;
    state.img_pixels = stbi_load(path, &w, &h, &channels, 4);
    if (state.img_pixels) {
        state.img_w = w;
        state.img_h = h;
    }
}

static void rerender_all(void) {
    for (struct output *out = state.outputs; out; out = out->next) {
        if (out->configured)
            render_image(out);
    }
    wl_display_flush(state.display);
}

static void handle_ipc_command(int client_fd) {
    char buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    /* strip trailing newline */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';

    if (strncmp(buf, "set ", 4) == 0) {
        /* "set <path> <mode>" — mode is last space-separated token */
        char *args = buf + 4;
        char *last_space = strrchr(args, ' ');
        if (!last_space) {
            ipc_reply(client_fd, "error missing mode\n");
            close(client_fd);
            return;
        }
        *last_space = '\0';
        char *path = args;
        char *mode = last_space + 1;

        state.fit = parse_fit_mode(mode);
        load_image(path);

        if (!state.img_pixels) {
            ipc_reply(client_fd, "error failed to load image\n");
            close(client_fd);
            return;
        }

        rerender_all();
        save_state(path, mode);
        ipc_reply(client_fd, "ok\n");
    } else if (strcmp(buf, "quit") == 0) {
        ipc_reply(client_fd, "ok\n");
        state.running = false;
    } else {
        ipc_reply(client_fd, "error unknown command\n");
    }
    close(client_fd);
}

/* ─── signal handler ────────────────────────────────────────────────── */

static volatile sig_atomic_t got_signal = 0;

static void signal_handler(int sig) {
    (void)sig;
    got_signal = 1;
}

/* ─── cleanup ───────────────────────────────────────────────────────── */

static void cleanup(void) {
    if (state.ipc_fd >= 0) {
        close(state.ipc_fd);
        unlink(state.sock_path);
    }
    remove_pid_file();

    for (struct output *out = state.outputs; out; ) {
        struct output *next = out->next;
        if (out->layer_surface)
            zwlr_layer_surface_v1_destroy(out->layer_surface);
        if (out->surface)
            wl_surface_destroy(out->surface);
        if (out->buffer)
            wl_buffer_destroy(out->buffer);
        if (out->wl_output)
            wl_output_destroy(out->wl_output);
        free(out->output_name);
        free(out);
        out = next;
    }
    state.outputs = NULL;

    if (state.layer_shell) zwlr_layer_shell_v1_destroy(state.layer_shell);
    if (state.shm)         wl_shm_destroy(state.shm);
    if (state.compositor)  wl_compositor_destroy(state.compositor);
    if (state.registry)    wl_registry_destroy(state.registry);
    if (state.display) {
        wl_display_flush(state.display);
        wl_display_disconnect(state.display);
    }

    if (state.img_pixels) stbi_image_free(state.img_pixels);
}

/* ─── main ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    init_paths();

    /* connect to Wayland */
    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "tuipaper-bg: cannot connect to Wayland display\n");
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, NULL);
    wl_display_roundtrip(state.display);

    if (!state.compositor) {
        fprintf(stderr, "tuipaper-bg: compositor not found\n");
        cleanup();
        return 1;
    }
    if (!state.shm) {
        fprintf(stderr, "tuipaper-bg: wl_shm not found\n");
        cleanup();
        return 1;
    }
    if (!state.layer_shell) {
        fprintf(stderr, "tuipaper-bg: wlr-layer-shell not supported\n");
        cleanup();
        return 1;
    }

    /* second roundtrip to receive output events (mode, scale, name) */
    wl_display_roundtrip(state.display);

    /* create layer surfaces for all outputs */
    for (struct output *out = state.outputs; out; out = out->next)
        create_layer_surface(out);

    wl_display_roundtrip(state.display);

    /* restore last wallpaper from state file */
    if (restore_state())
        rerender_all();

    /* setup IPC */
    state.ipc_fd = setup_ipc();
    if (state.ipc_fd < 0) {
        cleanup();
        return 1;
    }

    write_pid_file();

    /* signals */
    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    signal(SIGPIPE, SIG_IGN);

    state.running = true;

    /* event loop: poll on Wayland fd + IPC socket */
    int wl_fd = wl_display_get_fd(state.display);

    while (state.running && !got_signal) {
        /* flush pending Wayland requests */
        while (wl_display_prepare_read(state.display) != 0)
            wl_display_dispatch_pending(state.display);
        wl_display_flush(state.display);

        struct pollfd fds[2] = {
            { .fd = wl_fd,        .events = POLLIN },
            { .fd = state.ipc_fd, .events = POLLIN },
        };

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            wl_display_cancel_read(state.display);
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            wl_display_read_events(state.display);
            wl_display_dispatch_pending(state.display);
        } else {
            wl_display_cancel_read(state.display);
        }

        if (fds[1].revents & POLLIN) {
            int client = accept4(state.ipc_fd, NULL, NULL,
                                 SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (client >= 0)
                handle_ipc_command(client);
        }
    }

    cleanup();
    return 0;
}
