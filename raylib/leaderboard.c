#include "leaderboard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Load legacy binary leaderboard.dat (v1) for migration
static bool LoadLeaderboardLegacy(Leaderboard *lb)
{
    FILE *f = fopen(LEADERBOARD_FILE_LEGACY, "rb");
    if (!f) return false;

    unsigned int magic = 0;
    unsigned int version = 0;
    if (fread(&magic, sizeof(unsigned int), 1, f) != 1 || magic != LEADERBOARD_MAGIC_LEGACY) {
        fclose(f);
        return false;
    }
    if (fread(&version, sizeof(unsigned int), 1, f) != 1 || version != 1) {
        fclose(f);
        return false;
    }
    if (fread(&lb->entryCount, sizeof(int), 1, f) != 1) {
        lb->entryCount = 0;
        fclose(f);
        return false;
    }
    if (lb->entryCount < 0) lb->entryCount = 0;
    if (lb->entryCount > MAX_LEADERBOARD_ENTRIES) lb->entryCount = MAX_LEADERBOARD_ENTRIES;
    if (lb->entryCount > 0) {
        size_t nread = fread(lb->entries, sizeof(LeaderboardEntry), lb->entryCount, f);
        if ((int)nread != lb->entryCount) lb->entryCount = (int)nread;
    }
    fclose(f);
    return lb->entryCount > 0;
}

void LoadLeaderboard(Leaderboard *lb)
{
    memset(lb, 0, sizeof(Leaderboard));

    // Try loading JSON
    FILE *f = fopen(LEADERBOARD_FILE, "r");
    if (!f) {
        // Migration: try legacy binary format
        if (LoadLeaderboardLegacy(lb)) {
            printf("[Leaderboard] Migrated %d entries from legacy binary\n", lb->entryCount);
            SaveLeaderboard(lb);  // re-save as JSON immediately
        }
        return;
    }

    // Read entire file into buffer
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 1024 * 1024) { fclose(f); return; }

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    // Minimal JSON parser: find "entries" array, parse each entry object
    char *entries = strstr(buf, "\"entries\"");
    if (!entries) { free(buf); return; }

    // Walk through entry objects
    char *p = strchr(entries, '[');
    if (!p) { free(buf); return; }
    p++;

    while (lb->entryCount < MAX_LEADERBOARD_ENTRIES) {
        // Find next entry object
        char *objStart = strchr(p, '{');
        if (!objStart) break;
        char *objEnd = strchr(objStart, '}');
        if (!objEnd) break;

        LeaderboardEntry entry = {0};

        // Parse "player": "..."
        char *playerKey = strstr(objStart, "\"player\"");
        if (playerKey && playerKey < objEnd) {
            char *valStart = strchr(playerKey + 8, '"');
            if (valStart) {
                valStart++;
                char *valEnd = strchr(valStart, '"');
                if (valEnd) {
                    int len = (int)(valEnd - valStart);
                    if (len > 31) len = 31;
                    memcpy(entry.playerName, valStart, len);
                    entry.playerName[len] = '\0';
                }
            }
        }

        // Parse "round": N
        char *roundKey = strstr(objStart, "\"round\"");
        if (roundKey && roundKey < objEnd) {
            char *colon = strchr(roundKey + 7, ':');
            if (colon) entry.highestRound = atoi(colon + 1);
        }

        // Parse "units": ["code1", "code2", ...]
        char *unitsKey = strstr(objStart, "\"units\"");
        if (unitsKey) {
            char *arrStart = strchr(unitsKey + 7, '[');
            // Find the matching ']' — units array may extend past objEnd
            char *arrEnd = arrStart ? strchr(arrStart, ']') : NULL;
            if (arrStart && arrEnd) {
                char *up = arrStart + 1;
                entry.unitCount = 0;
                while (entry.unitCount < BLUE_TEAM_MAX_SIZE && up < arrEnd) {
                    char *qStart = strchr(up, '"');
                    if (!qStart || qStart >= arrEnd) break;
                    qStart++;
                    char *qEnd = strchr(qStart, '"');
                    if (!qEnd || qEnd >= arrEnd) break;

                    // Extract unit code string
                    int codeLen = (int)(qEnd - qStart);
                    char codeBuf[32];
                    if (codeLen > 31) codeLen = 31;
                    memcpy(codeBuf, qStart, codeLen);
                    codeBuf[codeLen] = '\0';

                    int typeIdx = 0;
                    AbilitySlot abilities[MAX_ABILITIES_PER_UNIT];
                    if (ParseUnitCode(codeBuf, &typeIdx, abilities)) {
                        int u = entry.unitCount;
                        entry.units[u].typeIndex = typeIdx;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            entry.units[u].abilities[a].abilityId = abilities[a].abilityId;
                            entry.units[u].abilities[a].level = abilities[a].level;
                        }
                        entry.unitCount++;
                    }
                    up = qEnd + 1;
                }
                // Update objEnd to be past the units array end
                if (arrEnd > objEnd) objEnd = arrEnd;
            }
        }

        lb->entries[lb->entryCount++] = entry;
        p = objEnd + 1;
    }

    free(buf);
}

void SaveLeaderboard(const Leaderboard *lb)
{
    FILE *f = fopen(LEADERBOARD_FILE, "w");
    if (!f) return;

    fprintf(f, "{\n  \"version\": %d,\n  \"entries\": [\n", LEADERBOARD_VERSION);
    for (int e = 0; e < lb->entryCount; e++) {
        const LeaderboardEntry *le = &lb->entries[e];
        fprintf(f, "    {\n");
        fprintf(f, "      \"player\": \"%s\",\n", le->playerName);
        fprintf(f, "      \"round\": %d,\n", le->highestRound);
        fprintf(f, "      \"units\": [");
        for (int u = 0; u < le->unitCount && u < BLUE_TEAM_MAX_SIZE; u++) {
            char codeBuf[16];
            // Convert SavedAbility to AbilitySlot for FormatUnitCode
            AbilitySlot slots[MAX_ABILITIES_PER_UNIT];
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                slots[a] = (AbilitySlot){
                    .abilityId = le->units[u].abilities[a].abilityId,
                    .level = le->units[u].abilities[a].level,
                    .cooldownRemaining = 0, .triggered = false
                };
            }
            FormatUnitCode(le->units[u].typeIndex, slots, codeBuf, sizeof(codeBuf));
            fprintf(f, "%s\"%s\"", (u > 0) ? ", " : "", codeBuf);
        }
        fprintf(f, "]\n");
        fprintf(f, "    }%s\n", (e < lb->entryCount - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
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
