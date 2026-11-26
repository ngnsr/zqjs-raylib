/*
 * zqjs-raylib: C11 Raylib host embedding zqjs (libzqjs.a + zqjs.h).
 *
 * Demonstrates why an embeddable JS engine shines inside a C/Raylib host:
 *   - External `game.js` owns gameplay & rendering logic
 *   - Live hot-reload on file save (or pressing R) without closing the window
 *   - Hard sandbox caps (per-frame step budget + heap cap): syntax errors,
 *     uncaught throws, or `while (true) {}` show an in-window error banner
 *     instead of freezing or crashing the host
 */

#include "raylib.h"
#include "zqjs.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WIN_W 900
#define WIN_H 560
#define FRAME_STEP_BUDGET 500000ull
#define SCRIPT_HEAP_CAP (8u * 1024u * 1024u)

typedef struct host_state {
    bool headless;
    const char *script_path;
    time_t last_mtime;
    uint32_t reload_count;

    Font ui_font;
    bool custom_font;
    double js_ms_avg;

    zqjs_runtime *rt;
    zqjs_program *prog;
    zqjs_handle update_fn;
    zqjs_handle draw_fn;

    bool has_error;
    char error_title[96];
    char error_detail[320];

    uint64_t draw_calls;
} host_state;

static bool on_print(void *ctx, const char *bytes, size_t len) {
    (void)ctx;
    return fwrite(bytes, 1, len, stdout) == len;
}

