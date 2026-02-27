#pragma once
#include "raylib.h"

//------------------------------------------------------------------------------------
// Ability System
//------------------------------------------------------------------------------------
#define MAX_ABILITIES_PER_UNIT 4
#define ABILITY_MAX_LEVELS 3
#define ABILITY_MAX_VALUES 10

typedef enum {
    ABILITY_MAGIC_MISSILE = 0,
    ABILITY_DIG,
    ABILITY_VACUUM,
    ABILITY_CHAIN_FROST,
    ABILITY_BLOOD_RAGE,
    ABILITY_COUNT,
} AbilityId;

typedef enum {
    TARGET_NONE = 0,          // passive / self-cast
    TARGET_CLOSEST_ENEMY,     // auto-targets closest enemy
    TARGET_SELF_AOE,          // AoE centered on caster
} AbilityTargetType;

typedef enum {
    MOD_STUN = 0,
    MOD_INVULNERABLE,
    MOD_LIFESTEAL,
    MOD_SPEED_MULT,
    MOD_ARMOR,
    MOD_DIG_HEAL,
} ModifierType;

typedef enum {
    PROJ_MAGIC_MISSILE = 0,
    PROJ_CHAIN_FROST,
} ProjectileType;

// Named value indices into AbilityDef.values[level][]
#define AV_MM_DAMAGE      0
#define AV_MM_STUN_DUR    1
#define AV_MM_PROJ_SPEED  2
#define AV_DIG_HP_THRESH  0
#define AV_DIG_HEAL_DUR   1
#define AV_VAC_RADIUS     0
#define AV_VAC_STUN_DUR   1
#define AV_VAC_PULL_DUR   2
#define AV_CF_DAMAGE      0
#define AV_CF_BOUNCES     1
#define AV_CF_PROJ_SPEED  2
#define AV_CF_BOUNCE_RANGE 3
#define AV_BR_LIFESTEAL   0
#define AV_BR_DURATION    1

typedef struct {
    const char *name;
    const char *description;
    AbilityTargetType targetType;
    int goldCost;
    float range[ABILITY_MAX_LEVELS];
    float cooldown[ABILITY_MAX_LEVELS];
    float values[ABILITY_MAX_LEVELS][ABILITY_MAX_VALUES];
} AbilityDef;

static const AbilityDef ABILITY_DEFS[ABILITY_COUNT] = {
    [ABILITY_MAGIC_MISSILE] = {
        .name = "Magic Missile", .description = "Ranged stun projectile",
        .targetType = TARGET_CLOSEST_ENEMY, .goldCost = 3,
        .range    = { 50.0f, 58.0f, 66.0f },
        .cooldown = { 10.0f, 9.0f, 8.0f },
        .values = {
            { [AV_MM_DAMAGE]=0.30f, [AV_MM_STUN_DUR]=1.5f, [AV_MM_PROJ_SPEED]=60.0f },
            { [AV_MM_DAMAGE]=0.40f, [AV_MM_STUN_DUR]=1.75f,[AV_MM_PROJ_SPEED]=60.0f },
            { [AV_MM_DAMAGE]=0.50f, [AV_MM_STUN_DUR]=2.0f, [AV_MM_PROJ_SPEED]=60.0f },
        },
    },
    [ABILITY_DIG] = {
        .name = "Dig", .description = "Invuln + heal at low HP",
        .targetType = TARGET_NONE, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 30.0f, 25.0f, 20.0f },
        .values = {
            { [AV_DIG_HP_THRESH]=0.25f, [AV_DIG_HEAL_DUR]=4.0f },
            { [AV_DIG_HP_THRESH]=0.25f, [AV_DIG_HEAL_DUR]=3.5f },
            { [AV_DIG_HP_THRESH]=0.25f, [AV_DIG_HEAL_DUR]=3.0f },
        },
    },
    [ABILITY_VACUUM] = {
        .name = "Vacuum", .description = "Pull + stun enemies in AoE",
        .targetType = TARGET_SELF_AOE, .goldCost = 5,
        .range    = { 0 },
        .cooldown = { 22.0f, 18.0f, 14.0f },
        .values = {
            { [AV_VAC_RADIUS]=30.0f, [AV_VAC_STUN_DUR]=1.0f, [AV_VAC_PULL_DUR]=0.5f },
            { [AV_VAC_RADIUS]=38.0f, [AV_VAC_STUN_DUR]=1.5f, [AV_VAC_PULL_DUR]=0.5f },
            { [AV_VAC_RADIUS]=46.0f, [AV_VAC_STUN_DUR]=2.0f, [AV_VAC_PULL_DUR]=0.5f },
        },
    },
    [ABILITY_CHAIN_FROST] = {
        .name = "Chain Frost", .description = "Bouncing damage projectile",
        .targetType = TARGET_CLOSEST_ENEMY, .goldCost = 5,
        .range    = { 50.0f, 58.0f, 66.0f },
        .cooldown = { 20.0f, 17.0f, 14.0f },
        .values = {
            { [AV_CF_DAMAGE]=100.0f, [AV_CF_BOUNCES]=5.0f, [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=40.0f },
            { [AV_CF_DAMAGE]=150.0f, [AV_CF_BOUNCES]=7.0f, [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=40.0f },
            { [AV_CF_DAMAGE]=200.0f, [AV_CF_BOUNCES]=10.0f,[AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=40.0f },
        },
    },
    [ABILITY_BLOOD_RAGE] = {
        .name = "Blood Rage", .description = "Grants lifesteal on attacks",
        .targetType = TARGET_NONE, .goldCost = 3,
        .range    = { 0 },
        .cooldown = { 18.0f, 15.0f, 12.0f },
        .values = {
            { [AV_BR_LIFESTEAL]=0.20f, [AV_BR_DURATION]=5.0f },
            { [AV_BR_LIFESTEAL]=0.35f, [AV_BR_DURATION]=6.0f },
            { [AV_BR_LIFESTEAL]=0.50f, [AV_BR_DURATION]=7.0f },
        },
    },
};

static const Color ABILITY_COLORS[ABILITY_COUNT] = {
    { 120, 80, 255, 255 },   // Magic Missile — purple
    { 160, 120, 60, 255 },   // Dig — brown
    { 60, 180, 180, 255 },   // Vacuum — cyan
    { 80, 140, 255, 255 },   // Chain Frost — blue
    { 220, 40, 40, 255 },    // Blood Rage — red
};

static const char *ABILITY_ABBREV[ABILITY_COUNT] = { "MM", "DG", "VC", "CF", "BR" };

// Clockwise activation order: TL(0) → TR(1) → BR(3) → BL(2)
static const int ACTIVATION_ORDER[MAX_ABILITIES_PER_UNIT] = { 0, 1, 3, 2 };
