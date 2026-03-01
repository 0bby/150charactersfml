#include "nfc_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void init_empty_abilities(NfcTagEntry *e)
{
    for (int i = 0; i < NFC_MAX_ABILITIES; i++) {
        e->abilities[i].abilityId = -1;
        e->abilities[i].level = 0;
    }
}

void NfcStoreLoad(NfcStore *store, const char *filepath)
{
    memset(store, 0, sizeof(NfcStore));

    FILE *f = fopen(filepath, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) { fclose(f); return; }

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    // Find "tags" array
    char *tags = strstr(buf, "\"tags\"");
    if (!tags) { free(buf); return; }

    char *p = strchr(tags, '[');
    if (!p) { free(buf); return; }
    p++;

    while (store->tagCount < NFC_MAX_TAGS) {
        char *objStart = strchr(p, '{');
        if (!objStart) break;
        // Find matching closing brace — need to handle nested "abilities" array
        // Look for "abilities" array first to skip past its braces
        char *objEnd = NULL;
        char *abKey = strstr(objStart, "\"abilities\"");
        if (abKey) {
            char *abEnd = strchr(abKey, ']');
            if (abEnd) {
                objEnd = strchr(abEnd, '}');
            }
        }
        if (!objEnd) {
            objEnd = strchr(objStart, '}');
        }
        if (!objEnd) break;

        NfcTagEntry entry = {0};
        init_empty_abilities(&entry);

        // Parse "uid": "..."
        char *uidKey = strstr(objStart, "\"uid\"");
        if (uidKey && uidKey < objEnd) {
            char *valStart = strchr(uidKey + 5, '"');
            if (valStart) {
                valStart++;
                char *valEnd = strchr(valStart, '"');
                if (valEnd) {
                    int len = (int)(valEnd - valStart);
                    if (len > NFC_UID_HEX_MAX - 1) len = NFC_UID_HEX_MAX - 1;
                    memcpy(entry.uidHex, valStart, len);
                    entry.uidHex[len] = '\0';
                }
            }
        }

        // Parse "type": N
        char *typeKey = strstr(objStart, "\"type\"");
        if (typeKey && typeKey < objEnd) {
            char *colon = strchr(typeKey + 6, ':');
            if (colon) entry.typeIndex = (uint8_t)atoi(colon + 1);
        }

        // Parse "rarity": N
        char *rarityKey = strstr(objStart, "\"rarity\"");
        if (rarityKey && rarityKey < objEnd) {
            char *colon = strchr(rarityKey + 8, ':');
            if (colon) entry.rarity = (uint8_t)atoi(colon + 1);
        }

        // Parse "abilities": [[id, level], ...]
        if (abKey && abKey < objEnd) {
            char *arrStart = strchr(abKey, '[');
            if (arrStart) {
                char *arrEnd = strchr(arrStart, ']');
                if (arrEnd) {
                    // Parse inner [id, level] pairs
                    char *ap = arrStart + 1;
                    int ai = 0;
                    while (ai < NFC_MAX_ABILITIES && ap < arrEnd) {
                        char *innerStart = strchr(ap, '[');
                        if (!innerStart || innerStart >= arrEnd) break;
                        char *innerEnd = strchr(innerStart, ']');
                        if (!innerEnd) break;
                        // Parse "id, level" from between [ and ]
                        int id = atoi(innerStart + 1);
                        char *comma = strchr(innerStart, ',');
                        int level = 0;
                        if (comma && comma < innerEnd) level = atoi(comma + 1);
                        entry.abilities[ai].abilityId = (int8_t)id;
                        entry.abilities[ai].level = (uint8_t)level;
                        ai++;
                        ap = innerEnd + 1;
                    }
                }
            }
        }

        // Parse "name": "..." (optional, backwards compatible)
        char *nameKey = strstr(objStart, "\"name\"");
        if (nameKey && nameKey < objEnd) {
            char *valStart = strchr(nameKey + 6, '"');
            if (valStart) {
                valStart++;
                char *valEnd = strchr(valStart, '"');
                if (valEnd) {
                    int len = (int)(valEnd - valStart);
                    if (len > NFC_NAME_MAX - 1) len = NFC_NAME_MAX - 1;
                    memcpy(entry.name, valStart, len);
                    entry.name[len] = '\0';
                }
            }
        }

        if (entry.uidHex[0]) {
            store->tags[store->tagCount++] = entry;
        }

        p = objEnd + 1;
    }

    free(buf);
}

void NfcStoreSave(const NfcStore *store, const char *filepath)
{
    FILE *f = fopen(filepath, "w");
    if (!f) return;

    fprintf(f, "{\n  \"version\": 1,\n  \"tags\": [\n");
    for (int i = 0; i < store->tagCount; i++) {
        const NfcTagEntry *e = &store->tags[i];
        fprintf(f, "    {\"uid\": \"%s\", \"type\": %d, \"rarity\": %d, \"name\": \"%s\", \"abilities\": [",
                e->uidHex, e->typeIndex, e->rarity, e->name);
        for (int a = 0; a < NFC_MAX_ABILITIES; a++) {
            fprintf(f, "%s[%d, %d]", (a > 0) ? ", " : "",
                    e->abilities[a].abilityId, e->abilities[a].level);
        }
        fprintf(f, "]}%s\n", (i < store->tagCount - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

NfcTagEntry *NfcStoreLookup(NfcStore *store, const char *uidHex)
{
    for (int i = 0; i < store->tagCount; i++) {
        if (strcasecmp(store->tags[i].uidHex, uidHex) == 0)
            return &store->tags[i];
    }
    return NULL;
}

int NfcStoreRegister(NfcStore *store, const char *uidHex, uint8_t typeIndex, uint8_t rarity)
{
    // Check for existing entry — update it
    NfcTagEntry *existing = NfcStoreLookup(store, uidHex);
    if (existing) {
        existing->typeIndex = typeIndex;
        existing->rarity = rarity;
        return 1;
    }

    if (store->tagCount >= NFC_MAX_TAGS) return -1;

    NfcTagEntry *e = &store->tags[store->tagCount++];
    int len = (int)strlen(uidHex);
    if (len > NFC_UID_HEX_MAX - 1) len = NFC_UID_HEX_MAX - 1;
    memcpy(e->uidHex, uidHex, len);
    e->uidHex[len] = '\0';
    e->typeIndex = typeIndex;
    e->rarity = rarity;
    init_empty_abilities(e);
    return 0;
}

int NfcStoreUpdateAbilities(NfcStore *store, const char *uidHex,
                            const NfcAbility abilities[], int count)
{
    NfcTagEntry *entry = NfcStoreLookup(store, uidHex);
    if (!entry) return -1;

    for (int i = 0; i < NFC_MAX_ABILITIES; i++) {
        if (i < count) {
            entry->abilities[i] = abilities[i];
        } else {
            entry->abilities[i].abilityId = -1;
            entry->abilities[i].level = 0;
        }
    }
    return 0;
}

int NfcStoreResetAbilities(NfcStore *store, const char *uidHex)
{
    NfcTagEntry *entry = NfcStoreLookup(store, uidHex);
    if (!entry) return -1;
    init_empty_abilities(entry);
    return 0;
}
