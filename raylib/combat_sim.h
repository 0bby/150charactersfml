#pragma once
#include "game.h"
#include "helpers.h"

// Deterministic combat tick — no rendering, no random calls.
// Returns: 0 = still fighting, 1 = blue wins, 2 = red wins, 3 = draw
// events[] is filled with visual feedback events (can be NULL for headless).
int CombatTick(Unit units[], int unitCount,
               Modifier modifiers[],
               Projectile projectiles[],
               Fissure fissures[],
               float dt,
               CombatEvent events[], int *eventCount);