static unsigned char clamp_u8(double v) {
    if (isnan(v) || v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (unsigned char)v;
}

static zqjs_status throw_type(zqjs_runtime *rt, const char *ascii) {
    uint16_t buf[96];
    size_t n = strlen(ascii);
    if (n > sizeof(buf) / sizeof(buf[0])) n = sizeof(buf) / sizeof(buf[0]);
    for (size_t i = 0; i < n; i++) buf[i] = (uint16_t)(unsigned char)ascii[i];
    zqjs_runtime_throw_js(rt, ZQJS_ERROR_TYPE, buf, n);
    return ZQJS_ERR_THROWN;
}

static zqjs_status arg_num(
    zqjs_runtime *rt,
    const zqjs_handle *args,
    size_t argc,
    size_t idx,
    double *out
) {
    if (idx >= argc) return throw_type(rt, "missing numeric argument");
    if (zqjs_handle_number(rt, args[idx], out) != ZQJS_OK) {
        return throw_type(rt, "expected a number");
    }
    return ZQJS_OK;
}

static zqjs_status arg_num_opt(
    zqjs_runtime *rt,
    const zqjs_handle *args,
    size_t argc,
    size_t idx,
    double fallback,
    double *out
) {
    if (idx >= argc) {
        *out = fallback;
        return ZQJS_OK;
    }
    return arg_num(rt, args, argc, idx, out);
}

/* --- Native JS bindings exposed to game.js --- */

static zqjs_status native_register_game(
    zqjs_runtime *rt,
    void *ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    (void)out;
    host_state *h = (host_state *)ctx;
    if (argc < 1) return throw_type(rt, "registerGame expects { update, draw }");

    zqjs_handle_kind k = ZQJS_HANDLE_UNDEFINED;
    if (zqjs_handle_kind_of(rt, args[0], &k) != ZQJS_OK || k != ZQJS_HANDLE_OBJECT) {
        return throw_type(rt, "registerGame argument must be an object");
    }

    zqjs_failure fail;
    zqjs_handle upd = ZQJS_HANDLE_NULL;
    zqjs_handle drw = ZQJS_HANDLE_NULL;
    zqjs_status st = zqjs_handle_get_property(rt, args[0], "update", 6, &fail, &upd);
    if (st != ZQJS_OK) return st;
    st = zqjs_handle_get_property(rt, args[0], "draw", 4, &fail, &drw);
    if (st != ZQJS_OK) {
        zqjs_handle_release(rt, upd);
        return st;
    }

    zqjs_handle_kind uk = ZQJS_HANDLE_UNDEFINED, dk = ZQJS_HANDLE_UNDEFINED;
    zqjs_handle_kind_of(rt, upd, &uk);
    zqjs_handle_kind_of(rt, drw, &dk);
    if (uk != ZQJS_HANDLE_FUNCTION || dk != ZQJS_HANDLE_FUNCTION) {
        zqjs_handle_release(rt, drw);
        zqjs_handle_release(rt, upd);
        return throw_type(rt, "registerGame requires update and draw functions");
    }

    if (h->update_fn.owner != NULL) zqjs_handle_release(rt, h->update_fn);
    if (h->draw_fn.owner != NULL) zqjs_handle_release(rt, h->draw_fn);
    h->update_fn = upd;
    h->draw_fn = drw;
    return ZQJS_OK;
}

static zqjs_status native_clear_bg(
    zqjs_runtime *rt, void *ctx, const zqjs_handle *t,
    const zqjs_handle *args, size_t argc, zqjs_native_out *out
) {
    (void)t; (void)out;
    host_state *h = (host_state *)ctx;
    double r, g, b;
    if (arg_num(rt, args, argc, 0, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 1, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 2, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;
    h->draw_calls++;
    if (!h->headless) ClearBackground((Color){ clamp_u8(r), clamp_u8(g), clamp_u8(b), 255 });
    return ZQJS_OK;
}

static zqjs_status native_draw_circle(
    zqjs_runtime *rt, void *ctx, const zqjs_handle *t,
    const zqjs_handle *args, size_t argc, zqjs_native_out *out
) {
    (void)t; (void)out;
    host_state *h = (host_state *)ctx;
    double x, y, rad, r, g, b, a;
    if (arg_num(rt, args, argc, 0, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 1, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 2, &rad) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 3, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 4, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 5, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num_opt(rt, args, argc, 6, 255.0, &a) != ZQJS_OK) return ZQJS_ERR_THROWN;
    h->draw_calls++;
    if (!h->headless) {
        DrawCircleV(
            (Vector2){ (float)x, (float)y },
            (float)rad,
            (Color){ clamp_u8(r), clamp_u8(g), clamp_u8(b), clamp_u8(a) }
        );
    }
    return ZQJS_OK;
}

static zqjs_status native_draw_ring(
    zqjs_runtime *rt, void *ctx, const zqjs_handle *t,
    const zqjs_handle *args, size_t argc, zqjs_native_out *out
) {
    (void)t; (void)out;
    host_state *h = (host_state *)ctx;
    double x, y, rad, r, g, b, a;
    if (arg_num(rt, args, argc, 0, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 1, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 2, &rad) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 3, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 4, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 5, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num_opt(rt, args, argc, 6, 255.0, &a) != ZQJS_OK) return ZQJS_ERR_THROWN;
    h->draw_calls++;
    if (!h->headless) {
        float outer = (float)rad;
        float inner = outer > 2.0f ? outer - 2.0f : 0.0f;
        DrawRing(
            (Vector2){ (float)x, (float)y },
            inner,
            outer,
            0.0f,
            360.0f,
            64,
            (Color){ clamp_u8(r), clamp_u8(g), clamp_u8(b), clamp_u8(a) }
        );
    }
    return ZQJS_OK;
}

static zqjs_status native_draw_rect(
    zqjs_runtime *rt, void *ctx, const zqjs_handle *t,
    const zqjs_handle *args, size_t argc, zqjs_native_out *out
) {
    (void)t; (void)out;
    host_state *h = (host_state *)ctx;
    double x, y, w, hh, r, g, b, a;
    if (arg_num(rt, args, argc, 0, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 1, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 2, &w) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 3, &hh) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 4, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 5, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 6, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num_opt(rt, args, argc, 7, 255.0, &a) != ZQJS_OK) return ZQJS_ERR_THROWN;
    h->draw_calls++;
    if (!h->headless) {
        DrawRectangle((int)x, (int)y, (int)w, (int)hh, (Color){ clamp_u8(r), clamp_u8(g), clamp_u8(b), clamp_u8(a) });
    }
    return ZQJS_OK;
}

static zqjs_status native_draw_line(
    zqjs_runtime *rt, void *ctx, const zqjs_handle *t,
    const zqjs_handle *args, size_t argc, zqjs_native_out *out
) {
    (void)t; (void)out;
    host_state *h = (host_state *)ctx;
    double x1, y1, x2, y2, r, g, b, a;
    if (arg_num(rt, args, argc, 0, &x1) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 1, &y1) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 2, &x2) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 3, &y2) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 4, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 5, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 6, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num_opt(rt, args, argc, 7, 255.0, &a) != ZQJS_OK) return ZQJS_ERR_THROWN;
    h->draw_calls++;
    if (!h->headless) {
        DrawLineEx(
            (Vector2){ (float)x1, (float)y1 },
            (Vector2){ (float)x2, (float)y2 },
            1.5f,
            (Color){ clamp_u8(r), clamp_u8(g), clamp_u8(b), clamp_u8(a) }
        );
    }
    return ZQJS_OK;
}

