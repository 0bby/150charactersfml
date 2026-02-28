#pragma once
#include "game.h"

#define MAX_LEADERBOARD_ENTRIES 50
#define LEADERBOARD_FILE "leaderboard.dat"
#define LEADERBOARD_MAGIC 0x4C445242  // "LDRB"
#define LEADERBOARD_VERSION 1

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

void LoadLeaderboard(Leaderboard *lb);
void SaveLeaderboard(const Leaderboard *lb);
void InsertLeaderboardEntry(Leaderboard *lb, const LeaderboardEntry *entry);
void SortLeaderboard(Leaderboard *lb);
