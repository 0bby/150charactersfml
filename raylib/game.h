#pragma once
#include <stdint.h>
#include "raylib.h"
#include "unit_stats.h"
#include "abilities.h"

//------------------------------------------------------------------------------------
// Data Structures & Constants
//------------------------------------------------------------------------------------
#define RARITY_COMMON 0
#define RARITY_RARE 1
#define RARITY_LEGENDARY 2
#define RARITY_MULT_RARE 1.1f
#define RARITY_MULT_LEGENDARY 1.3f

#define MAX_UNIT_TYPES 8
#define MAX_UNITS 64
#define TOTAL_ROUNDS 5
#define ATTACK_RANGE 12.0f      // how close a unit needs to be to attack
#define UNIT_COLLISION_RADIUS 3.0f  // circle-circle push radius for unit separation
#define BLUE_TEAM_MAX_SIZE 4   // player team cap (change this to rebalance)
#define ARENA_BOUNDARY_Z   5.0f // blue units can't be placed below this Z (into red territory)
#define ARENA_GRID_HALF  100.0f // half the visible grid (grid goes -100 to +100)
#define MAX_WAVE_ENEMIES 8

//------------------------------------------------------------------------------------
// Team
//------------------------------------------------------------------------------------
typedef enum { TEAM_BLUE = 0, TEAM_RED = 1 } Team;

//------------------------------------------------------------------------------------
// Game phases
//------------------------------------------------------------------------------------
typedef enum {
    PHASE_PLAZA,      // 3D plaza with roaming enemies, interactive objects
    PHASE_LOBBY,      // waiting in multiplayer lobby
    PHASE_PREP,       // place / arrange units
    PHASE_COMBAT,     // units fight automatically
    PHASE_ROUND_OVER, // brief pause showing round result
    PHASE_MILESTONE,  // "Set in Stone" selection screen
    PHASE_GAME_OVER,  // all rounds finished
} GamePhase;

typedef enum { ANIM_IDLE = 0, ANIM_WALK, ANIM_SCARED, ANIM_ATTACK, ANIM_CAST, ANIM_COUNT } AnimState;

#define MAX_SHOP_SLOTS 3
#define MAX_MODIFIERS 128
#define MAX_PROJECTILES 32
#define MAX_PARTICLES 1024
#define MAX_FLOATING_TEXTS 32
#define MAX_INVENTORY_SLOTS 6
#define MAX_FISSURES 8

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
    float maxDuration;
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
    float chargeTimer;  // >0 = still charging (not moving yet)
    float chargeMax;    // total charge time (for size lerp)
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
// Base values (designed for 720p) — scaled at runtime via uiScale
#define HUD_UNIT_BAR_HEIGHT_BASE 130
#define HUD_SHOP_HEIGHT_BASE 50
#define HUD_CARD_WIDTH_BASE 180
#define HUD_CARD_HEIGHT_BASE 120
#define HUD_CARD_SPACING_BASE 10
#define HUD_PORTRAIT_SIZE_BASE 80
#define HUD_ABILITY_SLOT_SIZE_BASE 32
#define HUD_ABILITY_SLOT_GAP_BASE 4
// Non-scaled constants
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
    ModelAnimation *scaredAnims;    // scared animations (NULL if none)
    int scaredAnimCount;            // number of scared animations
    ModelAnimation *attackAnims;    // melee attack anims (NULL if none)
    int attackAnimCount;
    ModelAnimation *castAnims;      // spellcast/ability anims (NULL if none)
    int castAnimCount;
    int animIndex[ANIM_COUNT];      // index into respective anim array (-1 = not found)
    bool hasAnimations;
    float yOffset;          // vertical draw offset (raise/lower model)
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
    float gazeAccum;       // Stone Gaze: time spent facing a stone-gazer
    float scaleOverride;   // model scale multiplier (1.0 = normal, 2.5 = boss)
    float hpMultiplier;    // max HP multiplier (1.0 = normal)
    float dmgMultiplier;   // attack damage multiplier (1.0 = normal)
    float speedMultiplier; // movement speed multiplier (1.0 = normal)
    float shieldHP;        // absorbs damage before HP (blue bar visual)
    float abilityCastDelay; // 0.75s delay between successive ability casts
    int   chargeTarget;    // Primal Charge: target unit index (-1 = not charging)
    float hitFlash;        // >0 = flash white on damage (decays to 0)
    float castPause;       // >0 = frozen after casting projectile ability
    float attackAnimTimer; // >0 = playing attack animation (counts down)
    // NFC tag UID (travels with unit during array compaction)
    unsigned char nfcUid[7];
    int nfcUidLen;         // 0 = not from NFC
    uint8_t rarity;        // 0=common, 1=rare, 2=legendary
    char nfcName[32];      // custom creature name (empty = use class name)
} Unit;

