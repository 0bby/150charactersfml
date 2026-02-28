#pragma once
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

static const UnitStats UNIT_STATS[] = {
    /* 0  Mushroom */ { .health = 40.0f, .movementSpeed = 12.0f, .attackDamage = 3.0f, .attackSpeed = 1.2f },
    /* 1  Goblin   */ { .health = 20.0f, .movementSpeed = 20.0f, .attackDamage = 2.0f, .attackSpeed = 0.5f },
};
