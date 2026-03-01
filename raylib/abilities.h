#pragma once
#include "raylib.h"

//------------------------------------------------------------------------------------
// Ability System
//------------------------------------------------------------------------------------
#define MAX_ABILITIES_PER_UNIT 4
#define ABILITY_MAX_LEVELS 5
#define ABILITY_MAX_VALUES 10

typedef enum {
    ABILITY_MAGIC_MISSILE = 0,
    ABILITY_DIG,
    ABILITY_VACUUM,
    ABILITY_CHAIN_FROST,
    ABILITY_BLOOD_RAGE,
    ABILITY_EARTHQUAKE,
    ABILITY_SPELL_PROTECT,
    ABILITY_CRAGGY_ARMOR,
    ABILITY_STONE_GAZE,
    ABILITY_SUNDER,
    ABILITY_FISSURE,
    ABILITY_VLAD_AURA,
    ABILITY_MAELSTROM,
    ABILITY_SWAP,
    ABILITY_APHOTIC_SHIELD,
    ABILITY_HOOK,
    ABILITY_PRIMAL_CHARGE,
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
    MOD_SPELL_PROTECT,
    MOD_CRAGGY_ARMOR,    // value = stun chance (0-1), stun dur stored separately
    MOD_STONE_GAZE,      // value = gaze threshold (seconds to stun)
    MOD_SHIELD,          // value = shield HP remaining
    MOD_MAELSTROM,       // value = proc chance (0-1)
    MOD_VLAD_AURA,       // value = lifesteal % granted to allies
    MOD_CHARGING,        // value = charge speed
} ModifierType;

typedef enum {
    PROJ_MAGIC_MISSILE = 0,
    PROJ_CHAIN_FROST,
    PROJ_HOOK,
    PROJ_MAELSTROM,
    PROJ_DEVIL_BOLT,
} ProjectileType;

// Named value indices into AbilityDef.values[level][]
// -- Magic Missile
#define AV_MM_DAMAGE       0
#define AV_MM_STUN_DUR     1
#define AV_MM_PROJ_SPEED   2
// -- Dig
#define AV_DIG_HP_THRESH   0
#define AV_DIG_HEAL_DUR    1
// -- Vacuum
#define AV_VAC_RADIUS      0
#define AV_VAC_STUN_DUR    1
#define AV_VAC_PULL_DUR    2
// -- Chain Frost
#define AV_CF_DAMAGE       0
#define AV_CF_BOUNCES      1
#define AV_CF_PROJ_SPEED   2
#define AV_CF_BOUNCE_RANGE 3
// -- Blood Rage
#define AV_BR_LIFESTEAL    0
#define AV_BR_DURATION     1
// -- Earthquake
#define AV_EQ_DAMAGE       0
#define AV_EQ_RADIUS       1
// -- Spell Protect
#define AV_SP_DURATION     0
// -- Craggy Armor
#define AV_CA_ARMOR        0
#define AV_CA_STUN_CHANCE  1
#define AV_CA_STUN_DUR     2
#define AV_CA_DURATION     3
// -- Stone Gaze
#define AV_SG_GAZE_THRESH  0
#define AV_SG_STUN_DUR     1
#define AV_SG_DURATION     2
#define AV_SG_CONE_ANGLE   3
// -- Sunder
#define AV_SU_HP_THRESH    0
// -- Fissure
#define AV_FI_LENGTH       0
#define AV_FI_WIDTH        1
#define AV_FI_DURATION     2
#define AV_FI_DAMAGE       3
#define AV_FI_RANGE        4
// -- Vlad's Aura
#define AV_VA_LIFESTEAL    0  // % lifesteal for allies
#define AV_VA_DURATION     1
#define AV_VA_RADIUS       2  // aura radius (0 = global)
// -- Maelstrom
#define AV_ML_PROC_CHANCE  0
#define AV_ML_DAMAGE       1
#define AV_ML_BOUNCES      2
#define AV_ML_SPEED        3
#define AV_ML_DURATION     4
#define AV_ML_BOUNCE_RANGE 5
// -- Swap Me
#define AV_SW_SHIELD       0  // flat shield HP
#define AV_SW_SHIELD_DUR   1
// -- Aphotic Shield
#define AV_AS_SHIELD       0
#define AV_AS_DURATION     1
// -- Hook
#define AV_HK_DMG_PER_DIST 0  // damage per unit of distance
#define AV_HK_SPEED        1
#define AV_HK_RANGE        2
// -- Primal Charge
#define AV_PC_DAMAGE       0
#define AV_PC_KNOCKBACK    1  // push distance
#define AV_PC_AOE_RADIUS   2
#define AV_PC_CHARGE_SPEED 3

