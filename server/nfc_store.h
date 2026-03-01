#pragma once
#include <stdint.h>

#define NFC_MAX_TAGS 256
#define NFC_UID_HEX_MAX 15  // 7 bytes -> 14 hex chars + null
#define NFC_MAX_ABILITIES 4

typedef struct {
    int8_t  abilityId;  // -1 = empty
    uint8_t level;
} NfcAbility;

#define NFC_NAME_MAX 32

typedef struct {
    char uidHex[NFC_UID_HEX_MAX];
    uint8_t typeIndex;
    uint8_t rarity;  // 0=common, 1=rare, 2=legendary
    NfcAbility abilities[NFC_MAX_ABILITIES];
    char name[NFC_NAME_MAX];  // custom creature name (empty = unnamed)
} NfcTagEntry;

typedef struct {
    int tagCount;
    NfcTagEntry tags[NFC_MAX_TAGS];
} NfcStore;

void NfcStoreLoad(NfcStore *store, const char *filepath);
void NfcStoreSave(const NfcStore *store, const char *filepath);

// Returns pointer to entry if found, NULL otherwise
NfcTagEntry *NfcStoreLookup(NfcStore *store, const char *uidHex);

// Returns 0 on success, -1 if store full, 1 if duplicate (updates existing)
int NfcStoreRegister(NfcStore *store, const char *uidHex, uint8_t typeIndex, uint8_t rarity);

// Update abilities for a tag. Returns 0 on success, -1 if tag not found.
int NfcStoreUpdateAbilities(NfcStore *store, const char *uidHex,
                            const NfcAbility abilities[], int count);

// Reset all abilities on a tag to empty (-1). Returns 0 on success, -1 if not found.
int NfcStoreResetAbilities(NfcStore *store, const char *uidHex);
