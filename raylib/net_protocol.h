#pragma once
#include <stdint.h>

//------------------------------------------------------------------------------------
// Network Protocol — shared between client and server
//------------------------------------------------------------------------------------
#define NET_PORT 7777
#define NET_MAGIC 0x4A4D  // "JM" — Jam Multiplayer
#define NET_MAX_PAYLOAD 4096
#define LOBBY_CODE_LEN 4

// Message header: [magic:2][type:1][size:2] = 5 bytes
#define NET_HEADER_SIZE 5

//------------------------------------------------------------------------------------
// Message types — Client to Server
//------------------------------------------------------------------------------------
typedef enum {
    MSG_JOIN             = 0x01,  // payload: lobby code (4 bytes, 0 = create new)
    MSG_READY            = 0x02,  // payload: serialized army (units + abilities)
    MSG_PLACE_UNIT       = 0x03,  // payload: unit type, position
    MSG_REMOVE_UNIT      = 0x04,  // payload: unit index
    MSG_BUY_ABILITY      = 0x05,  // payload: shop slot index
    MSG_ROLL_SHOP        = 0x06,  // payload: none
    MSG_ASSIGN_ABILITY   = 0x07,  // payload: inventory slot, unit index, ability slot
    MSG_LEADERBOARD_SUBMIT  = 0x10, // payload: serialized leaderboard entry (55 bytes)
    MSG_LEADERBOARD_REQUEST = 0x11, // payload: none
} ClientMsgType;

//------------------------------------------------------------------------------------
// Message types — Server to Client
//------------------------------------------------------------------------------------
typedef enum {
    MSG_LOBBY_CODE       = 0x80,  // payload: 4-char lobby code
    MSG_GAME_START       = 0x81,  // payload: player slot (0 or 1), starting gold
    MSG_PREP_START       = 0x82,  // payload: round number, gold, shop slots
    MSG_COMBAT_START     = 0x83,  // payload: serialized units (both teams)
    MSG_ROUND_RESULT     = 0x84,  // payload: winner (0=blue, 1=red, 2=draw), scores
    MSG_GAME_OVER        = 0x85,  // payload: final winner, scores
    MSG_SHOP_ROLL_RESULT = 0x86,  // payload: 3 shop slot ability IDs + levels
    MSG_OPPONENT_READY   = 0x87,  // payload: none
    MSG_ERROR            = 0x88,  // payload: error string
    MSG_GOLD_UPDATE      = 0x89,  // payload: current gold amount
    MSG_LEADERBOARD_DATA = 0x90,  // payload: entry count + serialized entries
} ServerMsgType;

//------------------------------------------------------------------------------------
// Serialized unit for network transfer (fixed-size, no pointers)
//------------------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint8_t typeIndex;
    uint8_t team;           // 0=blue, 1=red
    float   posX, posZ;
    float   currentHealth;
    float   facingAngle;
    // Abilities (4 slots)
    struct __attribute__((packed)) {
        int8_t  abilityId;  // -1 = empty
        uint8_t level;
    } abilities[4];
} NetUnit;

#define NET_MAX_UNITS 64

//------------------------------------------------------------------------------------
// Message structure (in-memory, not wire format)
//------------------------------------------------------------------------------------
typedef struct {
    uint8_t  type;
    uint16_t size;          // payload size
    uint8_t  payload[NET_MAX_PAYLOAD];
} NetMessage;
