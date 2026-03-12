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

# --- EOS P2P relay (enabled by default, disable with: make USE_EOS=0) ---
ifneq ($(USE_EOS),0)
  EOS_SDK = deps/eos-sdk
  GAME_CFLAGS += -DUSE_EOS -I$(EOS_SDK)/include
  # Link against EOS shared library (no -static when using EOS)
  ifeq ($(UNAME),Linux)
    EOS_LIB_NAME = libEOSSDK-Linux-Shipping.so
    EOS_LDFLAGS = -L$(EOS_SDK)/Bin -lEOSSDK-Linux-Shipping -Wl,-rpath,'$$ORIGIN'
  else ifeq ($(UNAME),Darwin)
    EOS_LIB_NAME = libEOSSDK-Mac-Shipping.dylib
    EOS_LDFLAGS = -L$(EOS_SDK)/Bin -lEOSSDK-Mac-Shipping -Wl,-rpath,@executable_path
  endif
endif

ifeq ($(UNAME),Darwin)
  ifdef STATIC_RAYLIB
    RAYLIB_MAC = deps/raylib-mac
    GAME_CFLAGS += -I$(RAYLIB_MAC)/include
    GAME_LDFLAGS = $(RAYLIB_MAC)/lib/libraylib.a -lm -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
  else
    GAME_CFLAGS += $(shell pkg-config --cflags raylib)
    GAME_LDFLAGS = $(shell pkg-config --libs raylib) -lm -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
  endif
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
.PHONY: all game clean clean-game clean-deps run export export-mac release deps deps-mac

all: game

game: $(GAME_TARGET)

run: game
	cd $(GAME_DIR) && ./game

$(GAME_TARGET): $(GAME_SRCS) $(GAME_HDRS)
	$(CC) $(GAME_CFLAGS) -o $@ $(GAME_SRCS) $(GAME_LDFLAGS) $(EOS_LDFLAGS)
ifneq ($(USE_EOS),0)
	@ln -sf ../$(EOS_SDK)/Bin/$(EOS_LIB_NAME) $(GAME_DIR)/$(EOS_LIB_NAME)
endif

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

deps-mac:
	@if [ ! -f deps/raylib-mac/lib/libraylib.a ]; then \
		echo "=== Building static raylib $(RAYLIB_VERSION) for macOS ==="; \
		rm -rf deps/raylib-src; \
		git clone --depth 1 --branch $(RAYLIB_VERSION) https://github.com/raysan5/raylib.git deps/raylib-src; \
		cd deps/raylib-src/src && $(MAKE) PLATFORM=PLATFORM_DESKTOP GRAPHICS=GRAPHICS_API_OPENGL_33; \
		mkdir -p ../../raylib-mac/lib ../../raylib-mac/include; \
		cp libraylib.a ../../raylib-mac/lib/; \
		cp raylib.h raymath.h rlgl.h ../../raylib-mac/include/; \
		cd ../../.. && rm -rf deps/raylib-src; \
		echo "=== Static raylib built at deps/raylib-mac/ ==="; \
	else \
		echo "=== deps/raylib-mac/lib/libraylib.a already exists, skipping ==="; \
	fi

export-linux: deps
	@echo "=== Building Linux (static) ==="
	$(MAKE) clean-game
	$(MAKE) STATIC_RAYLIB=1 game
	@echo "=== Assembling Linux export ==="
	@mkdir -p export/linux
	@cp $(GAME_TARGET) export/linux/
ifneq ($(USE_EOS),0)
	@cp $(EOS_SDK)/Bin/$(EOS_LIB_NAME) export/linux/
endif
	@for dir in $(GAME_ASSET_DIRS); do \
		if [ -d "$(GAME_DIR)/$$dir" ]; then \
			cp -r "$(GAME_DIR)/$$dir" export/linux/; \
		fi; \
	done
	@for f in $(GAME_ASSETS); do \
		[ -f "$$f" ] && cp "$$f" export/linux/ || true; \
	done
	@echo "=== Linux Export complete: export/linux/game ==="

export-mac: deps-mac
	@echo "=== Building macOS (static) ==="
	$(MAKE) clean-game
	$(MAKE) STATIC_RAYLIB=1 game
	@echo "=== Assembling macOS export ==="
	@mkdir -p export/mac
	@cp $(GAME_TARGET) export/mac/
ifneq ($(USE_EOS),0)
	@cp $(EOS_SDK)/Bin/libEOSSDK-Mac-Shipping.dylib export/mac/
endif
	@for dir in $(GAME_ASSET_DIRS); do \
		if [ -d "$(GAME_DIR)/$$dir" ]; then \
			cp -r "$(GAME_DIR)/$$dir" export/mac/; \
		fi; \
	done
	@for f in $(GAME_ASSETS); do \
		[ -f "$$f" ] && cp "$$f" export/mac/ || true; \
	done
	@echo "=== macOS Export complete: export/mac/game ==="

export-windows:
	@echo "=== Building Windows ==="
	$(MAKE) -f Makefile.win USE_EOS=1
	@echo "=== Assembling Windows export ==="
	@mkdir -p export/windows
	@cp $(GAME_DIR)/game.exe export/windows/
	@cp deps/eos-sdk/Bin/EOSSDK-Win64-Shipping.dll export/windows/
	@for dir in $(GAME_ASSET_DIRS); do \
		if [ -d "$(GAME_DIR)/$$dir" ]; then \
			cp -r "$(GAME_DIR)/$$dir" export/windows/; \
		fi; \
	done
	@for f in $(GAME_ASSETS); do \
		[ -f "$$f" ] && cp "$$f" export/windows/ || true; \
	done
	@echo "=== Windows Export complete: export/windows/game.exe ==="

ifeq ($(UNAME),Windows_NT)
export: export-windows
release: export
	@echo "=== Zipping ==="
	cd export && zip -qr ../$(GAME_NAME)-$(VERSION)-windows.zip windows/
	@echo "=== Creating GitHub release $(VERSION) ==="
	gh release create $(VERSION) \
		$(GAME_NAME)-$(VERSION)-windows.zip \
		--title "$(GAME_NAME) $(VERSION)" \
		--target local-only \
		$(if $(NOTES),--notes "$(NOTES)",--notes "")
	@rm -f $(GAME_NAME)-$(VERSION)-windows.zip
	@echo "=== Released $(VERSION) ==="
else
export: export-linux export-windows

release: export-windows
	@echo "=== Zipping ==="
	cd export && zip -qr ../$(GAME_NAME)-$(VERSION)-windows.zip windows/
	@echo "=== Creating GitHub release $(VERSION) ==="
	gh release create $(VERSION) \
		$(GAME_NAME)-$(VERSION)-windows.zip \
		--title "$(GAME_NAME) $(VERSION)" \
		--target local-only \
		$(if $(NOTES),--notes "$(NOTES)",--notes "")
	@rm -f $(GAME_NAME)-$(VERSION)-windows.zip
	@echo "=== Released $(VERSION) — CI will add Linux + Mac builds ==="
endif

clean: clean-game
	$(MAKE) -f Makefile.win clean
	rm -rf export
	rm -f $(GAME_NAME)-*.zip

clean-game:
	rm -f $(GAME_TARGET)

clean-deps:
	rm -rf deps/raylib-linux deps/raylib-src
