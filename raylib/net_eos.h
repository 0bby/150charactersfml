#pragma once
#ifdef USE_EOS

#include "net_protocol.h"
#include "game.h"
#include <stdbool.h>

//------------------------------------------------------------------------------------
// EOS P2P Transport — mirrors NetClient API for seamless integration
//------------------------------------------------------------------------------------

typedef enum {
    EOS_STATE_UNINIT = 0,
    EOS_STATE_LOGGING_IN,
    EOS_STATE_LOGGED_IN,
    EOS_STATE_IN_LOBBY,
    EOS_STATE_IN_GAME,
    EOS_STATE_ERROR,
} EosState;

typedef struct {
    EosState state;
    int  playerSlot;           // 0 or 1 (assigned during lobby)
    char lobbyCode[LOBBY_CODE_LEN + 1];
    char errorMsg[128];
    bool isHost;

    // Flags consumed by main loop (identical to NetClient)
    bool gameStarted;
    bool prepStarted;
    bool combatStarted;
    bool roundResultReady;
    bool gameOver;
    bool opponentReady;
    bool shopUpdated;
    bool goldUpdated;
    bool peerDisconnected;
    int  prepTimeRemaining;    // seconds left in prep timer (from MSG_OPPONENT_READY)

    // Player names
    char opponentName[32];

    // Data from server messages
    int startingGold;
    int currentGold;
    int currentRound;
    bool isPveRound;

    // Round result
    int roundWinner;
    bool roundIsPve;
    int playerHealth[2];       // HP per player (indexed by server slot)
    int lastRoundDamage;       // damage dealt this round

    // Game over
    int gameWinner;

    // Combat units
    NetUnit combatNetUnits[NET_MAX_UNITS];
    int combatNetUnitCount;
    uint32_t combatSeed;

    // Combat sync snapshots
    bool combatSyncReady;
    SyncUnit combatSyncUnits[NET_MAX_UNITS];
    int combatSyncCount;
    uint16_t combatSyncTick;
    uint32_t combatSyncHash;   // state hash for desync detection

    // Shop
    ShopSlot serverShop[MAX_SHOP_SLOTS];
} EosClient;

//------------------------------------------------------------------------------------
// Lifecycle (call from main)
//------------------------------------------------------------------------------------

// Initialize EOS SDK + start Device ID auth. Returns 0 on success.
// Pass altInstance=true for the second instance on the same machine
// (deletes existing device ID and creates a fresh one).
int  eos_init(bool altInstance);

// Call every frame to drive EOS callbacks.
void eos_tick(void);

// Shutdown EOS SDK. Call on exit.
void eos_shutdown(void);

// True once Device ID auth is complete.
bool eos_is_logged_in(void);

//------------------------------------------------------------------------------------
// Client state init
//------------------------------------------------------------------------------------
void eos_client_init(EosClient *ec);

//------------------------------------------------------------------------------------
// Lobby
//------------------------------------------------------------------------------------

// Host: create lobby, generate 4-char code. Returns 0 on success (async).
int  eos_host_lobby(EosClient *ec, const char *playerName);

// Join: search for lobby by code. Returns 0 on success (async).
int  eos_join_lobby(EosClient *ec, const char *lobbyCode, const char *playerName);

//------------------------------------------------------------------------------------
// Message API (mirrors net_client_send_*)
//------------------------------------------------------------------------------------
void eos_client_poll(EosClient *ec);
void eos_client_send_ready(EosClient *ec, const Unit units[], int unitCount);
void eos_client_send_roll(EosClient *ec);
void eos_client_send_buy(EosClient *ec, int shopSlot);
void eos_client_disconnect(EosClient *ec);

#endif // USE_EOS