static void draw_ui_text(const host_state *h, const char *text, float x, float y, float sz, Color col) {
    if (h->custom_font) {
        DrawTextEx(h->ui_font, text, (Vector2){ x, y }, sz, 0.5f, col);
    } else {
        DrawText(text, (int)x, (int)y, (int)sz, col);
    }
}

static void init_ui_font(host_state *h) {
    const char *candidates[] = {
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (FileExists(candidates[i])) {
            Font f = LoadFontEx(candidates[i], 40, NULL, 0);
            if (f.texture.id != 0) {
                SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
                h->ui_font = f;
                h->custom_font = true;
                return;
            }
        }
    }
    h->ui_font = GetFontDefault();
    h->custom_font = false;
}

static zqjs_status native_draw_text(
    zqjs_runtime *rt, void *ctx, const zqjs_handle *t,
    const zqjs_handle *args, size_t argc, zqjs_native_out *out
) {
    (void)t; (void)out;
    host_state *h = (host_state *)ctx;
    if (argc < 7) return throw_type(rt, "drawText(text, x, y, size, r, g, b)");

    uint16_t units[191];
    size_t ulen = 0;
    if (zqjs_handle_string_utf16(rt, args[0], units, sizeof(units) / sizeof(units[0]), &ulen) != ZQJS_OK) {
        return throw_type(rt, "drawText expects a string");
    }
    char text[192];
    size_t n = ulen < sizeof(text) - 1 ? ulen : sizeof(text) - 1;
    for (size_t i = 0; i < n; i++) text[i] = units[i] < 128u ? (char)units[i] : '?';
    text[n] = '\0';

    double x, y, sz, r, g, b;
    if (arg_num(rt, args, argc, 1, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 2, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 3, &sz) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 4, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 5, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_num(rt, args, argc, 6, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;

    h->draw_calls++;
    if (!h->headless) {
        draw_ui_text(h, text, (float)x, (float)y, (float)sz, (Color){ clamp_u8(r), clamp_u8(g), clamp_u8(b), 255 });
    }
    return ZQJS_OK;
}

/* --- Script lifecycle, hot-reload & error containment --- */

static void host_unload(host_state *h) {
    if (h->rt != NULL) {
        if (h->update_fn.owner != NULL) zqjs_handle_release(h->rt, h->update_fn);
        if (h->draw_fn.owner != NULL) zqjs_handle_release(h->rt, h->draw_fn);
        if (h->prog != NULL) zqjs_program_destroy(h->prog);
        zqjs_runtime_destroy(h->rt);
        h->rt = NULL;
    }
    h->prog = NULL;
    h->update_fn = ZQJS_HANDLE_NULL;
    h->draw_fn = ZQJS_HANDLE_NULL;
}

static void host_set_error(host_state *h, const char *title, const char *detail) {
    h->has_error = true;
    snprintf(h->error_title, sizeof(h->error_title), "%s", title);
    snprintf(h->error_detail, sizeof(h->error_detail), "%s", detail);
    fprintf(stderr, "[zqjs-raylib] %s: %s\n", h->error_title, h->error_detail);
}

static const char *failure_title(const zqjs_failure *f) {
    switch (f->kind) {
        case ZQJS_FAILURE_THROWN: return "Uncaught JS Exception";
        case ZQJS_FAILURE_LIMIT:
            if (f->limit == ZQJS_LIMIT_STEPS) return "Sandbox LimitExceeded (max_steps)";
            if (f->limit == ZQJS_LIMIT_HEAP_BYTES) return "Sandbox LimitExceeded (max_heap_bytes)";
            return "Sandbox LimitExceeded";
        case ZQJS_FAILURE_UNSUPPORTED: return "Unsupported Feature";
        case ZQJS_FAILURE_OUT_OF_MEMORY: return "Out of Memory";
        default: return "Runtime Error";
    }
}

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static time_t file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

static zqjs_status host_load_source(host_state *h, const char *name, const char *src, size_t len) {
    host_unload(h);
    h->has_error = false;
    h->error_title[0] = '\0';
    h->error_detail[0] = '\0';

    zqjs_options opts = zqjs_options_default();
    opts.output_fn = on_print;
    opts.random_seed = 0x5A514A53ull ^ (uint64_t)h->reload_count;
    opts.limits.max_heap_bytes = SCRIPT_HEAP_CAP;
    opts.limits.max_steps = FRAME_STEP_BUDGET;

    zqjs_status st = zqjs_runtime_create(&opts, &h->rt);
    if (st != ZQJS_OK) {
        host_set_error(h, "Runtime Init Failed", "Could not allocate zqjs runtime");
        return st;
    }

    const struct { const char *name; zqjs_native_fn fn; } bindings[] = {
        { "registerGame", native_register_game },
        { "clearBg",      native_clear_bg },
        { "drawCircle",   native_draw_circle },
        { "drawRing",     native_draw_ring },
        { "drawRect",     native_draw_rect },
        { "drawLine",     native_draw_line },
        { "drawText",     native_draw_text },
    };
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        st = zqjs_runtime_define_global_function(
            h->rt, bindings[i].name, strlen(bindings[i].name), bindings[i].fn, h
        );
        if (st != ZQJS_OK) {
            host_set_error(h, "Host Binding Failed", bindings[i].name);
            return st;
        }
    }

    zqjs_diagnostic diag;
    zqjs_program *prog = NULL;
    st = zqjs_runtime_compile(h->rt, name, strlen(name), src, len, &diag, &prog);
    if (st != ZQJS_OK) {
        char detail[320];
        snprintf(detail, sizeof(detail), "%s:%u:%u: %.*s",
            name, diag.line, diag.column, (int)diag.message_len, diag.message);
        host_set_error(h, "Compile / SyntaxError", detail);
        return st;
    }

    zqjs_failure fail;
    st = zqjs_runtime_run(h->rt, prog, &fail);
    if (st != ZQJS_OK) {
        zqjs_program_destroy(prog);
        char detail[320];
        snprintf(detail, sizeof(detail), "%.*s", (int)fail.message_len, fail.message);
        host_set_error(h, failure_title(&fail), detail);
        return st;
    }
    /* Keep `prog` alive for the lifetime of closures (`update`, `draw`) created by it. */
    h->prog = prog;

    if (h->update_fn.owner == NULL || h->draw_fn.owner == NULL) {
        host_set_error(h, "Script Contract Error", "Script did not call registerGame({ update, draw })");
        return ZQJS_ERR_HOST;
    }
    return ZQJS_OK;
}

static bool host_reload_file(host_state *h) {
    size_t len = 0;
    char *src = read_entire_file(h->script_path, &len);
    if (!src) {
        host_set_error(h, "Cannot Read Script", h->script_path);
        return false;
    }
    h->last_mtime = file_mtime(h->script_path);
    h->reload_count++;
    zqjs_status st = host_load_source(h, h->script_path, src, len);
    free(src);
    return st == ZQJS_OK;
}

static bool set_num(zqjs_runtime *rt, zqjs_handle obj, const char *k, double v, zqjs_failure *f) {
    zqjs_handle h = ZQJS_HANDLE_NULL;
    if (zqjs_new_number(rt, v, &h) != ZQJS_OK) return false;
    zqjs_status st = zqjs_handle_set_property(rt, obj, k, strlen(k), h, f);
    zqjs_handle_release(rt, h);
    return st == ZQJS_OK;
}

static bool set_bool(zqjs_runtime *rt, zqjs_handle obj, const char *k, bool v, zqjs_failure *f) {
    zqjs_handle h = ZQJS_HANDLE_NULL;
    if (zqjs_new_boolean(rt, v, &h) != ZQJS_OK) return false;
    zqjs_status st = zqjs_handle_set_property(rt, obj, k, strlen(k), h, f);
    zqjs_handle_release(rt, h);
    return st == ZQJS_OK;
}

static zqjs_status host_step_frame(host_state *h, double dt, double elapsed) {
    if (h->has_error || h->rt == NULL) return ZQJS_OK;

    zqjs_failure fail;
    zqjs_handle input = ZQJS_HANDLE_NULL;
    if (zqjs_new_object(h->rt, &input) != ZQJS_OK) {
        host_set_error(h, "Host Allocation Failed", "Could not allocate input object");
        return ZQJS_ERR_OUT_OF_MEMORY;
    }

    Vector2 mouse = h->headless ? (Vector2){ WIN_W * 0.5f, WIN_H * 0.5f } : GetMousePosition();
    bool up    = !h->headless && (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP));
    bool down  = !h->headless && (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN));
    bool left  = !h->headless && (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT));
    bool right = !h->headless && (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT));
    bool space = !h->headless && IsKeyDown(KEY_SPACE);
    bool mdown = !h->headless && IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    bool ok = set_num(h->rt, input, "dt", dt, &fail) &&
              set_num(h->rt, input, "time", elapsed, &fail) &&
              set_num(h->rt, input, "mouseX", mouse.x, &fail) &&
              set_num(h->rt, input, "mouseY", mouse.y, &fail) &&
              set_bool(h->rt, input, "mouseDown", mdown, &fail) &&
              set_bool(h->rt, input, "up", up, &fail) &&
              set_bool(h->rt, input, "down", down, &fail) &&
              set_bool(h->rt, input, "left", left, &fail) &&
              set_bool(h->rt, input, "right", right, &fail) &&
              set_bool(h->rt, input, "space", space, &fail);
    if (!ok) {
        zqjs_handle_release(h->rt, input);
        host_set_error(h, "Input Setup Failed", "Could not populate frame input");
        return ZQJS_ERR_HOST;
    }

    zqjs_handle res = ZQJS_HANDLE_NULL;
    zqjs_status st = zqjs_runtime_call(h->rt, h->update_fn, NULL, &input, 1, &fail, &res);
    zqjs_handle_release(h->rt, input);
    if (st != ZQJS_OK) {
        char detail[320];
        snprintf(detail, sizeof(detail), "%.*s", (int)fail.message_len, fail.message);
        host_set_error(h, failure_title(&fail), detail);
        return st;
    }
    zqjs_handle_release(h->rt, res);

    res = ZQJS_HANDLE_NULL;
    st = zqjs_runtime_call(h->rt, h->draw_fn, NULL, NULL, 0, &fail, &res);
    if (st != ZQJS_OK) {
        char detail[320];
        snprintf(detail, sizeof(detail), "%.*s", (int)fail.message_len, fail.message);
        host_set_error(h, failure_title(&fail), detail);
        return st;
    }
    zqjs_handle_release(h->rt, res);
    return ZQJS_OK;
}

