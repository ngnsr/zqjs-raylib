# zqjs-raylib

Standalone C11 Raylib host demo embedded with [zqjs](https://github.com/ngnsr/zqjs) (`libzqjs.a` + `zqjs.h`).

JavaScript owns the game loop (`initGame()` returning `{ update(dt), draw(), state }`), while C owns the Raylib window, input/rendering primitives, and deterministic headless smoke testing.

## Requirements
- [zqjs](https://github.com/ngnsr/zqjs) checked out alongside this repo (`../zqjs`, or pass `ZQJS_DIR=/path/to/zqjs`), built with **Zig 0.17.1**
- **Raylib** (`brew install raylib` on macOS, or `pkg-config` `raylib` on Linux)

## Build & Run
```sh
# Build libzqjs.a and the Raylib demo binary
make

# Interactive GUI window
make run

# Headless 60-frame + error-handling smoke test
make smoke
```
