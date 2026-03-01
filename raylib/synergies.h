#pragma once

//------------------------------------------------------------------------------------
// Synergy Definitions — data-driven synergy table
// Add new synergies by appending to SYNERGY_DEFS[].
//------------------------------------------------------------------------------------

typedef struct {
    const char *name;
    int requiredTypes[4];    // unit type indices that count toward this synergy
    int requiredTypeCount;
    int targetType;          // which type gets the buff (-1 = all matching)
    int tierCount;
    struct {
        int minUnits;
        float speedMult;     // 1.0 = no change
        float hpMult;
        float dmgMult;
    } tiers[4];
} SynergyDef;

static const SynergyDef SYNERGY_DEFS[] = {
    {
        .name = "Goblin Swarm",
        .requiredTypes = { 1 },       // Goblin
        .requiredTypeCount = 1,
        .targetType = 1,              // buff goes to goblins
        .tierCount = 3,
        .tiers = {
            { .minUnits = 2, .speedMult = 1.15f, .hpMult = 1.0f, .dmgMult = 1.0f },
            { .minUnits = 3, .speedMult = 1.30f, .hpMult = 1.0f, .dmgMult = 1.0f },
            { .minUnits = 4, .speedMult = 1.50f, .hpMult = 1.0f, .dmgMult = 1.0f },
        }
    },
};
#define SYNERGY_COUNT (sizeof(SYNERGY_DEFS) / sizeof(SYNERGY_DEFS[0]))