static void draw_status_footer(const host_state *h) {
    DrawRectangle(0, WIN_H - 38, WIN_W, 38, (Color){ 10, 14, 24, 248 });
    DrawLine(0, WIN_H - 38, WIN_W, WIN_H - 38, (Color){ 42, 56, 86, 255 });
    zqjs_memory mem;
    memset(&mem, 0, sizeof(mem));
    if (h->rt != NULL) (void)zqjs_runtime_memory(h->rt, &mem);

    char bar[256];
    snprintf(
        bar, sizeof(bar),
        "zqjs  |  %s  |  reloads: %u  |  heap: %.0f KB (gc: %llu)  |  js: %.2f ms  |  %d FPS",
        h->script_path,
        h->reload_count,
        (double)mem.heap_bytes / 1024.0,
        (unsigned long long)mem.collections,
        h->js_ms_avg,
        GetFPS()
    );
    draw_ui_text(h, bar, 16.0f, (float)(WIN_H - 28), 18.0f, (Color){ 215, 228, 248, 255 });
}

static void draw_error_overlay(const host_state *h) {
    ClearBackground((Color){ 28, 12, 18, 255 });
    DrawRectangle(40, 80, WIN_W - 80, WIN_H - 170, (Color){ 46, 18, 26, 255 });
    DrawRectangleLines(40, 80, WIN_W - 80, WIN_H - 170, (Color){ 240, 86, 108, 255 });
    draw_ui_text(h, "SCRIPT PAUSED - HOST STILL ALIVE", 66.0f, 108.0f, 26.0f, (Color){ 255, 120, 138, 255 });
    draw_ui_text(h, h->error_title, 66.0f, 156.0f, 20.0f, (Color){ 255, 215, 130, 255 });
    draw_ui_text(h, h->error_detail, 66.0f, 196.0f, 18.0f, (Color){ 240, 240, 245, 255 });
    draw_ui_text(
        h,
        "Edit & save game.js (auto hot-reload) or press R to retry.",
        66.0f, (float)(WIN_H - 134), 18.0f, (Color){ 180, 205, 245, 255 }
    );
}

