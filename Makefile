ZQJS_DIR ?= ../zqjs
CC       ?= cc
CFLAGS   := -std=c11 -O2 -Wall -Wextra -Werror

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
RAYLIB_PREFIX ?= /opt/homebrew/opt/raylib
RAYLIB_CFLAGS := -I$(RAYLIB_PREFIX)/include
RAYLIB_LIBS   := -L$(RAYLIB_PREFIX)/lib -lraylib -framework CoreVideo -framework IOKit -framework Cocoa -framework GLUT -framework OpenGL
else
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib)
endif

ZQJS_INC := $(ZQJS_DIR)/zig-out/include
ZQJS_LIB := $(ZQJS_DIR)/zig-out/lib/libzqjs.a

.PHONY: all zqjs run smoke clean

all: build/zqjs-raylib

zqjs:
	cd $(ZQJS_DIR) && zig build -Doptimize=ReleaseFast

build/zqjs-raylib: src/main.c | zqjs
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(ZQJS_INC) $(RAYLIB_CFLAGS) src/main.c $(ZQJS_LIB) $(RAYLIB_LIBS) -lpthread -lm -o $@

run: build/zqjs-raylib
	./build/zqjs-raylib game.js

smoke: build/zqjs-raylib
	./build/zqjs-raylib --smoke game.js

clean:
	rm -rf build
