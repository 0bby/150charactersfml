#include "events.h"
#include "unit_stats.h"
#include "raylib.h"

const EventDef EVENT_DEFS[] = {
    { "Mysterious Fountain",
      "A glowing spring bubbles with restorative energy.",
      2, {
        { "Heal all to full HP",    EVFX_HEAL_FULL, 0, 0 },
        { "Gain 10 gold",           EVFX_GOLD, 10, 0 },
    }},
    { "Wandering Merchant",
      "A cloaked figure offers rare goods.",
      2, {
        { "Buy random ability (5g)", EVFX_RANDOM_ABILITY, 0, 5 },
        { "Skip",                    EVFX_NONE, 0, 0 },
    }},
    { "Training Grounds",
      "An ancient arena pulses with power.",
      2, {
        { "All units +10% HP",      EVFX_HP_MULT, 110, 0 },
        { "All units +10% DMG",     EVFX_DMG_MULT, 110, 0 },
    }},
    { "Cursed Altar",
      "Dark energy radiates from the stone.",
      2, {
        { "Sacrifice 20% HP, gain 15g", EVFX_SACRIFICE_HP_GOLD, 15, 0 },
        { "Walk away",                   EVFX_NONE, 0, 0 },
    }},
    { "Ancient Tome",
      "A dusty book hums with magical knowledge.",
      2, {
        { "Learn random ability",   EVFX_RANDOM_ABILITY, 0, 0 },
        { "Leave it alone",         EVFX_NONE, 0, 0 },
    }},
    { "Bazaar of Wonders",
      "A bustling market appears from thin air.",
      3, {
        { "Extra shop slot (8g)",    EVFX_ADD_SHOP_SLOT, 1, 8 },
        { "Gain 12 gold",           EVFX_GOLD, 12, 0 },
        { "Skip",                    EVFX_NONE, 0, 0 },
    }},
    { "Shady Dealer",
      "\"I'll take that off your hands...\"",
      2, {
        { "Remove an ability, gain 8g",  EVFX_REMOVE_ABILITY, 8, 0 },
        { "Decline",                     EVFX_NONE, 0, 0 },
    }},
    { "Enchanted Forge",
      "The forge glows with arcane runes.",
      2, {
        { "Level up random ability (6g)",  EVFX_LEVEL_UP_RANDOM, 1, 6 },
        { "Level up chosen ability (10g)", EVFX_LEVEL_UP_CHOOSE, 1, 10 },
    }},
    { "Witch's Cauldron",
      "A bubbling brew promises great power... at a cost.",
      2, {
        { "Random ability + reroll (2g each)", EVFX_REROLL_ABILITY, 2, 0 },
        { "Walk away",                          EVFX_NONE, 0, 0 },
    }},
    { "Crown of Thorns",
      "A thorny crown radiates an unsettling aura.",
      2, {
        { "Upgrade random unit rarity",  EVFX_RARITY_UP, 0, 0 },
        { "Downgrade random, gain 12g",  EVFX_RARITY_DOWN, 12, 0 },
    }},
    { "Narrow Market",
      "The stalls are closing. \"Last chance!\"",
      2, {
        { "Lose a shop slot, gain 15g",  EVFX_REMOVE_SHOP_SLOT, 15, 0 },
        { "Decline",                     EVFX_NONE, 0, 0 },
    }},
};

const int EVENT_DEF_COUNT = sizeof(EVENT_DEFS) / sizeof(EVENT_DEFS[0]);

// Simple seeded RNG for events
static uint32_t evtRng;
static void evt_rng_seed(uint32_t s) { evtRng = s ? s : 1; }
static uint32_t evt_rng_next(void) {
    evtRng ^= evtRng << 13;
    evtRng ^= evtRng >> 17;
    evtRng ^= evtRng << 5;
    return evtRng;
}

int GetRandomEventIndex(uint32_t seed) {
    evt_rng_seed(seed);
    return (int)(evt_rng_next() % (uint32_t)EVENT_DEF_COUNT);
}