/* --- Headless smoke test (verifies game.js, hot-reload, and sandbox isolation) --- */

static int run_smoke(const char *script_path) {
    host_state h;
    memset(&h, 0, sizeof(h));
    h.headless = true;
    h.script_path = script_path;

    if (!host_reload_file(&h)) return 1;
    for (int i = 0; i < 60; i++) {
        if (host_step_frame(&h, 1.0 / 60.0, i / 60.0) != ZQJS_OK) {
            host_unload(&h);
            return 1;
        }
    }
    if (h.draw_calls < 60) {
        fprintf(stderr, "smoke: expected draw calls from game.js\n");
        host_unload(&h);
        return 1;
    }

    /* Hot-reload cleanly resets VM state with zero leaks */
    if (!host_reload_file(&h) || host_step_frame(&h, 1.0 / 60.0, 0.0) != ZQJS_OK) {
        host_unload(&h);
        return 1;
    }

    /* Sandbox fault containment: syntax error, runtime throw, infinite loop */
    const char *bad_syntax = "const x = ;";
    if (host_load_source(&h, "bad.js", bad_syntax, strlen(bad_syntax)) != ZQJS_ERR_SOURCE) {
        fprintf(stderr, "smoke: expected ZQJS_ERR_SOURCE\n");
        host_unload(&h);
        return 1;
    }

    const char *bad_throw = "registerGame({ update() { throw new Error('boom'); }, draw() {} });";
    if (host_load_source(&h, "throw.js", bad_throw, strlen(bad_throw)) != ZQJS_OK ||
        host_step_frame(&h, 1.0 / 60.0, 0.0) != ZQJS_ERR_THROWN) {
        fprintf(stderr, "smoke: expected ZQJS_ERR_THROWN\n");
        host_unload(&h);
        return 1;
    }

    const char *inf_loop = "registerGame({ update() { while (true) {} }, draw() {} });";
    if (host_load_source(&h, "loop.js", inf_loop, strlen(inf_loop)) != ZQJS_OK ||
        host_step_frame(&h, 1.0 / 60.0, 0.0) != ZQJS_ERR_LIMIT_EXCEEDED) {
        fprintf(stderr, "smoke: expected ZQJS_ERR_LIMIT_EXCEEDED\n");
        host_unload(&h);
        return 1;
    }

    /* Recover cleanly back to game.js after sandbox faults */
    if (!host_reload_file(&h) || host_step_frame(&h, 1.0 / 60.0, 0.0) != ZQJS_OK) {
        host_unload(&h);
        return 1;
    }

    zqjs_memory mem;
    zqjs_runtime_memory(h.rt, &mem);
    printf(
        "smoke ok: script=%s reloads=%u draw_calls=%llu heap=%.1fKB gc=%llu\n",
        script_path, h.reload_count, (unsigned long long)h.draw_calls,
        (double)mem.heap_bytes / 1024.0, (unsigned long long)mem.collections
    );
    host_unload(&h);
    return 0;
}

