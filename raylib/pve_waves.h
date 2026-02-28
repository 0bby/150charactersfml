#pragma once
#include "game.h"

//------------------------------------------------------------------------------------
// PVE Wave Definitions
// Each wave specifies enemy units for PVE rounds.
// Both players fight the same wave independently.
//------------------------------------------------------------------------------------

#define MAX_WAVE_UNITS 4
#define PVE_WAVE_COUNT 3

typedef struct {
    int typeIndex;
    float posX, posZ;
    struct { int abilityId; int level; } abilities[MAX_ABILITIES_PER_UNIT];
    int abilityCount;
} WaveUnit;

typedef struct {
    WaveUnit units[MAX_WAVE_UNITS];
    int unitCount;
} PveWave;

static const PveWave PVE_WAVES[PVE_WAVE_COUNT] = {
    // Wave 1: 2 goblins with 1 ability each
    {
        .unitCount = 2,
        .units = {
            { .typeIndex = 1, .posX = -15.0f, .posZ = -30.0f,
              .abilityCount = 1, .abilities = {{ ABILITY_MAGIC_MISSILE, 0 }} },
            { .typeIndex = 1, .posX = 15.0f, .posZ = -30.0f,
              .abilityCount = 1, .abilities = {{ ABILITY_BLOOD_RAGE, 0 }} },
        },
    },
    // Wave 2: 2 mushrooms with 2 abilities each
    {
        .unitCount = 2,
        .units = {
            { .typeIndex = 0, .posX = -10.0f, .posZ = -25.0f,
              .abilityCount = 2, .abilities = {{ ABILITY_DIG, 0 }, { ABILITY_VACUUM, 0 }} },
            { .typeIndex = 0, .posX = 10.0f, .posZ = -25.0f,
              .abilityCount = 2, .abilities = {{ ABILITY_CHAIN_FROST, 0 }, { ABILITY_BLOOD_RAGE, 0 }} },
        },
    },
    // Wave 3: 3 goblins, 2 abilities each (harder)
    {
        .unitCount = 3,
        .units = {
            { .typeIndex = 1, .posX = -20.0f, .posZ = -35.0f,
              .abilityCount = 2, .abilities = {{ ABILITY_MAGIC_MISSILE, 1 }, { ABILITY_BLOOD_RAGE, 0 }} },
            { .typeIndex = 1, .posX = 0.0f, .posZ = -30.0f,
              .abilityCount = 2, .abilities = {{ ABILITY_CHAIN_FROST, 0 }, { ABILITY_VACUUM, 0 }} },
            { .typeIndex = 1, .posX = 20.0f, .posZ = -35.0f,
              .abilityCount = 2, .abilities = {{ ABILITY_MAGIC_MISSILE, 0 }, { ABILITY_DIG, 0 }} },
        },
    },
};

// Round structure: which rounds are PVE vs PVP
// Rounds 1-2: PVE, Round 3: PVP, Round 4: PVE, Rounds 5+: PVP
static inline int IsPveRound(int roundIndex)
{
    // roundIndex is 0-based
    return (roundIndex == 0 || roundIndex == 1 || roundIndex == 3);
}

static inline int GetPveWaveIndex(int roundIndex)
{
    // Map round to wave
    if (roundIndex == 0) return 0;
    if (roundIndex == 1) return 1;
    if (roundIndex == 3) return 2;
    return 0; // fallback
}