bool ApplyEventEffect(EventEffectType effect, int value, int cost,
                      Unit units[], int unitCount, int *gold,
                      bool *needsPicker, int *pickerType) {
    *needsPicker = false;
    *pickerType = 0;

    // Check gold cost
    if (cost > 0 && *gold < cost)
        return false;
    if (cost > 0)
        *gold -= cost;

    switch (effect) {
    case EVFX_HEAL_FULL:
        for (int i = 0; i < unitCount; i++) {
            if (units[i].active && units[i].team == TEAM_BLUE) {
                float maxHP = UNIT_STATS[units[i].typeIndex].health *
                              units[i].hpMultiplier;
                units[i].currentHealth = maxHP;
            }
        }
        break;

    case EVFX_GOLD:
        *gold += value;
        break;

    case EVFX_RANDOM_ABILITY:
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE)
                continue;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                if (units[i].abilities[a].abilityId == -1) {
                    units[i].abilities[a].abilityId =
                        GetRandomValue(0, ABILITY_COUNT - 1);
                    units[i].abilities[a].level = 0;
                    return true;
                }
            }
        }
        break;

    case EVFX_REMOVE_ABILITY:
        *needsPicker = true;
        *pickerType = 0; // ability remove picker
        *gold += value; // gain gold for removing
        break;

    case EVFX_REROLL_ABILITY:
        *needsPicker = true;
        *pickerType = 1; // reroll picker
        break;

    case EVFX_LEVEL_UP_RANDOM: {
        // Find a random ability across blue units and level it up
        int candidates[MAX_UNITS * MAX_ABILITIES_PER_UNIT];
        int candidateUnits[MAX_UNITS * MAX_ABILITIES_PER_UNIT];
        int count = 0;
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE) continue;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                if (units[i].abilities[a].abilityId >= 0 &&
                    units[i].abilities[a].level < ABILITY_MAX_LEVELS - 1) {
                    candidateUnits[count] = i;
                    candidates[count] = a;
                    count++;
                }
            }
        }
        if (count > 0) {
            int pick = GetRandomValue(0, count - 1);
            units[candidateUnits[pick]].abilities[candidates[pick]].level++;
        }
        break;
    }

    case EVFX_LEVEL_UP_CHOOSE:
        *needsPicker = true;
        *pickerType = 2; // ability level up picker
        break;

    case EVFX_LEVEL_DOWN_RANDOM: {
        int candidates[MAX_UNITS * MAX_ABILITIES_PER_UNIT];
        int candidateUnits[MAX_UNITS * MAX_ABILITIES_PER_UNIT];
        int count = 0;
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE) continue;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                if (units[i].abilities[a].abilityId >= 0 &&
                    units[i].abilities[a].level > 0) {
                    candidateUnits[count] = i;
                    candidates[count] = a;
                    count++;
                }
            }
        }
        if (count > 0) {
            int pick = GetRandomValue(0, count - 1);
            units[candidateUnits[pick]].abilities[candidates[pick]].level--;
        }
        break;
    }

    case EVFX_ADD_SHOP_SLOT:
        // Handled in main.c (needs access to shopSlotCount)
        break;

    case EVFX_REMOVE_SHOP_SLOT:
        *gold += value;
        // Handled in main.c (needs access to shopSlotCount)
        break;

    case EVFX_RARITY_UP:
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE) continue;
            if (units[i].rarity < RARITY_LEGENDARY) {
                units[i].rarity++;
                // Recalc multipliers
                if (units[i].rarity == RARITY_RARE) {
                    units[i].hpMultiplier *= RARITY_MULT_RARE;
                    units[i].dmgMultiplier *= RARITY_MULT_RARE;
                } else if (units[i].rarity == RARITY_LEGENDARY) {
                    units[i].hpMultiplier *= RARITY_MULT_LEGENDARY / RARITY_MULT_RARE;
                    units[i].dmgMultiplier *= RARITY_MULT_LEGENDARY / RARITY_MULT_RARE;
                }
                float maxHP = UNIT_STATS[units[i].typeIndex].health * units[i].hpMultiplier;
                units[i].currentHealth = maxHP;
                break;
            }
        }
        break;

    case EVFX_RARITY_DOWN:
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE) continue;
            if (units[i].rarity > RARITY_COMMON) {
                if (units[i].rarity == RARITY_LEGENDARY) {
                    units[i].hpMultiplier /= RARITY_MULT_LEGENDARY / RARITY_MULT_RARE;
                    units[i].dmgMultiplier /= RARITY_MULT_LEGENDARY / RARITY_MULT_RARE;
                } else if (units[i].rarity == RARITY_RARE) {
                    units[i].hpMultiplier /= RARITY_MULT_RARE;
                    units[i].dmgMultiplier /= RARITY_MULT_RARE;
                }
                units[i].rarity--;
                float maxHP = UNIT_STATS[units[i].typeIndex].health * units[i].hpMultiplier;
                units[i].currentHealth = maxHP;
                *gold += value;
                break;
            }
        }
        break;

    case EVFX_HP_MULT: {
        float mult = (float)value / 100.0f;
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE) continue;
            units[i].hpMultiplier *= mult;
            float maxHP = UNIT_STATS[units[i].typeIndex].health * units[i].hpMultiplier;
            units[i].currentHealth = maxHP;
        }
        break;
    }

    case EVFX_DMG_MULT: {
        float mult = (float)value / 100.0f;
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].team != TEAM_BLUE) continue;
            units[i].dmgMultiplier *= mult;
        }
        break;
    }

    case EVFX_SACRIFICE_HP_GOLD:
        for (int i = 0; i < unitCount; i++) {
            if (units[i].active && units[i].team == TEAM_BLUE)
                units[i].currentHealth *= 0.8f;
        }
        *gold += value;
        break;

    case EVFX_NONE:
        break;
    }
    return true;
}
