/*
 * M11.4: Raylib C-ABI embedding demo (C11, linked against libzqjs.a + zqjs.h + raylib).
 *
 * JavaScript owns game logic (`update(dt)` and `draw()`), while C owns the frame loop,
 * Raylib window/drawing calls, and deterministic headless smoke testing (`--frames N`).
 */

#include "raylib.h"
#include "zqjs.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct demo_host {
    bool headless;
    zqjs_handle update_cb;
    zqjs_handle draw_cb;
    zqjs_handle state_h;
    uint64_t clear_calls;
    uint64_t circle_calls;
    uint64_t rect_calls;
    uint64_t text_calls;
    char last_text[128];
} demo_host;

static bool on_print(void *ctx, const char *bytes, size_t len) {
    (void)ctx;
    return fwrite(bytes, 1, len, stdout) == len;
}

static unsigned char clamp_u8(double v) {
    if (isnan(v) || v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (unsigned char)v;
}

static zqjs_status arg_number(
    zqjs_runtime *rt,
    const zqjs_handle *args,
    size_t argc,
    size_t idx,
    double *out
) {
    if (idx >= argc) {
        const uint16_t msg[] = { 'm', 'i', 's', 's', 'i', 'n', 'g', ' ', 'a', 'r', 'g' };
        zqjs_runtime_throw_js(rt, ZQJS_ERROR_TYPE, msg, 11);
        return ZQJS_ERR_THROWN;
    }
    zqjs_status st = zqjs_handle_number(rt, args[idx], out);
    if (st != ZQJS_OK) {
        const uint16_t msg[] = { 'e', 'x', 'p', 'e', 'c', 't', 'e', 'd', ' ', 'n', 'u', 'm' };
        zqjs_runtime_throw_js(rt, ZQJS_ERROR_TYPE, msg, 12);
        return ZQJS_ERR_THROWN;
    }
    return ZQJS_OK;
}

static zqjs_status throw_type(zqjs_runtime *rt, const char *ascii) {
    uint16_t buf[64];
    size_t n = strlen(ascii);
    if (n > sizeof(buf) / sizeof(buf[0])) n = sizeof(buf) / sizeof(buf[0]);
    for (size_t i = 0; i < n; i++) buf[i] = (uint16_t)(unsigned char)ascii[i];
    zqjs_runtime_throw_js(rt, ZQJS_ERROR_TYPE, buf, n);
    return ZQJS_ERR_THROWN;
}

static zqjs_status native_init_game(
    zqjs_runtime *rt,
    void *raw_ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    (void)out;
    demo_host *host = (demo_host *)raw_ctx;
    if (argc < 1) return throw_type(rt, "initGame expects config object");

    zqjs_handle_kind cfg_kind = ZQJS_HANDLE_UNDEFINED;
    zqjs_status st = zqjs_handle_kind_of(rt, args[0], &cfg_kind);
    if (st != ZQJS_OK) return st;
    if (cfg_kind != ZQJS_HANDLE_OBJECT) return throw_type(rt, "config must be an object");

    zqjs_failure fail;
    zqjs_handle new_update = ZQJS_HANDLE_NULL;
    zqjs_handle new_draw = ZQJS_HANDLE_NULL;
    zqjs_handle new_state = ZQJS_HANDLE_NULL;

    st = zqjs_handle_get_property(rt, args[0], "update", 6, &fail, &new_update);
    if (st != ZQJS_OK) return st;
    st = zqjs_handle_get_property(rt, args[0], "draw", 4, &fail, &new_draw);
    if (st != ZQJS_OK) {
        zqjs_handle_release(rt, new_update);
        return st;
    }
    st = zqjs_handle_get_property(rt, args[0], "state", 5, &fail, &new_state);
    if (st != ZQJS_OK) {
        zqjs_handle_release(rt, new_draw);
        zqjs_handle_release(rt, new_update);
        return st;
    }

    zqjs_handle_kind uk = ZQJS_HANDLE_UNDEFINED;
    zqjs_handle_kind dk = ZQJS_HANDLE_UNDEFINED;
    zqjs_handle_kind sk = ZQJS_HANDLE_UNDEFINED;
    assert(zqjs_handle_kind_of(rt, new_update, &uk) == ZQJS_OK);
    assert(zqjs_handle_kind_of(rt, new_draw, &dk) == ZQJS_OK);
    assert(zqjs_handle_kind_of(rt, new_state, &sk) == ZQJS_OK);
    if (uk != ZQJS_HANDLE_FUNCTION || dk != ZQJS_HANDLE_FUNCTION || sk != ZQJS_HANDLE_OBJECT) {
        zqjs_handle_release(rt, new_state);
        zqjs_handle_release(rt, new_draw);
        zqjs_handle_release(rt, new_update);
        return throw_type(rt, "config requires update/draw functions and state object");
    }

    /* Inspect nested array/object state (`state.balls[0].c[0]`) and write `state.hostAck = true` */
    zqjs_handle balls = ZQJS_HANDLE_NULL;
    st = zqjs_handle_get_property(rt, new_state, "balls", 5, &fail, &balls);
    if (st == ZQJS_OK) {
        zqjs_handle_kind bk = ZQJS_HANDLE_UNDEFINED;
        assert(zqjs_handle_kind_of(rt, balls, &bk) == ZQJS_OK);
        if (bk == ZQJS_HANDLE_ARRAY) {
            zqjs_handle b0 = ZQJS_HANDLE_NULL;
            if (zqjs_handle_get_index(rt, balls, 0, &fail, &b0) == ZQJS_OK) {
                zqjs_handle col = ZQJS_HANDLE_NULL;
                if (zqjs_handle_get_property(rt, b0, "c", 1, &fail, &col) == ZQJS_OK) {
                    zqjs_handle c0 = ZQJS_HANDLE_NULL;
                    if (zqjs_handle_get_index(rt, col, 0, &fail, &c0) == ZQJS_OK) {
                        double ch = 0.0;
                        assert(zqjs_handle_number(rt, c0, &ch) == ZQJS_OK);
                        assert(ch >= 0.0 && ch <= 255.0);
                        zqjs_handle_release(rt, c0);
                    }
                    zqjs_handle_release(rt, col);
                }
                zqjs_handle_release(rt, b0);
            }
        }
        zqjs_handle_release(rt, balls);
    }

    zqjs_handle ack = ZQJS_HANDLE_NULL;
    if (zqjs_new_boolean(rt, true, &ack) == ZQJS_OK) {
        st = zqjs_handle_set_property(rt, new_state, "hostAck", 7, ack, &fail);
        zqjs_handle_release(rt, ack);
        if (st != ZQJS_OK) {
            zqjs_handle_release(rt, new_state);
            zqjs_handle_release(rt, new_draw);
            zqjs_handle_release(rt, new_update);
            return st;
        }
    }

    if (host->update_cb.owner != NULL) zqjs_handle_release(rt, host->update_cb);
    if (host->draw_cb.owner != NULL) zqjs_handle_release(rt, host->draw_cb);
    if (host->state_h.owner != NULL) zqjs_handle_release(rt, host->state_h);
    host->update_cb = new_update;
    host->draw_cb = new_draw;
    host->state_h = new_state;
    return ZQJS_OK;
}

static zqjs_status native_clear_bg(
    zqjs_runtime *rt,
    void *raw_ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    (void)out;
    demo_host *host = (demo_host *)raw_ctx;
    double r = 0, g = 0, b = 0;
    if (arg_number(rt, args, argc, 0, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 1, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 2, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;

    host->clear_calls++;
    if (!host->headless) {
        Color c = { clamp_u8(r), clamp_u8(g), clamp_u8(b), 255 };
        ClearBackground(c);
    }
    return ZQJS_OK;
}

static zqjs_status native_draw_circle(
    zqjs_runtime *rt,
    void *raw_ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    (void)out;
    demo_host *host = (demo_host *)raw_ctx;
    double x = 0, y = 0, rad = 0, r = 0, g = 0, b = 0;
    if (arg_number(rt, args, argc, 0, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 1, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 2, &rad) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 3, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 4, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 5, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;

    host->circle_calls++;
    if (!host->headless) {
        Color c = { clamp_u8(r), clamp_u8(g), clamp_u8(b), 255 };
        DrawCircle((int)x, (int)y, (float)rad, c);
    }
    return ZQJS_OK;
}

static zqjs_status native_draw_rect(
    zqjs_runtime *rt,
    void *raw_ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    (void)out;
    demo_host *host = (demo_host *)raw_ctx;
    double x = 0, y = 0, w = 0, h = 0, r = 0, g = 0, b = 0;
    if (arg_number(rt, args, argc, 0, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 1, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 2, &w) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 3, &h) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 4, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 5, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 6, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;

    host->rect_calls++;
    if (!host->headless) {
        Color c = { clamp_u8(r), clamp_u8(g), clamp_u8(b), 255 };
        DrawRectangle((int)x, (int)y, (int)w, (int)h, c);
    }
    return ZQJS_OK;
}

static zqjs_status native_draw_text(
    zqjs_runtime *rt,
    void *raw_ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    (void)out;
    demo_host *host = (demo_host *)raw_ctx;
    if (argc < 6) {
        const uint16_t msg[] = { 'm', 'i', 's', 's', 'i', 'n', 'g', ' ', 'a', 'r', 'g' };
        zqjs_runtime_throw_js(rt, ZQJS_ERROR_TYPE, msg, 11);
        return ZQJS_ERR_THROWN;
    }

    uint16_t units[127];
    size_t ulen = 0;
    zqjs_status st = zqjs_handle_string_utf16(rt, args[0], units, sizeof(units) / sizeof(units[0]), &ulen);
    if (st != ZQJS_OK) {
        const uint16_t msg[] = { 'e', 'x', 'p', 'e', 'c', 't', 'e', 'd', ' ', 's', 't', 'r' };
        zqjs_runtime_throw_js(rt, ZQJS_ERROR_TYPE, msg, 12);
        return ZQJS_ERR_THROWN;
    }
    size_t copy_len = ulen < sizeof(host->last_text) - 1 ? ulen : sizeof(host->last_text) - 1;
    for (size_t i = 0; i < copy_len; i++) {
        host->last_text[i] = units[i] < 128u ? (char)units[i] : '?';
    }
    host->last_text[copy_len] = '\0';

    double x = 0, y = 0, sz = 0, r = 0, g = 0, b = 0;
    if (arg_number(rt, args, argc, 1, &x) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 2, &y) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 3, &sz) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 4, &r) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 5, &g) != ZQJS_OK) return ZQJS_ERR_THROWN;
    if (arg_number(rt, args, argc, 6, &b) != ZQJS_OK) return ZQJS_ERR_THROWN;

    host->text_calls++;
    if (!host->headless) {
        Color c = { clamp_u8(r), clamp_u8(g), clamp_u8(b), 255 };
        DrawText(host->last_text, (int)x, (int)y, (int)sz, c);
    }
    return ZQJS_OK;
}