//------------------------------------------------------------------------------------
// Snapshot of a unit for round-reset
//------------------------------------------------------------------------------------
typedef struct {
    int typeIndex;
    Vector3 position;
    Team team;
    AbilitySlot abilities[MAX_ABILITIES_PER_UNIT];
    unsigned char nfcUid[7];
    int nfcUidLen;
    uint8_t rarity;
    char nfcName[32];
    float hpMultiplier;
    float dmgMultiplier;
    float speedMultiplier;
} UnitSnapshot;

//------------------------------------------------------------------------------------
// Screen Shake
//------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------
// Floating Text (spell shouts)
//------------------------------------------------------------------------------------
typedef struct {
    Vector3 position;       // world position (rises over time)
    char text[32];          // ability name
    Color color;
    float life;
    float maxLife;
    int fontSize;           // 0 = use default (16)
    float driftX;           // horizontal drift speed (pixels/sec in screen space)
    bool active;
} FloatingText;

//------------------------------------------------------------------------------------
// Screen Shake
//------------------------------------------------------------------------------------
typedef struct {
    float intensity;  // current intensity (decays over time)
    float duration;   // total duration
    float timer;      // time remaining
    Vector3 offset;   // current frame offset (applied to camera)
} ScreenShake;

//------------------------------------------------------------------------------------
// Unit Introduction Screen ("New Challenger" splash)
//------------------------------------------------------------------------------------
#define INTRO_DURATION       2.0f
#define INTRO_WIPE_IN        0.3f    // slash-wipe reveal duration
#define INTRO_HOLD_START     0.3f    // model + text appear
#define INTRO_FADE_OUT_START 1.5f
#define INTRO_FADE_OUT_END   2.0f

typedef struct {
    bool    active;
    float   timer;        // counts UP from 0 to INTRO_DURATION
    int     typeIndex;    // which unit type (0=Mushroom, 1=Goblin)
    int     unitIndex;    // index in units[] array
    int     animFrame;    // dedicated anim counter for intro model
} UnitIntro;

//------------------------------------------------------------------------------------
// Statue Spawn Animation (blue units fall from sky as stone statues)
//------------------------------------------------------------------------------------
#define SPAWN_ANIM_DELAY            0.08f
#define SPAWN_ANIM_START_Y          250.0f
#define SPAWN_ANIM_GRAVITY          350.0f
#define SPAWN_ANIM_IMPACT_PARTICLES 25
#define SPAWN_ANIM_SHAKE_INTENSITY  12.0f
#define SPAWN_ANIM_SHAKE_DURATION   0.45f
#define SPAWN_ANIM_TRAIL_INTERVAL   0.02f

typedef enum {
    SSPAWN_INACTIVE = 0,
    SSPAWN_DELAY,
    SSPAWN_FALLING,
    SSPAWN_DONE,
} StatueSpawnPhase;

