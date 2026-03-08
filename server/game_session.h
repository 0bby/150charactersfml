#pragma once
#include <stdint.h>
#include "../raylib/game.h"
#include "../raylib/net_protocol.h"
#include "../raylib/net_common.h"
#include "../raylib/pve_waves.h"

//------------------------------------------------------------------------------------
// Game Session — manages one 1v1 match between two players
//------------------------------------------------------------------------------------
#define COMBAT_DT (1.0f / 60.0f)  // headless combat tick rate
#define STARTING_HEALTH 20 // HP per player at game start
#define MAX_ROUNDS 10      // absolute max rounds
#define PREP_TIMER 45.0f   // seconds before auto-ready

typedef enum {
    SESSION_WAITING,       // waiting for second player
    SESSION_PREP,          // both connected, prep phase
    SESSION_COMBAT,        // combat running (headless)
    SESSION_ROUND_OVER,    // brief pause after combat
    SESSION_GAME_OVER,     // match finished
    SESSION_DEAD,          // session cleaned up
} SessionState;

typedef struct {
    int sockfd;
    bool connected;
    bool ready;
    char name[32];
    // Player's army
    Unit units[MAX_UNITS];
    int unitCount;
    // Economy
    int gold;
    ShopSlot shop[MAX_SHOP_SLOTS];
    InventorySlot inventory[MAX_INVENTORY_SLOTS];
} PlayerState;

typedef struct {
    SessionState state;
    char lobbyCode[LOBBY_CODE_LEN + 1];
    PlayerState players[2];

    // Round state
    int currentRound;
    int playerHealth[2];   // HP per player, starts at STARTING_HEALTH

    // Combat state (headless)
    Unit combatUnits[MAX_UNITS];
    int combatUnitCount;
    Modifier combatModifiers[MAX_MODIFIERS];
    Projectile combatProjectiles[MAX_PROJECTILES];
    Fissure combatFissures[MAX_FISSURES];

    // Combat sync
    int combatTickCount;    // tick counter within current combat round
    int combatBlueCount;    // number of blue (player 0) units at combat start
    uint32_t combatSeed;    // shared RNG seed for deterministic combat

    // Prep timer
    float prepTimer;
} GameSession;

// Initialize a new session with the first player's socket
void session_init(GameSession *s, int player0_sock);

// Add second player. Returns 0 on success.
int session_add_player(GameSession *s, int player1_sock);

// Tick the session. Called from main server loop.
// Returns 0 if session is still alive, 1 if session is dead.
int session_tick(GameSession *s, float dt);

// Handle a message from a player (0 or 1).
void session_handle_msg(GameSession *s, int playerIdx, const NetMessage *msg);

// Send the current shop state to a player.
void session_send_shop(GameSession *s, int playerIdx);

// Start prep phase (send gold, shop, round info)
void session_start_prep(GameSession *s);

// Start combat phase
void session_start_combat(GameSession *s);