static zqjs_status native_key_down(
    zqjs_runtime *rt,
    void *raw_ctx,
    const zqjs_handle *opt_this,
    const zqjs_handle *args,
    size_t argc,
    zqjs_native_out *out
) {
    (void)opt_this;
    demo_host *host = (demo_host *)raw_ctx;
    double key = 0;
    if (arg_number(rt, args, argc, 0, &key) != ZQJS_OK) return ZQJS_ERR_THROWN;
    bool down = (!host->headless) && IsKeyDown((int)key);
    return zqjs_new_boolean(rt, down, &out->result);
}

static const char *DEFAULT_SCRIPT =
    "const W = 800, H = 480;\n"
    "let state = {\n"
    "  balls: [],\n"
    "  frameCount: 0,\n"
    "  caughtCount: 0,\n"
    "  hostAck: false,\n"
    "  hud: ''\n"
    "};\n"
    "\n"
    "for (let i = 0; i < 12; i++) {\n"
    "  let angle = (i / 12) * Math.PI * 2;\n"
    "  let speed = 90 + Math.floor(Math.random() * 140);\n"
    "  state.balls.push({\n"
    "    x: W * 0.5 + Math.cos(angle) * 60,\n"
    "    y: H * 0.5 + Math.sin(angle) * 60,\n"
    "    vx: Math.cos(angle) * speed,\n"
    "    vy: Math.sin(angle) * speed,\n"
    "    r: 10 + (i % 4) * 4,\n"
    "    c: [80 + (i * 15) % 170, 140 + (i * 23) % 110, 220 - (i * 11) % 120]\n"
    "  });\n"
    "}\n"
    "\n"
    "function update(input) {\n"
    "  if (!state.hostAck) throw 'missing-host-ack';\n"
    "  let dt = input.dt;\n"
    "  state.frameCount++;\n"
    "  try {\n"
    "    if (state.frameCount === 1) throw 'simulated-recoverable-error';\n"
    "  } catch (e) {\n"
    "    if (('' + e).includes('recoverable')) state.caughtCount++;\n"
    "  }\n"
    "  let boost = input.spaceDown ? 1.8 : 1.0;\n"
    "  for (let i = 0; i < state.balls.length; i++) {\n"
    "    let b = state.balls[i];\n"
    "    b.x = b.x + b.vx * dt * boost;\n"
    "    b.y = b.y + b.vy * dt * boost;\n"
    "    if (b.x - b.r < 0) { b.x = b.r; b.vx = Math.abs(b.vx); }\n"
    "    if (b.x + b.r > W) { b.x = W - b.r; b.vx = -Math.abs(b.vx); }\n"
    "    if (b.y - b.r < 48) { b.y = 48 + b.r; b.vy = Math.abs(b.vy); }\n"
    "    if (b.y + b.r > H) { b.y = H - b.r; b.vy = -Math.abs(b.vy); }\n"
    "  }\n"
    "  let sample = state.balls.slice(0, 3);\n"
    "  let label = ('zqjs raylib c-abi | f:' + state.frameCount + ' balls:' + state.balls.length + ' caught:' + state.caughtCount).toUpperCase();\n"
    "  state.hud = label.slice(0, 64);\n"
    "  if (sample.length === 0 || state.hud.indexOf('ZQJS') !== 0) throw 'invariant';\n"
    "}\n"
    "\n"
    "function draw() {\n"
    "  clearBg(18, 22, 32);\n"
    "  drawRect(0, 0, W, 44, 30, 36, 52);\n"
    "  drawText(state.hud, 16, 12, 20, 230, 240, 255);\n"
    "  for (let i = 0; i < state.balls.length; i++) {\n"
    "    let b = state.balls[i];\n"
    "    drawCircle(b.x, b.y, b.r, b.c[0], b.c[1], b.c[2]);\n"
    "  }\n"
    "}\n"
    "\n"
    "initGame({ update: update, draw: draw, state: state });\n";

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static void print_diag(const char *name, const zqjs_diagnostic *d) {
    fprintf(stderr, "%s:%u:%u: error: %.*s\n",
        name, d->line, d->column, (int)d->message_len, d->message);
}