typedef struct {
    const char *name;
    const char *description;
    const char *abbrev;
    Color       color;
    AbilityTargetType targetType;
    bool        isPassive;
    int         goldCost;
    float       range[ABILITY_MAX_LEVELS];
    float       cooldown[ABILITY_MAX_LEVELS];
    float       values[ABILITY_MAX_LEVELS][ABILITY_MAX_VALUES];
} AbilityDef;

static const AbilityDef ABILITY_DEFS[ABILITY_COUNT] = {
    [ABILITY_MAGIC_MISSILE] = {
        .name = "Magic Missile", .description = "Ranged stun projectile",
        .abbrev = "MM", .color = { 120, 80, 255, 255 },
        .targetType = TARGET_CLOSEST_ENEMY, .isPassive = false, .goldCost = 3,
        .range    = { 50.0f, 58.0f, 66.0f, 76.0f, 88.0f },
        .cooldown = { 7.0f, 6.0f, 5.0f, 4.0f, 3.0f },
        .values = {
            { [AV_MM_DAMAGE]=0.30f, [AV_MM_STUN_DUR]=1.5f, [AV_MM_PROJ_SPEED]=60.0f },
            { [AV_MM_DAMAGE]=0.40f, [AV_MM_STUN_DUR]=1.75f,[AV_MM_PROJ_SPEED]=60.0f },
            { [AV_MM_DAMAGE]=0.50f, [AV_MM_STUN_DUR]=2.0f, [AV_MM_PROJ_SPEED]=60.0f },
            { [AV_MM_DAMAGE]=0.65f, [AV_MM_STUN_DUR]=2.5f, [AV_MM_PROJ_SPEED]=60.0f },
            { [AV_MM_DAMAGE]=0.85f, [AV_MM_STUN_DUR]=3.0f, [AV_MM_PROJ_SPEED]=60.0f },
        },
    },
    [ABILITY_DIG] = {
        .name = "Dig", .description = "Invuln + heal at low HP",
        .abbrev = "DG", .color = { 160, 120, 60, 255 },
        .targetType = TARGET_NONE, .isPassive = true, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 22.0f, 18.0f, 14.0f, 11.0f, 8.0f },
        .values = {
            { [AV_DIG_HP_THRESH]=0.25f, [AV_DIG_HEAL_DUR]=4.0f },
            { [AV_DIG_HP_THRESH]=0.25f, [AV_DIG_HEAL_DUR]=3.5f },
            { [AV_DIG_HP_THRESH]=0.25f, [AV_DIG_HEAL_DUR]=3.0f },
            { [AV_DIG_HP_THRESH]=0.30f, [AV_DIG_HEAL_DUR]=2.5f },
            { [AV_DIG_HP_THRESH]=0.35f, [AV_DIG_HEAL_DUR]=2.0f },
        },
    },
    [ABILITY_VACUUM] = {
        .name = "Vacuum", .description = "Pull + stun enemies in AoE",
        .abbrev = "VC", .color = { 60, 180, 180, 255 },
        .targetType = TARGET_SELF_AOE, .isPassive = false, .goldCost = 5,
        .range    = { 40.0f, 48.0f, 56.0f, 66.0f, 78.0f },
        .cooldown = { 16.0f, 13.0f, 10.0f, 8.0f, 6.0f },
        .values = {
            { [AV_VAC_RADIUS]=30.0f, [AV_VAC_STUN_DUR]=1.0f, [AV_VAC_PULL_DUR]=0.5f },
            { [AV_VAC_RADIUS]=38.0f, [AV_VAC_STUN_DUR]=1.5f, [AV_VAC_PULL_DUR]=0.5f },
            { [AV_VAC_RADIUS]=46.0f, [AV_VAC_STUN_DUR]=2.0f, [AV_VAC_PULL_DUR]=0.5f },
            { [AV_VAC_RADIUS]=56.0f, [AV_VAC_STUN_DUR]=2.75f,[AV_VAC_PULL_DUR]=0.5f },
            { [AV_VAC_RADIUS]=68.0f, [AV_VAC_STUN_DUR]=3.5f, [AV_VAC_PULL_DUR]=0.5f },
        },
    },
    [ABILITY_CHAIN_FROST] = {
        .name = "Chain Frost", .description = "Bouncing damage projectile",
        .abbrev = "CF", .color = { 80, 140, 255, 255 },
        .targetType = TARGET_CLOSEST_ENEMY, .isPassive = false, .goldCost = 5,
        .range    = { 50.0f, 58.0f, 66.0f, 78.0f, 92.0f },
        .cooldown = { 14.0f, 12.0f, 10.0f, 8.0f, 6.0f },
        .values = {
            { [AV_CF_DAMAGE]=100.0f, [AV_CF_BOUNCES]=5.0f,  [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=40.0f },
            { [AV_CF_DAMAGE]=150.0f, [AV_CF_BOUNCES]=7.0f,  [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=40.0f },
            { [AV_CF_DAMAGE]=200.0f, [AV_CF_BOUNCES]=10.0f, [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=40.0f },
            { [AV_CF_DAMAGE]=300.0f, [AV_CF_BOUNCES]=14.0f, [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=45.0f },
            { [AV_CF_DAMAGE]=450.0f, [AV_CF_BOUNCES]=20.0f, [AV_CF_PROJ_SPEED]=50.0f, [AV_CF_BOUNCE_RANGE]=50.0f },
        },
    },
    [ABILITY_BLOOD_RAGE] = {
        .name = "Blood Rage", .description = "Grants lifesteal on attacks",
        .abbrev = "BR", .color = { 220, 40, 40, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 3,
        .range    = { 0 },
        .cooldown = { 12.0f, 10.0f, 8.0f, 6.0f, 4.0f },
        .values = {
            { [AV_BR_LIFESTEAL]=0.20f, [AV_BR_DURATION]=5.0f },
            { [AV_BR_LIFESTEAL]=0.35f, [AV_BR_DURATION]=6.0f },
            { [AV_BR_LIFESTEAL]=0.50f, [AV_BR_DURATION]=7.0f },
            { [AV_BR_LIFESTEAL]=0.70f, [AV_BR_DURATION]=8.0f },
            { [AV_BR_LIFESTEAL]=1.00f, [AV_BR_DURATION]=10.0f },
        },
    },
    [ABILITY_EARTHQUAKE] = {
        .name = "Earthquake", .description = "AoE damage (hits allies!)",
        .abbrev = "EQ", .color = { 180, 120, 40, 255 },
        .targetType = TARGET_SELF_AOE, .isPassive = false, .goldCost = 5,
        .range    = { 30.0f, 38.0f, 46.0f, 56.0f, 70.0f },
        .cooldown = { 14.0f, 12.0f, 10.0f, 8.0f, 6.0f },
        .values = {
            { [AV_EQ_DAMAGE]=3.0f,  [AV_EQ_RADIUS]=25.0f },
            { [AV_EQ_DAMAGE]=5.0f,  [AV_EQ_RADIUS]=32.0f },
            { [AV_EQ_DAMAGE]=7.0f,  [AV_EQ_RADIUS]=40.0f },
            { [AV_EQ_DAMAGE]=10.0f, [AV_EQ_RADIUS]=50.0f },
            { [AV_EQ_DAMAGE]=15.0f, [AV_EQ_RADIUS]=65.0f },
        },
    },
    [ABILITY_SPELL_PROTECT] = {
        .name = "Spell Protect", .description = "Blocks stuns & debuffs",
        .abbrev = "SP", .color = { 200, 240, 255, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 14.0f, 12.0f, 10.0f, 7.0f, 5.0f },
        .values = {
            { [AV_SP_DURATION]=4.0f },
            { [AV_SP_DURATION]=5.0f },
            { [AV_SP_DURATION]=6.0f },
            { [AV_SP_DURATION]=8.0f },
            { [AV_SP_DURATION]=12.0f },
        },
    },
    [ABILITY_CRAGGY_ARMOR] = {
        .name = "Craggy Armor", .description = "Armor + stun attackers",
        .abbrev = "CA", .color = { 140, 140, 160, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 14.0f, 12.0f, 10.0f, 8.0f, 6.0f },
        .values = {
            { [AV_CA_ARMOR]=1.0f, [AV_CA_STUN_CHANCE]=0.15f, [AV_CA_STUN_DUR]=0.8f, [AV_CA_DURATION]=6.0f },
            { [AV_CA_ARMOR]=1.5f, [AV_CA_STUN_CHANCE]=0.25f, [AV_CA_STUN_DUR]=1.0f, [AV_CA_DURATION]=7.0f },
            { [AV_CA_ARMOR]=2.0f, [AV_CA_STUN_CHANCE]=0.35f, [AV_CA_STUN_DUR]=1.2f, [AV_CA_DURATION]=8.0f },
            { [AV_CA_ARMOR]=3.0f, [AV_CA_STUN_CHANCE]=0.50f, [AV_CA_STUN_DUR]=1.5f, [AV_CA_DURATION]=10.0f },
            { [AV_CA_ARMOR]=4.0f, [AV_CA_STUN_CHANCE]=0.70f, [AV_CA_STUN_DUR]=2.0f, [AV_CA_DURATION]=12.0f },
        },
    },
    [ABILITY_STONE_GAZE] = {
        .name = "Stone Gaze", .description = "Stuns enemies facing you",
        .abbrev = "SG", .color = { 160, 80, 200, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 5,
        .range    = { 0 },
        .cooldown = { 16.0f, 14.0f, 12.0f, 9.0f, 6.0f },
        .values = {
            { [AV_SG_GAZE_THRESH]=2.0f, [AV_SG_STUN_DUR]=1.5f, [AV_SG_DURATION]=6.0f, [AV_SG_CONE_ANGLE]=45.0f },
            { [AV_SG_GAZE_THRESH]=1.6f, [AV_SG_STUN_DUR]=2.0f, [AV_SG_DURATION]=7.0f, [AV_SG_CONE_ANGLE]=45.0f },
            { [AV_SG_GAZE_THRESH]=1.2f, [AV_SG_STUN_DUR]=2.5f, [AV_SG_DURATION]=8.0f, [AV_SG_CONE_ANGLE]=45.0f },
            { [AV_SG_GAZE_THRESH]=0.8f, [AV_SG_STUN_DUR]=3.5f, [AV_SG_DURATION]=10.0f,[AV_SG_CONE_ANGLE]=60.0f },
            { [AV_SG_GAZE_THRESH]=0.5f, [AV_SG_STUN_DUR]=5.0f, [AV_SG_DURATION]=12.0f,[AV_SG_CONE_ANGLE]=90.0f },
        },
    },
    [ABILITY_SUNDER] = {
        .name = "Sunder", .description = "Swap HP with ally at low HP",
        .abbrev = "SU", .color = { 180, 40, 80, 255 },
        .targetType = TARGET_NONE, .isPassive = true, .goldCost = 3,
        .range    = { 0 },
        .cooldown = { 22.0f, 18.0f, 14.0f, 10.0f, 7.0f },
        .values = {
            { [AV_SU_HP_THRESH]=0.25f },
            { [AV_SU_HP_THRESH]=0.30f },
            { [AV_SU_HP_THRESH]=0.35f },
            { [AV_SU_HP_THRESH]=0.45f },
            { [AV_SU_HP_THRESH]=0.60f },
        },
    },
    [ABILITY_FISSURE] = {
        .name = "Fissure", .description = "Impassable terrain + damage",
        .abbrev = "FI", .color = { 120, 110, 100, 255 },
        .targetType = TARGET_CLOSEST_ENEMY, .isPassive = false, .goldCost = 5,
        .range    = { 70.0f, 80.0f, 90.0f, 105.0f, 120.0f },
        .cooldown = { 14.0f, 12.0f, 10.0f, 7.0f, 5.0f },
        .values = {
            { [AV_FI_LENGTH]=45.0f, [AV_FI_WIDTH]=8.0f,  [AV_FI_DURATION]=4.0f,  [AV_FI_DAMAGE]=2.0f,  [AV_FI_RANGE]=70.0f },
            { [AV_FI_LENGTH]=55.0f, [AV_FI_WIDTH]=8.0f,  [AV_FI_DURATION]=5.0f,  [AV_FI_DAMAGE]=3.5f,  [AV_FI_RANGE]=80.0f },
            { [AV_FI_LENGTH]=65.0f, [AV_FI_WIDTH]=8.0f,  [AV_FI_DURATION]=6.0f,  [AV_FI_DAMAGE]=5.0f,  [AV_FI_RANGE]=90.0f },
            { [AV_FI_LENGTH]=80.0f, [AV_FI_WIDTH]=10.0f, [AV_FI_DURATION]=8.0f,  [AV_FI_DAMAGE]=8.0f,  [AV_FI_RANGE]=105.0f },
            { [AV_FI_LENGTH]=100.0f,[AV_FI_WIDTH]=12.0f, [AV_FI_DURATION]=10.0f, [AV_FI_DAMAGE]=12.0f, [AV_FI_RANGE]=120.0f },
        },
    },
    [ABILITY_VLAD_AURA] = {
        .name = "Vlad's Aura", .description = "Grants lifesteal to allies",
        .abbrev = "VA", .color = { 180, 30, 30, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 14.0f, 12.0f, 10.0f, 7.0f, 5.0f },
        .values = {
            { [AV_VA_LIFESTEAL]=0.15f, [AV_VA_DURATION]=6.0f,  [AV_VA_RADIUS]=0.0f },
            { [AV_VA_LIFESTEAL]=0.25f, [AV_VA_DURATION]=7.0f,  [AV_VA_RADIUS]=0.0f },
            { [AV_VA_LIFESTEAL]=0.35f, [AV_VA_DURATION]=8.0f,  [AV_VA_RADIUS]=0.0f },
            { [AV_VA_LIFESTEAL]=0.50f, [AV_VA_DURATION]=10.0f, [AV_VA_RADIUS]=0.0f },
            { [AV_VA_LIFESTEAL]=0.75f, [AV_VA_DURATION]=12.0f, [AV_VA_RADIUS]=0.0f },
        },
    },
    [ABILITY_MAELSTROM] = {
        .name = "Maelstrom", .description = "Attacks proc chain lightning",
        .abbrev = "ML", .color = { 255, 230, 50, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 12.0f, 10.0f, 8.0f, 6.0f, 4.0f },
        .values = {
            { [AV_ML_PROC_CHANCE]=0.25f, [AV_ML_DAMAGE]=40.0f,  [AV_ML_BOUNCES]=3.0f,  [AV_ML_SPEED]=30.0f, [AV_ML_DURATION]=7.0f,  [AV_ML_BOUNCE_RANGE]=40.0f },
            { [AV_ML_PROC_CHANCE]=0.30f, [AV_ML_DAMAGE]=60.0f,  [AV_ML_BOUNCES]=4.0f,  [AV_ML_SPEED]=30.0f, [AV_ML_DURATION]=8.0f,  [AV_ML_BOUNCE_RANGE]=40.0f },
            { [AV_ML_PROC_CHANCE]=0.40f, [AV_ML_DAMAGE]=80.0f,  [AV_ML_BOUNCES]=5.0f,  [AV_ML_SPEED]=30.0f, [AV_ML_DURATION]=9.0f,  [AV_ML_BOUNCE_RANGE]=40.0f },
            { [AV_ML_PROC_CHANCE]=0.55f, [AV_ML_DAMAGE]=120.0f, [AV_ML_BOUNCES]=7.0f,  [AV_ML_SPEED]=30.0f, [AV_ML_DURATION]=11.0f, [AV_ML_BOUNCE_RANGE]=45.0f },
            { [AV_ML_PROC_CHANCE]=0.75f, [AV_ML_DAMAGE]=180.0f, [AV_ML_BOUNCES]=10.0f, [AV_ML_SPEED]=30.0f, [AV_ML_DURATION]=13.0f, [AV_ML_BOUNCE_RANGE]=50.0f },
        },
    },
    [ABILITY_SWAP] = {
        .name = "Swap Me", .description = "Swap pos with furthest enemy + shield",
        .abbrev = "SW", .color = { 200, 100, 255, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 5,
        .range    = { 0 },
        .cooldown = { 18.0f, 15.0f, 12.0f, 9.0f, 6.0f },
        .values = {
            { [AV_SW_SHIELD]=10.0f, [AV_SW_SHIELD_DUR]=4.0f },
            { [AV_SW_SHIELD]=15.0f, [AV_SW_SHIELD_DUR]=5.0f },
            { [AV_SW_SHIELD]=20.0f, [AV_SW_SHIELD_DUR]=6.0f },
            { [AV_SW_SHIELD]=30.0f, [AV_SW_SHIELD_DUR]=8.0f },
            { [AV_SW_SHIELD]=45.0f, [AV_SW_SHIELD_DUR]=10.0f },
        },
    },
    [ABILITY_APHOTIC_SHIELD] = {
        .name = "Aphotic Shield", .description = "Shield ally + purge debuffs",
        .abbrev = "AS", .color = { 80, 160, 255, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 4,
        .range    = { 0 },
        .cooldown = { 14.0f, 12.0f, 10.0f, 7.0f, 5.0f },
        .values = {
            { [AV_AS_SHIELD]=8.0f,  [AV_AS_DURATION]=5.0f },
            { [AV_AS_SHIELD]=12.0f, [AV_AS_DURATION]=6.0f },
            { [AV_AS_SHIELD]=16.0f, [AV_AS_DURATION]=7.0f },
            { [AV_AS_SHIELD]=24.0f, [AV_AS_DURATION]=9.0f },
            { [AV_AS_SHIELD]=35.0f, [AV_AS_DURATION]=12.0f },
        },
    },
    [ABILITY_HOOK] = {
        .name = "Dendi Hook", .description = "Hook furthest enemy, dmg by distance",
        .abbrev = "HK", .color = { 200, 60, 60, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 5,
        .range    = { 0 },
        .cooldown = { 12.0f, 10.0f, 8.0f, 6.0f, 4.0f },
        .values = {
            { [AV_HK_DMG_PER_DIST]=0.08f, [AV_HK_SPEED]=45.0f, [AV_HK_RANGE]=80.0f },
            { [AV_HK_DMG_PER_DIST]=0.12f, [AV_HK_SPEED]=45.0f, [AV_HK_RANGE]=90.0f },
            { [AV_HK_DMG_PER_DIST]=0.16f, [AV_HK_SPEED]=45.0f, [AV_HK_RANGE]=100.0f },
            { [AV_HK_DMG_PER_DIST]=0.22f, [AV_HK_SPEED]=45.0f, [AV_HK_RANGE]=115.0f },
            { [AV_HK_DMG_PER_DIST]=0.30f, [AV_HK_SPEED]=45.0f, [AV_HK_RANGE]=135.0f },
        },
    },
    [ABILITY_PRIMAL_CHARGE] = {
        .name = "Primal Charge", .description = "Charge at furthest enemy, AoE impact",
        .abbrev = "PC", .color = { 255, 140, 0, 255 },
        .targetType = TARGET_NONE, .isPassive = false, .goldCost = 5,
        .range    = { 0 },
        .cooldown = { 16.0f, 14.0f, 12.0f, 9.0f, 6.0f },
        .values = {
            { [AV_PC_DAMAGE]=5.0f,  [AV_PC_KNOCKBACK]=15.0f, [AV_PC_AOE_RADIUS]=15.0f, [AV_PC_CHARGE_SPEED]=80.0f },
            { [AV_PC_DAMAGE]=8.0f,  [AV_PC_KNOCKBACK]=20.0f, [AV_PC_AOE_RADIUS]=15.0f, [AV_PC_CHARGE_SPEED]=80.0f },
            { [AV_PC_DAMAGE]=12.0f, [AV_PC_KNOCKBACK]=25.0f, [AV_PC_AOE_RADIUS]=15.0f, [AV_PC_CHARGE_SPEED]=80.0f },
            { [AV_PC_DAMAGE]=18.0f, [AV_PC_KNOCKBACK]=32.0f, [AV_PC_AOE_RADIUS]=20.0f, [AV_PC_CHARGE_SPEED]=90.0f },
            { [AV_PC_DAMAGE]=28.0f, [AV_PC_KNOCKBACK]=40.0f, [AV_PC_AOE_RADIUS]=25.0f, [AV_PC_CHARGE_SPEED]=100.0f },
        },
    },
};

// Clockwise activation order: TL(0) -> TR(1) -> BR(3) -> BL(2)
static const int ACTIVATION_ORDER[MAX_ABILITIES_PER_UNIT] = { 0, 1, 3, 2 };
