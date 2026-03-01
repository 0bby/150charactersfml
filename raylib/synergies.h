#pragma once

#include <stddef.h>

#ifndef SERVER_BUILD
#include "raylib.h"
#endif

//------------------------------------------------------------------------------------
// Synergy Definitions — data-driven synergy table
// Add new synergies by appending to SYNERGY_DEFS[].
//------------------------------------------------------------------------------------

typedef struct {
    const char *name;
    const char *abbrev;          // short label for badge pills (e.g. "GS")
#ifndef SERVER_BUILD
    Color color;                 // synergy theme color
#endif
    const char *buffDesc[4];     // human-readable buff per tier
    int requiredTypes[4];        // unit type indices that count toward this synergy
    int requiredTypeCount;
    int targetType;              // which type gets the buff (-1 = all matching)
    bool requireAllTypes;        // true = count distinct types present (for multi-type synergies)
    int tierCount;
    struct {
        int minUnits;
        float speedMult;     // 1.0 = no change
        float hpMult;
        float dmgMult;
    } tiers[4];
} SynergyDef;

static const SynergyDef SYNERGY_DEFS[] = {
    // 0: Goblin Swarm
    {
        .name = "Goblin Swarm",
        .abbrev = "GS",
#ifndef SERVER_BUILD
        .color = { 60, 180, 60, 255 },
#endif
        .buffDesc = { "+15% SPD", "+30% SPD", "+50% SPD", NULL },
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
    // 1: Mushroom Fort
    {
        .name = "Mushroom Fort",
        .abbrev = "MF",
#ifndef SERVER_BUILD
        .color = { 180, 100, 60, 255 },
#endif
        .buffDesc = { "+20% HP", "+40% HP", "+60% HP, +10% DMG", NULL },
        .requiredTypes = { 0 },       // Mushroom
        .requiredTypeCount = 1,
        .targetType = 0,              // buff goes to mushrooms
        .tierCount = 3,
        .tiers = {
            { .minUnits = 2, .speedMult = 1.0f, .hpMult = 1.20f, .dmgMult = 1.0f },
            { .minUnits = 3, .speedMult = 1.0f, .hpMult = 1.40f, .dmgMult = 1.0f },
            { .minUnits = 4, .speedMult = 1.0f, .hpMult = 1.60f, .dmgMult = 1.10f },
        }
    },
    // 2: Reptile Fury
    {
        .name = "Reptile Fury",
        .abbrev = "RF",
#ifndef SERVER_BUILD
        .color = { 100, 60, 180, 255 },
#endif
        .buffDesc = { "+20% DMG", "+40% DMG, +10% SPD", NULL, NULL },
        .requiredTypes = { 5 },       // Reptile
        .requiredTypeCount = 1,
        .targetType = 5,              // buff goes to reptiles
        .tierCount = 2,
        .tiers = {
            { .minUnits = 2, .speedMult = 1.0f,  .hpMult = 1.0f, .dmgMult = 1.20f },
            { .minUnits = 3, .speedMult = 1.10f, .hpMult = 1.0f, .dmgMult = 1.40f },
        }
    },
    // 3: Wild Alliance
    {
        .name = "Wild Alliance",
        .abbrev = "WA",
#ifndef SERVER_BUILD
        .color = { 200, 180, 60, 255 },
#endif
        .buffDesc = { "+10% DMG, +5% SPD", NULL, NULL, NULL },
        .requiredTypes = { 0, 1, 5 }, // Mushroom, Goblin, Reptile
        .requiredTypeCount = 3,
        .targetType = -1,             // buff goes to all matching types
        .requireAllTypes = true,      // need 1 of each type, not just 3 total
        .tierCount = 1,
        .tiers = {
            { .minUnits = 3, .speedMult = 1.05f, .hpMult = 1.0f, .dmgMult = 1.10f },
        }
    },
    // 4: Devil Pact
    {
        .name = "Devil Pact",
        .abbrev = "DP",
#ifndef SERVER_BUILD
        .color = { 200, 50, 50, 255 },
#endif
        .buffDesc = { "+15% DMG, +10% HP", "+30% DMG, +20% HP", NULL, NULL },
        .requiredTypes = { 2 },       // Devil
        .requiredTypeCount = 1,
        .targetType = 2,              // buff goes to devils
        .tierCount = 2,
        .tiers = {
            { .minUnits = 2, .speedMult = 1.0f, .hpMult = 1.10f, .dmgMult = 1.15f },
            { .minUnits = 3, .speedMult = 1.0f, .hpMult = 1.20f, .dmgMult = 1.30f },
        }
    },
};
#define SYNERGY_COUNT (sizeof(SYNERGY_DEFS) / sizeof(SYNERGY_DEFS[0]))