typedef struct {
    StatueSpawnPhase phase;
    int     unitIndex;
    float   timer;
    float   currentY;
    float   velocityY;
    float   targetY;    // 0.0
    float   trailTimer;
    float   driftX;     // random XZ offset at start, lerps to 0 at ground
    float   driftZ;
} StatueSpawn;

//------------------------------------------------------------------------------------
// Fissure (terrain obstacle)
//------------------------------------------------------------------------------------
typedef struct {
    Vector3 position;     // center of fissure
    float   rotation;     // angle in degrees on XZ plane
    float   length;       // along rotation axis
    float   width;        // perpendicular to rotation
    float   duration;     // remaining lifetime
    bool    active;
    Team    sourceTeam;
    int     sourceIndex;
} Fissure;

//------------------------------------------------------------------------------------
// Combat State (bundled game state for ability handlers)
//------------------------------------------------------------------------------------
typedef struct {
    Unit *units;
    int unitCount;
    Modifier *modifiers;
    Projectile *projectiles;
    Particle *particles;
    Fissure *fissures;
    FloatingText *floatingTexts;
    ScreenShake *shake;
#ifndef SERVER_BUILD
    void *battleLog;    // BattleLog* (void* to avoid pulling BattleLog into server)
    float combatTime;
#endif
} CombatState;

//------------------------------------------------------------------------------------
// Wave System
//------------------------------------------------------------------------------------
typedef struct {
    int unitType;       // -1 = random from available types
    int numAbilities;   // ability slots to fill (0-4)
    int abilityLevel;   // level for each ability (0, 1, or 2)
    float hpMult;       // HP multiplier
    float dmgMult;      // attack damage multiplier
    float scaleMult;    // model scale multiplier
} WaveEntry;

typedef struct {
    WaveEntry entries[MAX_WAVE_ENEMIES];
    int count;
} WaveDef;

//------------------------------------------------------------------------------------
// Combat Event (for deterministic combat simulation feedback)
//------------------------------------------------------------------------------------
#define MAX_COMBAT_EVENTS 64
typedef enum {
    COMBAT_EVT_ABILITY_CAST,   // unit cast an ability
    COMBAT_EVT_SHAKE,          // screen shake trigger
} CombatEventType;

typedef struct {
    CombatEventType type;
    int unitIndex;
    int abilityId;             // for ABILITY_CAST
    Vector3 position;
    float value1;              // intensity for SHAKE
    float value2;              // duration for SHAKE
} CombatEvent;

//------------------------------------------------------------------------------------
// Battle Log (client-only persistent combat event log)
//------------------------------------------------------------------------------------
#ifndef SERVER_BUILD
typedef enum { BLOG_CAST, BLOG_KILL } BattleLogType;

typedef struct {
    BattleLogType type;
    float timestamp;
    char text[80];
    Color color;
} BattleLogEntry;

#define MAX_BATTLE_LOG 64
typedef struct {
    BattleLogEntry entries[MAX_BATTLE_LOG];
    int count;
    int scroll;
} BattleLog;
#endif

//------------------------------------------------------------------------------------
// Environment Piece Editor
//------------------------------------------------------------------------------------
#define MAX_ENV_PIECES 32
#define MAX_ENV_MODELS 8

typedef struct {
    const char *name;        // display name for UI
    const char *modelPath;
    const char *texturePath;    // NULL if no separate BC texture
    const char *ormTexturePath; // NULL if no separate ORM texture
    const char *normalTexturePath; // NULL if no normal map
    Model model;
    Texture2D texture;          // BC texture (id=0 if none)
    Texture2D ormTexture;       // ORM texture (id=0 if none)
    Texture2D normalTexture;    // Normal map texture (id=0 if none)
    bool loaded;
} EnvModelDef;

typedef struct {
    int modelIndex;          // index into envModels[]
    Vector3 position;
    float rotationY;         // degrees around Y axis
    float scale;             // uniform multiplier (1.0 = auto-computed default)
    bool active;
} EnvPiece;
