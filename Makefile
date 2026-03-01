CC = gcc
CFLAGS = -Wall -Wextra -O2

# Detect platform
UNAME := $(shell uname -s)

# --- Game client ---
GAME_DIR = raylib
GAME_SRCS = $(filter-out $(GAME_DIR)/screenpicker.c, $(wildcard $(GAME_DIR)/*.c))
GAME_HDRS = $(wildcard $(GAME_DIR)/*.h)
GAME_TARGET = $(GAME_DIR)/game

GAME_CFLAGS = $(CFLAGS) -I$(GAME_DIR)

ifeq ($(UNAME),Darwin)
  GAME_CFLAGS += $(shell pkg-config --cflags raylib)
  GAME_LDFLAGS = $(shell pkg-config --libs raylib) -lm -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
  GAME_CFLAGS += -I/usr/local/include
  GAME_LDFLAGS = -lraylib -lm -lGL -lpthread -ldl -lrt
endif

# --- NFC bridge ---
NFC_DIR = nfc
NFC_SRC = $(NFC_DIR)/nfc_bridge.c
NFC_TARGET = $(NFC_DIR)/build/bridge
NFC_CFLAGS = $(CFLAGS)

# --- Asset files to bundle with exports ---
GAME_ASSETS = $(GAME_DIR)/env_layout.txt \
              $(wildcard $(GAME_DIR)/leaderboard.dat) \
              $(wildcard $(GAME_DIR)/leaderboard.json) \
              $(wildcard $(GAME_DIR)/*.obj) \
              $(wildcard $(GAME_DIR)/*.mtl)
GAME_ASSET_DIRS = assets fonts resources sfx music

# --- Targets ---
.PHONY: all game bridge clean run export

all: game bridge

game: $(GAME_TARGET)
bridge: $(NFC_TARGET)

run: game bridge
	cd $(GAME_DIR) && ./game

$(GAME_TARGET): $(GAME_SRCS) $(GAME_HDRS)
	$(CC) $(GAME_CFLAGS) -o $@ $(GAME_SRCS) $(GAME_LDFLAGS)

$(NFC_TARGET): $(NFC_SRC)
	@mkdir -p $(NFC_DIR)/build
	$(CC) $(NFC_CFLAGS) -o $@ $^

export: game
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

clean:
	rm -f $(GAME_TARGET) $(NFC_TARGET)
	$(MAKE) -f Makefile.win clean
	rm -rf export