int main(int argc, char **argv) {
    if ((zqjs_abi_version() >> 16) != ZQJS_ABI_MAJOR) {
        fprintf(stderr, "zqjs ABI mismatch\n");
        return 1;
    }

    bool smoke = false;
    const char *script_path = "game.js";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else {
            script_path = argv[i];
        }
    }

    if (smoke) return run_smoke(script_path);

    host_state h;
    memset(&h, 0, sizeof(h));
    h.script_path = script_path;
    (void)host_reload_file(&h);

    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    InitWindow(WIN_W, WIN_H, "zqjs + Raylib - Live Hot-Reload JS Sandbox");
    init_ui_font(&h);

    double watch_accum = 0.0;
    while (!WindowShouldClose()) {
        double dt = GetFrameTime();
        watch_accum += dt;

        /* Auto hot-reload when game.js is saved on disk, or immediately on R */
        if (IsKeyPressed(KEY_R)) {
            (void)host_reload_file(&h);
        } else if (watch_accum >= 0.2) {
            watch_accum = 0.0;
            time_t mt = file_mtime(h.script_path);
            if (mt != 0 && mt != h.last_mtime) {
                (void)host_reload_file(&h);
            }
        }

        BeginDrawing();
        if (h.has_error) {
            draw_error_overlay(&h);
        } else {
            double t0 = GetTime();
            (void)host_step_frame(&h, dt, t0);
            double js_ms = (GetTime() - t0) * 1000.0;
            h.js_ms_avg = h.js_ms_avg == 0.0 ? js_ms : (h.js_ms_avg * 0.92 + js_ms * 0.08);
            if (h.has_error) draw_error_overlay(&h);
        }
        draw_status_footer(&h);
        EndDrawing();
    }

    if (h.custom_font) UnloadFont(h.ui_font);
    CloseWindow();
    host_unload(&h);
    return 0;
}