static void print_failure(const char *phase, const zqjs_failure *f) {
    fprintf(stderr, "zqjs %s failed (kind=%d): %.*s\n",
        phase, (int)f->kind, (int)f->message_len, f->message);
}

static int run_error_handling_check(void) {
    zqjs_options opts = zqjs_options_default();
    opts.limits.max_heap_bytes = 2u * 1024u * 1024u;
    opts.limits.max_steps = 50000ull;

    zqjs_runtime *rt = NULL;
    assert(zqjs_runtime_create(&opts, &rt) == ZQJS_OK);

    /* 1. Syntax error produces diagnostic cleanly without crashing. */
    const char *bad_syntax = "let = 1;";
    zqjs_diagnostic diag;
    zqjs_program *prog = NULL;
    zqjs_status st = zqjs_runtime_compile(rt, "bad_syntax.js", 13, bad_syntax, strlen(bad_syntax), &diag, &prog);
    assert(st == ZQJS_ERR_SOURCE);
    assert(prog == NULL);
    assert(diag.message_len > 0);

    /* 2. Uncaught JS exception during run populates failure cleanly. */
    const char *throws_js = "throw 'boom-from-js';";
    st = zqjs_runtime_compile(rt, "throws.js", 9, throws_js, strlen(throws_js), &diag, &prog);
    assert(st == ZQJS_OK);
    zqjs_failure fail;
    st = zqjs_runtime_run(rt, prog, &fail);
    assert(st == ZQJS_ERR_THROWN);
    assert(fail.kind == ZQJS_FAILURE_THROWN);
    assert(fail.message_len > 0);
    assert(zqjs_program_destroy(prog) == ZQJS_OK);

    assert(zqjs_runtime_collect_garbage(rt) == ZQJS_OK);
    assert(zqjs_runtime_destroy(rt) == ZQJS_OK);
    printf("error-handling smoke check passed\n");
    return 0;
}

