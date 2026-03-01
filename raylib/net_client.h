#pragma once
#include "net_protocol.h"
#include "net_common.h"
#include "game.h"

//------------------------------------------------------------------------------------
// Client Network State
//------------------------------------------------------------------------------------
typedef enum {
    NET_DISCONNECTED = 0,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_IN_LOBBY,
    NET_IN_GAME,
    NET_ERROR,
} NetClientState;

typedef struct {
    int sockfd;
    NetClientState state;
    int playerSlot;            // 0 or 1 (assigned by server)
    char lobbyCode[LOBBY_CODE_LEN + 1];
    char errorMsg[128];

    // Flags set by incoming messages (consumed by main loop)
    bool gameStarted;
    bool prepStarted;
    bool combatStarted;
    bool roundResultReady;
    bool gameOver;
    bool opponentReady;
    bool shopUpdated;
    bool goldUpdated;

    // Player names
    char opponentName[32];

    // Data from server messages
    int startingGold;
    int currentGold;
    int currentRound;
    bool isPveRound;

    // Round result
    int roundWinner;           // 0=blue(me), 1=red(opponent), 2=draw
    bool roundIsPve;
    int pvpWins[2];

    // Game over
    int gameWinner;            // 0=me, 1=opponent

    // Combat units from server
    NetUnit combatNetUnits[NET_MAX_UNITS];
    int combatNetUnitCount;

    // Shop from server
    ShopSlot serverShop[MAX_SHOP_SLOTS];
} NetClient;

// Initialize client state (does not connect)
void net_client_init(NetClient *nc);

// Connect to server and send JOIN (create lobby: code=NULL, join: code="ABCD")
// playerName is sent to the server (max 15 chars). Returns 0 on success, -1 on error.
int net_client_connect(NetClient *nc, const char *host, int port, const char *lobbyCode, const char *playerName);

// Non-blocking poll for incoming messages. Call each frame.
void net_client_poll(NetClient *nc);

// Send MSG_READY with the player's army
void net_client_send_ready(NetClient *nc, const Unit units[], int unitCount);

// Send MSG_ROLL_SHOP
void net_client_send_roll(NetClient *nc);

// Send MSG_BUY_ABILITY with shop slot index
void net_client_send_buy(NetClient *nc, int shopSlot);

// Send MSG_PLACE_UNIT
void net_client_send_place_unit(NetClient *nc, int typeIndex, float posX, float posZ);

// Send MSG_REMOVE_UNIT
void net_client_send_remove_unit(NetClient *nc, int unitIndex);

// Disconnect and cleanup
void net_client_disconnect(NetClient *nc);

// Standalone leaderboard operations (short-lived blocking TCP connections)
#include "leaderboard.h"
int net_leaderboard_submit(const char *host, int port, const LeaderboardEntry *entry);
int net_leaderboard_fetch(const char *host, int port, Leaderboard *lb);

// NFC tag operations (short-lived blocking TCP connections)
// Lookup: returns 0 on success (check outStatus for NFC_STATUS_OK/NOT_FOUND), -1 on network error
// outAbilities receives 4 ability slots from the server
int net_nfc_lookup(const char *host, int port, const uint8_t *uid, int uidLen,
                   uint8_t *outStatus, uint8_t *outTypeIndex, uint8_t *outRarity,
                   AbilitySlot outAbilities[MAX_ABILITIES_PER_UNIT]);

// Update abilities on server for a given NFC tag UID. Returns 0 on success, -1 on error.
int net_nfc_update_abilities(const char *host, int port, const uint8_t *uid, int uidLen,
                             const AbilitySlot abilities[], int abilityCount);

// Reset abilities on server for a given NFC tag UID. Returns 0 on success, -1 on error.
int net_nfc_reset_abilities(const char *host, int port, const uint8_t *uid, int uidLen);
