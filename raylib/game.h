#pragma once
#include "raylib.h"
#include "unit_stats.h"
#include "abilities.h"

//------------------------------------------------------------------------------------
// Data Structures & Constants
//------------------------------------------------------------------------------------
#define MAX_UNIT_TYPES 8
#define MAX_UNITS 64
#define TOTAL_ROUNDS 5
#define ATTACK_RANGE 8.0f       // how close a unit needs to be to attack
#define BLUE_TEAM_MAX_SIZE 4   // player team cap (change this to rebalance)

//------------------------------------------------------------------------------------
// Team
//------------------------------------------------------------------------------------
typedef enum { TEAM_BLUE = 0, TEAM_RED = 1 } Team;

//------------------------------------------------------------------------------------
// Game phases
//------------------------------------------------------------------------------------
typedef enum {
    PHASE_PREP,       // place / arrange units
    PHASE_COMBAT,     // units fight automatically
    PHASE_ROUND_OVER, // brief pause showing round result
    PHASE_GAME_OVER,  // all rounds finished
} GamePhase;

typedef enum { ANIM_IDLE = 0, ANIM_WALK, ANIM_COUNT } AnimState;

#define MAX_SHOP_SLOTS 3
#define MAX_MODIFIERS 128
#define MAX_PROJECTILES 32
#define MAX_PARTICLES 256
#define MAX_INVENTORY_SLOTS 6

//------------------------------------------------------------------------------------
// Ability Slot (on units)
//------------------------------------------------------------------------------------
typedef struct {
    int abilityId;            // -1 = empty
    int level;                // 0-2 (displayed as 1-3)
    float cooldownRemaining;
    bool triggered;           // for passives like Dig
} AbilitySlot;

//------------------------------------------------------------------------------------
// Modifier
//------------------------------------------------------------------------------------
typedef struct {
    ModifierType type;
    int unitIndex;
    float duration;
    float value;
    bool active;
} Modifier;

//------------------------------------------------------------------------------------
// Projectile
//------------------------------------------------------------------------------------
typedef struct {
    ProjectileType type;
    Vector3 position;
    int targetIndex;
    int sourceIndex;
    Team sourceTeam;
    float speed;
    float damage;
    float stunDuration;
    int bouncesRemaining;
    float bounceRange;
    int lastHitUnit;
    int level;
    Color color;
    bool active;
} Projectile;

//------------------------------------------------------------------------------------
// Particle (simple visual effect)
//------------------------------------------------------------------------------------
typedef struct {
    Vector3 position;
    Vector3 velocity;
    float life;       // seconds remaining
    float maxLife;
    Color color;
    float size;
    bool active;
} Particle;

//------------------------------------------------------------------------------------
// Shop & Inventory
//------------------------------------------------------------------------------------
typedef struct {
    int abilityId;
    int level;
} ShopSlot;

typedef struct {
    int abilityId;
    int level;
} InventorySlot;

typedef struct {
    bool dragging;
    int sourceType;       // 0 = inventory, 1 = unit ability slot
    int sourceIndex;      // slot index
    int sourceUnitIndex;  // unit index (when sourceType == 1)
    int abilityId;
    int level;
} DragState;

//------------------------------------------------------------------------------------
// HUD Layout Constants
//------------------------------------------------------------------------------------
#define HUD_UNIT_BAR_HEIGHT 130
#define HUD_SHOP_HEIGHT 50
#define HUD_TOTAL_HEIGHT (HUD_UNIT_BAR_HEIGHT + HUD_SHOP_HEIGHT)
#define HUD_CARD_WIDTH 180
#define HUD_CARD_HEIGHT 120
#define HUD_CARD_SPACING 10
#define HUD_PORTRAIT_SIZE 80
#define HUD_ABILITY_SLOT_SIZE 32
#define HUD_ABILITY_SLOT_GAP 4
#define HUD_INVENTORY_COLS 3
#define HUD_INVENTORY_ROWS 2

//------------------------------------------------------------------------------------
// Unit type (visual info — model, scale, name)
//------------------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *modelPath;
    Model model;
    BoundingBox baseBounds;
    float scale;
    bool loaded;
    ModelAnimation *anims;          // walk animations (NULL if none)
    int animCount;                  // number of walk animations
    ModelAnimation *idleAnims;      // idle animations (NULL if none)
    int idleAnimCount;              // number of idle animations
    int animIndex[ANIM_COUNT];      // index into respective anim array (-1 = not found)
    bool hasAnimations;
} UnitType;

//------------------------------------------------------------------------------------
// Runtime unit instance
//------------------------------------------------------------------------------------
typedef struct {
    int typeIndex;
    Vector3 position;
    Team team;
    float currentHealth;
    float attackCooldown;
    int targetIndex;
    bool active;
    bool selected;
    bool dragging;
    float facingAngle;     // degrees around Y axis (for smooth turning)
    AnimState currentAnim;
    int animFrame;
    AbilitySlot abilities[MAX_ABILITIES_PER_UNIT];
    int nextAbilitySlot;   // index into ACTIVATION_ORDER for clockwise cycling
} Unit;

//------------------------------------------------------------------------------------
// Snapshot of a unit for round-reset
//------------------------------------------------------------------------------------
typedef struct {
    int typeIndex;
    Vector3 position;
    Team team;
    AbilitySlot abilities[MAX_ABILITIES_PER_UNIT];
} UnitSnapshot;

//------------------------------------------------------------------------------------
// Screen Shake
//------------------------------------------------------------------------------------
typedef struct {
    float intensity;  // current intensity (decays over time)
    float duration;   // total duration
    float timer;      // time remaining
    Vector3 offset;   // current frame offset (applied to camera)
} ScreenShake;
