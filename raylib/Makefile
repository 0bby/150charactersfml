CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lraylib -lm -lGL -lpthread -ldl

SRC = main.c helpers.c abilities_cast.c leaderboard.c combat_sim.c net_common.c net_client.c
OBJ = $(SRC:.c=.o)
TARGET = game

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