static zqjs_status make_frame_input(
    zqjs_runtime *rt,
    double dt,
    bool space_down,
    zqjs_failure *fail,
    zqjs_handle *out_input
) {
    zqjs_handle input = ZQJS_HANDLE_NULL;
    zqjs_handle dt_h = ZQJS_HANDLE_NULL;
    zqjs_handle sp_h = ZQJS_HANDLE_NULL;
    zqjs_status st = zqjs_new_object(rt, &input);
    if (st != ZQJS_OK) return st;
    st = zqjs_new_number(rt, dt, &dt_h);
    if (st != ZQJS_OK) {
        zqjs_handle_release(rt, input);
        return st;
    }
    st = zqjs_new_boolean(rt, space_down, &sp_h);
    if (st != ZQJS_OK) {
        zqjs_handle_release(rt, dt_h);
        zqjs_handle_release(rt, input);
        return st;
    }
    st = zqjs_handle_set_property(rt, input, "dt", 2, dt_h, fail);
    zqjs_handle_release(rt, dt_h);
    if (st == ZQJS_OK) st = zqjs_handle_set_property(rt, input, "spaceDown", 9, sp_h, fail);
    zqjs_handle_release(rt, sp_h);
    if (st != ZQJS_OK) {
        zqjs_handle_release(rt, input);
        return st;
    }
    *out_input = input;
    return ZQJS_OK;
}

