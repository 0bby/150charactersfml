#pragma once
#include "game.h"
#include "abilities.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_EVENT_CHOICES 3

typedef enum {
    EVFX_HEAL_FULL,          // Heal all units to full
    EVFX_GOLD,               // Add gold (value = amount)
    EVFX_RANDOM_ABILITY,     // Give random ability to random unit
    EVFX_REMOVE_ABILITY,     // Player picks ability to remove (opens picker)
    EVFX_REROLL_ABILITY,     // Random ability assigned, can reroll for gold
    EVFX_LEVEL_UP_RANDOM,    // Level up a random ability by 1
    EVFX_LEVEL_UP_CHOOSE,    // Player picks ability to level up (opens picker)
    EVFX_LEVEL_DOWN_RANDOM,  // Level down a random ability
    EVFX_ADD_SHOP_SLOT,      // +1 shop slot (permanent)
    EVFX_REMOVE_SHOP_SLOT,   // -1 shop slot (permanent)
    EVFX_RARITY_UP,          // Upgrade a random unit's rarity
    EVFX_RARITY_DOWN,        // Downgrade a random unit's rarity
    EVFX_HP_MULT,            // Multiply all units HP (value = multiplier * 100, e.g. 110 = 1.1x)
    EVFX_DMG_MULT,           // Multiply all units DMG (value = multiplier * 100)
    EVFX_SACRIFICE_HP_GOLD,  // Lose 20% HP, gain value gold
    EVFX_NONE,               // No effect (skip / walk away)
} EventEffectType;

typedef struct {
    const char *label;          // Button text
    EventEffectType effect;
    int value;                  // Meaning depends on effect type
    int cost;                   // Gold cost (0 = free)
} EventChoice;

typedef struct {
    const char *title;          // "Mysterious Fountain"
    const char *description;    // Flavor text
    int choiceCount;
    EventChoice choices[MAX_EVENT_CHOICES];
} EventDef;

extern const EventDef EVENT_DEFS[];
extern const int EVENT_DEF_COUNT;

// Get a random event index from seed
int GetRandomEventIndex(uint32_t seed);

// Apply an event effect. Returns true if needs a secondary UI (picker).
bool ApplyEventEffect(EventEffectType effect, int value, int cost,
                      Unit units[], int unitCount, int *gold,
                      bool *needsPicker, int *pickerType);
