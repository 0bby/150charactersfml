#pragma once
#include <stddef.h>
#include "raylib.h"

//------------------------------------------------------------------------------------
// Unit Stats — "Master Library" for balancing.
// Change numbers here; every spawned unit picks them up automatically.
//------------------------------------------------------------------------------------
typedef struct {
    float health;
    float movementSpeed;        // world-units per second
    float attackDamage;
    float attackSpeed;          // seconds between attacks
} UnitStats;

#ifndef SERVER_BUILD
static const char *UNIT_TYPE_NAMES[] = {
    /* 0 */ "Mushroom",
    /* 1 */ "Goblin",
    /* 2 */ "Devil",
    /* 3 */ "Puppycat",
    /* 4 */ "Siren",
    /* 5 */ "Reptile",
};
#define UNIT_TYPE_NAME_COUNT (int)(sizeof(UNIT_TYPE_NAMES) / sizeof(UNIT_TYPE_NAMES[0]))
static inline const char *GetUnitTypeName(int idx) {
    if (idx < 0 || idx >= UNIT_TYPE_NAME_COUNT || !UNIT_TYPE_NAMES[idx]) return "Unknown";
    return UNIT_TYPE_NAMES[idx];
}
#endif

// Valid (non-empty) unit type indices for random selection
static const int VALID_UNIT_TYPES[] = { 0, 1, 2, 5 };
#define VALID_UNIT_TYPE_COUNT (int)(sizeof(VALID_UNIT_TYPES) / sizeof(VALID_UNIT_TYPES[0]))

static const UnitStats UNIT_STATS[] = {
    /* 0  Mushroom */ { .health = 40.0f, .movementSpeed = 12.0f, .attackDamage = 3.0f, .attackSpeed = 1.2f },
    /* 1  Goblin   */ { .health = 20.0f, .movementSpeed = 20.0f, .attackDamage = 2.0f, .attackSpeed = 0.5f },
    /* 2  Devil   */ { .health = 25.0f, .movementSpeed = 10.0f, .attackDamage = 6.0f, .attackSpeed = 1.4f },
    /* 3  Puppycat */ { .health = 35.0f, .movementSpeed = 10.0f, .attackDamage = 2.5f, .attackSpeed = 1.0f },
    /* 4  Siren    */ { .health = 18.0f, .movementSpeed = 18.0f, .attackDamage = 7.0f, .attackSpeed = 1.3f },
    /* 5  Reptile  */ { .health = 30.0f, .movementSpeed = 15.0f, .attackDamage = 5.0f, .attackSpeed = 0.9f },
};