int main(int argc, char **argv) {
    assert((zqjs_abi_version() >> 16) == ZQJS_ABI_MAJOR);
    long headless_frames = -1;
    const char *script_path = NULL;
    bool verify_errors = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            headless_frames = strtol(argv[++i], NULL, 10);
            if (headless_frames < 0) headless_frames = 0;
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script_path = argv[++i];
        } else if (strcmp(argv[i], "--verify-errors") == 0) {
            verify_errors = true;
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--script path.js] [--verify-errors]\n", argv[0]);
            return 2;
        }
    }

    if (verify_errors && run_error_handling_check() != 0) {
        return 1;
    }

    char *loaded_script = NULL;
    const char *src = DEFAULT_SCRIPT;
    size_t src_len = strlen(DEFAULT_SCRIPT);
    const char *src_name = "raylib_demo.js";
    if (script_path != NULL) {
        loaded_script = read_file_all(script_path, &src_len);
        if (!loaded_script) {
            fprintf(stderr, "failed to read script: %s\n", script_path);
            return 1;
        }
        src = loaded_script;
        src_name = script_path;
    }

    zqjs_options opts = zqjs_options_default();
    opts.limits.max_heap_bytes = 8u * 1024u * 1024u;
    opts.limits.max_steps = 500000ull;
    opts.output_fn = on_print;

    zqjs_runtime *rt = NULL;
    if (zqjs_runtime_create(&opts, &rt) != ZQJS_OK) {
        free(loaded_script);
        fprintf(stderr, "failed to create zqjs runtime\n");
        return 1;
    }

    demo_host host = {
        .headless = (headless_frames >= 0),
        .update_cb = ZQJS_HANDLE_NULL,
        .draw_cb = ZQJS_HANDLE_NULL,
        .state_h = ZQJS_HANDLE_NULL,
    };

    assert(zqjs_runtime_define_global_function(rt, "initGame", 8, native_init_game, &host) == ZQJS_OK);
    assert(zqjs_runtime_define_global_function(rt, "clearBg", 7, native_clear_bg, &host) == ZQJS_OK);
    assert(zqjs_runtime_define_global_function(rt, "drawCircle", 10, native_draw_circle, &host) == ZQJS_OK);
    assert(zqjs_runtime_define_global_function(rt, "drawRect", 8, native_draw_rect, &host) == ZQJS_OK);
    assert(zqjs_runtime_define_global_function(rt, "drawText", 8, native_draw_text, &host) == ZQJS_OK);
    assert(zqjs_runtime_define_global_function(rt, "keyDown", 7, native_key_down, &host) == ZQJS_OK);

    int exit_code = 0;
    zqjs_diagnostic diag;
    zqjs_program *prog = NULL;
    if (zqjs_runtime_compile(rt, src_name, strlen(src_name), src, src_len, &diag, &prog) != ZQJS_OK) {
        print_diag(src_name, &diag);
        exit_code = 1;
        goto cleanup_rt;
    }

    zqjs_failure fail;
    if (zqjs_runtime_run(rt, prog, &fail) != ZQJS_OK) {
        print_failure("run", &fail);
        exit_code = 1;
        goto cleanup_prog;
    }

    if (host.update_cb.owner == NULL || host.draw_cb.owner == NULL || host.state_h.owner == NULL) {
        fprintf(stderr, "script did not register initGame({ update, draw, state })\n");
        exit_code = 1;
        goto cleanup_prog;
    }

    if (host.headless) {
        const double fixed_dt = 1.0 / 60.0;
        for (long frame = 0; frame < headless_frames; frame++) {
            zqjs_handle input_arg = ZQJS_HANDLE_NULL;
            zqjs_handle res = ZQJS_HANDLE_NULL;
            assert(make_frame_input(rt, fixed_dt, false, &fail, &input_arg) == ZQJS_OK);
            zqjs_status st = zqjs_runtime_call(rt, host.update_cb, NULL, &input_arg, 1, &fail, &res);
            assert(zqjs_handle_release(rt, input_arg) == ZQJS_OK);
            if (st != ZQJS_OK) {
                print_failure("update", &fail);
                exit_code = 1;
                goto cleanup_prog;
            }
            assert(zqjs_handle_release(rt, res) == ZQJS_OK);

            res = ZQJS_HANDLE_NULL;
            st = zqjs_runtime_call(rt, host.draw_cb, NULL, NULL, 0, &fail, &res);
            if (st != ZQJS_OK) {
                print_failure("draw", &fail);
                exit_code = 1;
                goto cleanup_prog;
            }
            assert(zqjs_handle_release(rt, res) == ZQJS_OK);
        }

        zqjs_handle fc_h = ZQJS_HANDLE_NULL;
        assert(zqjs_handle_get_property(rt, host.state_h, "frameCount", 10, &fail, &fc_h) == ZQJS_OK);
        double fc = 0.0;
        assert(zqjs_handle_number(rt, fc_h, &fc) == ZQJS_OK);
        assert((long)fc == headless_frames);
        assert(zqjs_handle_release(rt, fc_h) == ZQJS_OK);

        assert(zqjs_runtime_collect_garbage(rt) == ZQJS_OK);
        zqjs_memory mem;
        assert(zqjs_runtime_memory(rt, &mem) == ZQJS_OK);
        printf(
            "headless ok: frames=%ld clears=%llu rects=%llu circles=%llu texts=%llu hud=\"%s\" mem=%zuB\n",
            headless_frames,
            (unsigned long long)host.clear_calls,
            (unsigned long long)host.rect_calls,
            (unsigned long long)host.circle_calls,
            (unsigned long long)host.text_calls,
            host.last_text,
            mem.bytes
        );
    } else {
        InitWindow(800, 480, "ZQJS Raylib C-ABI Demo");
        SetTargetFPS(60);
        while (!WindowShouldClose()) {
            double dt = (double)GetFrameTime();
            zqjs_handle input_arg = ZQJS_HANDLE_NULL;
            zqjs_handle res = ZQJS_HANDLE_NULL;
            assert(make_frame_input(rt, dt, IsKeyDown(KEY_SPACE), &fail, &input_arg) == ZQJS_OK);
            zqjs_status st = zqjs_runtime_call(rt, host.update_cb, NULL, &input_arg, 1, &fail, &res);
            assert(zqjs_handle_release(rt, input_arg) == ZQJS_OK);
            if (st != ZQJS_OK) {
                print_failure("update", &fail);
                exit_code = 1;
                break;
            }
            assert(zqjs_handle_release(rt, res) == ZQJS_OK);

            BeginDrawing();
            res = ZQJS_HANDLE_NULL;
            st = zqjs_runtime_call(rt, host.draw_cb, NULL, NULL, 0, &fail, &res);
            EndDrawing();
            if (st != ZQJS_OK) {
                print_failure("draw", &fail);
                exit_code = 1;
                break;
            }
            assert(zqjs_handle_release(rt, res) == ZQJS_OK);
        }
        CloseWindow();
    }

cleanup_prog:
    if (host.update_cb.owner != NULL) assert(zqjs_handle_release(rt, host.update_cb) == ZQJS_OK);
    if (host.draw_cb.owner != NULL) assert(zqjs_handle_release(rt, host.draw_cb) == ZQJS_OK);
    if (host.state_h.owner != NULL) assert(zqjs_handle_release(rt, host.state_h) == ZQJS_OK);
    assert(zqjs_program_destroy(prog) == ZQJS_OK);
cleanup_rt:
    assert(zqjs_runtime_collect_garbage(rt) == ZQJS_OK);
    assert(zqjs_runtime_destroy(rt) == ZQJS_OK);
    free(loaded_script);
    return exit_code;
}
