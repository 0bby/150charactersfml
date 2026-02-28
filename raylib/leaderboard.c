#include "leaderboard.h"
#include <stdio.h>
#include <string.h>

void LoadLeaderboard(Leaderboard *lb)
{
    memset(lb, 0, sizeof(Leaderboard));
    FILE *f = fopen(LEADERBOARD_FILE, "rb");
    if (!f) return;  // missing file = empty leaderboard

    unsigned int magic = 0;
    unsigned int version = 0;
    if (fread(&magic, sizeof(unsigned int), 1, f) != 1 || magic != LEADERBOARD_MAGIC) {
        fclose(f);
        return;
    }
    if (fread(&version, sizeof(unsigned int), 1, f) != 1 || version != LEADERBOARD_VERSION) {
        fclose(f);
        return;
    }
    if (fread(&lb->entryCount, sizeof(int), 1, f) != 1) {
        lb->entryCount = 0;
        fclose(f);
        return;
    }
    if (lb->entryCount < 0) lb->entryCount = 0;
    if (lb->entryCount > MAX_LEADERBOARD_ENTRIES) lb->entryCount = MAX_LEADERBOARD_ENTRIES;
    if (lb->entryCount > 0) {
        size_t read = fread(lb->entries, sizeof(LeaderboardEntry), lb->entryCount, f);
        if ((int)read != lb->entryCount) lb->entryCount = (int)read;
    }
    fclose(f);
}

void SaveLeaderboard(const Leaderboard *lb)
{
    FILE *f = fopen(LEADERBOARD_FILE, "wb");
    if (!f) return;
    unsigned int magic = LEADERBOARD_MAGIC;
    unsigned int version = LEADERBOARD_VERSION;
    fwrite(&magic, sizeof(unsigned int), 1, f);
    fwrite(&version, sizeof(unsigned int), 1, f);
    fwrite(&lb->entryCount, sizeof(int), 1, f);
    if (lb->entryCount > 0)
        fwrite(lb->entries, sizeof(LeaderboardEntry), lb->entryCount, f);
    fclose(f);
}

void SortLeaderboard(Leaderboard *lb)
{
    // Simple insertion sort by highestRound descending
    for (int i = 1; i < lb->entryCount; i++) {
        LeaderboardEntry key = lb->entries[i];
        int j = i - 1;
        while (j >= 0 && lb->entries[j].highestRound < key.highestRound) {
            lb->entries[j + 1] = lb->entries[j];
            j--;
        }
        lb->entries[j + 1] = key;
    }
}

void InsertLeaderboardEntry(Leaderboard *lb, const LeaderboardEntry *entry)
{
    if (lb->entryCount < MAX_LEADERBOARD_ENTRIES) {
        lb->entries[lb->entryCount] = *entry;
        lb->entryCount++;
    } else {
        // Replace the lowest-ranked entry if new entry is better
        SortLeaderboard(lb);
        int last = lb->entryCount - 1;
        if (entry->highestRound > lb->entries[last].highestRound) {
            lb->entries[last] = *entry;
        }
    }
    SortLeaderboard(lb);
}
