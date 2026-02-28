CC = gcc
CFLAGS = -Wall -Wextra -O2

# Detect platform
UNAME := $(shell uname -s)

# --- Game client ---
GAME_DIR = raylib
GAME_SRCS = $(GAME_DIR)/main.c \
            $(GAME_DIR)/helpers.c \
            $(GAME_DIR)/leaderboard.c \
            $(GAME_DIR)/combat_sim.c \
            $(GAME_DIR)/net_client.c \
            $(GAME_DIR)/net_common.c
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

# --- Targets ---
.PHONY: all game bridge clean run

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

clean:
	rm -f $(GAME_TARGET) $(NFC_TARGET)
