#pragma once
#include <stdint.h>
#include "game.h"
#include "helpers.h"

#define MAX_LEADERBOARD_ENTRIES 50
#define LEADERBOARD_FILE "leaderboard.json"
#define LEADERBOARD_FILE_LEGACY "leaderboard.dat"
#define LEADERBOARD_MAGIC_LEGACY 0x4C445242  // "LDRB" (for migration)
#define LEADERBOARD_VERSION 2

typedef struct {
    int abilityId;  // -1 = empty
    int level;      // 0-2
} SavedAbility;

typedef struct {
    int typeIndex;
    SavedAbility abilities[MAX_ABILITIES_PER_UNIT];
} SavedUnit;

typedef struct {
    char playerName[32];
    int highestRound;       // 1-indexed milestone round
    int unitCount;          // 1-4 units set in stone
    SavedUnit units[BLUE_TEAM_MAX_SIZE];
} LeaderboardEntry;

typedef struct {
    int entryCount;
    LeaderboardEntry entries[MAX_LEADERBOARD_ENTRIES];
} Leaderboard;

void LoadLeaderboard(Leaderboard *lb, const char *filepath);
void SaveLeaderboard(const Leaderboard *lb, const char *filepath);
void InsertLeaderboardEntry(Leaderboard *lb, const LeaderboardEntry *entry);
void SortLeaderboard(Leaderboard *lb);

// Binary serialization for network transfer (55 bytes per entry)
#define LEADERBOARD_ENTRY_NET_SIZE 55
int serialize_leaderboard_entry(const LeaderboardEntry *entry, uint8_t *buf, int bufSize);
int deserialize_leaderboard_entry(const uint8_t *buf, int bufSize, LeaderboardEntry *entry);
