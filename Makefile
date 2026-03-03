CC = gcc
CFLAGS = -Wall -Wextra -O2
RAYLIB_VERSION = 5.5

# Detect platform
UNAME := $(shell uname -s)

# --- Game client ---
GAME_DIR = raylib
GAME_SRCS = $(filter-out $(GAME_DIR)/screenpicker.c, $(wildcard $(GAME_DIR)/*.c)) server/game_session.c
GAME_HDRS = $(wildcard $(GAME_DIR)/*.h) server/game_session.h
GAME_TARGET = $(GAME_DIR)/game

GAME_CFLAGS = $(CFLAGS) -I$(GAME_DIR) -Iserver

# --- Optional EOS P2P relay (build with: make USE_EOS=1 run) ---
ifdef USE_EOS
  EOS_SDK = deps/eos-sdk
  GAME_CFLAGS += -DUSE_EOS -I$(EOS_SDK)/include
  # Link against EOS shared library (no -static when using EOS)
  ifeq ($(UNAME),Linux)
    EOS_LDFLAGS = -L$(EOS_SDK)/Bin -lEOSSDK-Linux-Shipping -Wl,-rpath,'$$ORIGIN'
  else ifeq ($(UNAME),Darwin)
    EOS_LDFLAGS = -L$(EOS_SDK)/Bin -lEOSSDK-Mac-Shipping -Wl,-rpath,@executable_path
  endif
endif

ifeq ($(UNAME),Darwin)
  GAME_CFLAGS += $(shell pkg-config --cflags raylib)
  GAME_LDFLAGS = $(shell pkg-config --libs raylib) -lm -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
  ifdef STATIC_RAYLIB
    RAYLIB_LINUX = deps/raylib-linux
    GAME_CFLAGS += -I$(RAYLIB_LINUX)/include
    GAME_LDFLAGS = $(RAYLIB_LINUX)/lib/libraylib.a -lm -lGL -lpthread -ldl -lrt
  else
    GAME_CFLAGS += $(shell pkg-config --cflags raylib 2>/dev/null || echo -I/usr/local/include)
    GAME_LDFLAGS = -lraylib -lm -lGL -lpthread -ldl -lrt
  endif
endif

# --- Asset files to bundle with exports ---
GAME_ASSETS = $(GAME_DIR)/env_layout.txt \
              $(wildcard $(GAME_DIR)/leaderboard.dat) \
              $(wildcard $(GAME_DIR)/leaderboard.json) \
              $(wildcard $(GAME_DIR)/*.obj) \
              $(wildcard $(GAME_DIR)/*.mtl)
GAME_ASSET_DIRS = assets fonts resources sfx music

GAME_NAME = relic-rivals
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

# --- Targets ---
.PHONY: all game clean clean-game clean-deps run export release deps

all: game

game: $(GAME_TARGET)

run: game
	cd $(GAME_DIR) && ./game

$(GAME_TARGET): $(GAME_SRCS) $(GAME_HDRS)
	$(CC) $(GAME_CFLAGS) -o $@ $(GAME_SRCS) $(GAME_LDFLAGS) $(EOS_LDFLAGS)

deps:
	@if [ ! -f deps/raylib-linux/lib/libraylib.a ]; then \
		echo "=== Building static raylib $(RAYLIB_VERSION) for Linux ==="; \
		rm -rf deps/raylib-src; \
		git clone --depth 1 --branch $(RAYLIB_VERSION) https://github.com/raysan5/raylib.git deps/raylib-src; \
		cd deps/raylib-src/src && $(MAKE) PLATFORM=PLATFORM_DESKTOP GRAPHICS=GRAPHICS_API_OPENGL_33; \
		mkdir -p ../../raylib-linux/lib ../../raylib-linux/include; \
		cp libraylib.a ../../raylib-linux/lib/; \
		cp raylib.h raymath.h rlgl.h ../../raylib-linux/include/; \
		cd ../../.. && rm -rf deps/raylib-src; \
		echo "=== Static raylib built at deps/raylib-linux/ ==="; \
	else \
		echo "=== deps/raylib-linux/lib/libraylib.a already exists, skipping ==="; \
	fi

export: deps
	@echo "=== Building Linux (static) ==="
	$(MAKE) clean-game
	$(MAKE) STATIC_RAYLIB=1 game
	@echo "=== Building Windows ==="
	$(MAKE) -f Makefile.win
	@echo "=== Assembling export ==="
	@rm -rf export
	@mkdir -p export/linux export/windows
	@cp $(GAME_TARGET) export/linux/
	@cp $(GAME_DIR)/game.exe export/windows/
	@for dir in $(GAME_ASSET_DIRS); do \
		if [ -d "$(GAME_DIR)/$$dir" ]; then \
			cp -r "$(GAME_DIR)/$$dir" export/linux/; \
			cp -r "$(GAME_DIR)/$$dir" export/windows/; \
		fi; \
	done
	@for f in $(GAME_ASSETS); do \
		[ -f "$$f" ] && cp "$$f" export/linux/ && cp "$$f" export/windows/ || true; \
	done
	@echo "=== Export complete ==="
	@echo "  Linux:   export/linux/game"
	@echo "  Windows: export/windows/game.exe"

release: export
	@echo "=== Zipping ==="
	cd export && zip -qr ../$(GAME_NAME)-$(VERSION)-linux.zip linux/
	cd export && zip -qr ../$(GAME_NAME)-$(VERSION)-windows.zip windows/
	@echo "=== Creating GitHub release $(VERSION) ==="
	gh release create $(VERSION) \
		$(GAME_NAME)-$(VERSION)-linux.zip \
		$(GAME_NAME)-$(VERSION)-windows.zip \
		--title "$(GAME_NAME) $(VERSION)" \
		--notes "Linux and Windows builds"
	@rm -f $(GAME_NAME)-$(VERSION)-linux.zip $(GAME_NAME)-$(VERSION)-windows.zip
	@echo "=== Released $(VERSION) ==="

clean: clean-game
	$(MAKE) -f Makefile.win clean
	rm -rf export
	rm -f $(GAME_NAME)-*.zip

clean-game:
	rm -f $(GAME_TARGET)

clean-deps:
	rm -rf deps/raylib-linux deps/raylib-src
