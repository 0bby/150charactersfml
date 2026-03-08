#pragma once
#include <stdbool.h>
#include "raylib.h"
#include "abilities.h"

#define MAX_ITEMS 32
#define ITEM_NONE -1

typedef enum {
    ITEM_SWIFT_BOOTS = 0,
    ITEM_IRON_SHIELD,
    ITEM_WHETSTONE,
    ITEM_CHRONO_SHARD,
    ITEM_VAMPIRIC_FANG,
    ITEM_STONE_SKIN,
    ITEM_COUNT,
} ItemId;

typedef enum {
    IEFF_HP_MULT = 0,
    IEFF_DMG_MULT,
    IEFF_SPEED_MULT,
    IEFF_MODIFIER,
} ItemEffectType;

typedef struct {
    const char *name;
    const char *description;
    int cost;
    bool enabled;
    ItemEffectType effectType;
    float effectValue;
    int modType;
    float modDuration;
    Color color;
} ItemDef;

static const ItemDef ITEM_DEFS[ITEM_COUNT] = {
    [ITEM_SWIFT_BOOTS]  = { "Swift Boots",   "+15% Speed",      8,  true, IEFF_SPEED_MULT, 1.15f, 0, 0, {80,200,220,255} },
    [ITEM_IRON_SHIELD]  = { "Iron Shield",   "+20% Max HP",    10,  true, IEFF_HP_MULT,    1.20f, 0, 0, {160,160,180,255} },
    [ITEM_WHETSTONE]    = { "Whetstone",     "+15% Attack Dmg",  8, true, IEFF_DMG_MULT,   1.15f, 0, 0, {220,120,60,255} },
    [ITEM_CHRONO_SHARD] = { "Chrono Shard",  "20% CDR",        12,  true, IEFF_MODIFIER,   0.20f, MOD_COOLDOWN_REDUCTION, 99999.0f, {180,120,255,255} },
    [ITEM_VAMPIRIC_FANG]= { "Vampiric Fang", "10% Lifesteal",  10, true, IEFF_MODIFIER,   0.10f, MOD_LIFESTEAL, 99999.0f, {200,50,50,255} },
    [ITEM_STONE_SKIN]   = { "Stone Skin",    "+3 Armor",       10,  true, IEFF_MODIFIER,   3.0f,  MOD_ARMOR, 99999.0f, {140,140,120,255} },
};
