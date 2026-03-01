CC = gcc
CFLAGS = -DSERVER_BUILD -Wall -Wextra -O2 -I../raylib
LDFLAGS = -lm

RAYLIB_DIR = ../raylib

# Shared game logic (no raylib dependency)
SHARED_SRCS = $(RAYLIB_DIR)/combat_sim.c \
              $(RAYLIB_DIR)/helpers.c \
              $(RAYLIB_DIR)/net_common.c \
              $(RAYLIB_DIR)/leaderboard.c \
              $(RAYLIB_DIR)/abilities_cast.c

SERVER_SRCS = server_main.c game_session.c server_stubs.c nfc_store.c

ALL_SRCS = $(SERVER_SRCS) $(SHARED_SRCS)

TARGET = server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(ALL_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
