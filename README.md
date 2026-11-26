# zqjs-raylib

Live hot-reloadable C11 + Raylib game sandbox powered by [zqjs](https://github.com/ngnsr/zqjs) (`libzqjs.a` + `zqjs.h`).

## Why Embed JS in C?

- **Live hot-reload without recompiling C:** edit [game.js](game.js) (orb count, speed, gravity, colors, rules) and save — the running Raylib window reloads the script automatically (or press `R` anytime).
- **Hard per-frame sandbox:** `game.js` runs under a strict `500,000`-step per-frame budget and `8 MiB` heap cap. Uncomment `while (true) {}` or `throw new Error("live crash!")` in `game.js` and save: the host catches it in milliseconds, shows an in-window error banner, and seamlessly resumes as soon as you fix and save the file — zero freezes, zero crashes.
- **Zero-dependency engine:** links only static `libzqjs.a` + `zqjs.h` alongside Raylib.

## Controls

- `WASD` / `Arrow keys` — move
- `Space` — radial shockwave pulse
- `Left Click` (hold) — gravity well
- `R` — reload `game.js` immediately

## Requirements

- [zqjs](https://github.com/ngnsr/zqjs) checked out alongside this repo (`../zqjs`, or pass `ZQJS_DIR=/path/to/zqjs`), built with **Zig 0.17.1**
- **Raylib** (`brew install raylib` on macOS, or `pkg-config` `raylib` on Linux)

## Build & Run

```sh
# Build libzqjs.a (ReleaseFast) and the Raylib host binary
make

# Launch the interactive window (edit game.js live while running!)
make run

# Headless smoke test (60 frames + hot-reload + syntax/throw/infinite-loop isolation)
make smoke
```

## License

Licensed under the [Apache License, Version 2.0](LICENSE).
