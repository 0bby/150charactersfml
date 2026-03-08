#pragma once
#include "game.h"
#include "helpers.h"
#include <stdint.h>

// Seeded PRNG for deterministic combat
void combat_rng_seed(uint32_t seed);
float combat_rng_next(void);

// Deterministic combat tick — no rendering, no random calls.
// Returns: 0 = still fighting, 1 = blue wins, 2 = red wins, 3 = draw
// events[] is filled with visual feedback events (can be NULL for headless).
int CombatTick(Unit units[], int *unitCount,
               Modifier modifiers[],
               Projectile projectiles[],
               Fissure fissures[],
               float dt,
               CombatEvent events[], int *eventCount);
