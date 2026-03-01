#include "net_client.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

void net_client_init(NetClient *nc)
{
    memset(nc, 0, sizeof(*nc));
    nc->sockfd = -1;
    nc->state = NET_DISCONNECTED;
    for (int i = 0; i < MAX_SHOP_SLOTS; i++)
        nc->serverShop[i].abilityId = -1;
}

int net_client_connect(NetClient *nc, const char *host, int port, const char *lobbyCode, const char *playerName)
{
    nc->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (nc->sockfd < 0) {
        snprintf(nc->errorMsg, sizeof(nc->errorMsg), "Failed to create socket");
        nc->state = NET_ERROR;
        return -1;
    }

    // Resolve hostname
    struct hostent *he = gethostbyname(host);
    if (!he) {
        snprintf(nc->errorMsg, sizeof(nc->errorMsg), "Cannot resolve host: %s", host);
        close(nc->sockfd); nc->sockfd = -1;
        nc->state = NET_ERROR;
        return -1;
    }

    struct sockaddr_in servaddr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    memcpy(&servaddr.sin_addr, he->h_addr_list[0], he->h_length);

    // Connect (blocking for simplicity)
    if (connect(nc->sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        snprintf(nc->errorMsg, sizeof(nc->errorMsg), "Connection failed: %s", strerror(errno));
        close(nc->sockfd); nc->sockfd = -1;
        nc->state = NET_ERROR;
        return -1;
    }

    // TCP_NODELAY
    int one = 1;
    setsockopt(nc->sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Send JOIN message: [lobbyCode:4][nameLen:1][name:N]
    uint8_t joinPayload[LOBBY_CODE_LEN + 1 + 32] = {0};
    if (lobbyCode && lobbyCode[0]) {
        memcpy(joinPayload, lobbyCode, LOBBY_CODE_LEN);
    }
    int nameLen = 0;
    if (playerName && playerName[0]) {
        nameLen = (int)strlen(playerName);
        if (nameLen > 15) nameLen = 15;
        joinPayload[LOBBY_CODE_LEN] = (uint8_t)nameLen;
        memcpy(joinPayload + LOBBY_CODE_LEN + 1, playerName, nameLen);
    }
    if (net_send_msg(nc->sockfd, MSG_JOIN, joinPayload, LOBBY_CODE_LEN + 1 + nameLen) < 0) {
        snprintf(nc->errorMsg, sizeof(nc->errorMsg), "Failed to send JOIN");
        close(nc->sockfd); nc->sockfd = -1;
        nc->state = NET_ERROR;
        return -1;
    }

    // Set non-blocking for polling
    net_set_nonblocking(nc->sockfd);
    nc->state = NET_CONNECTING;
    printf("[Net] Connected to %s:%d, sent JOIN\n", host, port);
    return 0;
}

static void handle_server_msg(NetClient *nc, const NetMessage *msg)
{
    switch (msg->type) {
    case MSG_LOBBY_CODE:
        if (msg->size >= LOBBY_CODE_LEN) {
            memcpy(nc->lobbyCode, msg->payload, LOBBY_CODE_LEN);
            nc->lobbyCode[LOBBY_CODE_LEN] = '\0';
            nc->state = NET_IN_LOBBY;
            printf("[Net] Lobby code: %s\n", nc->lobbyCode);
        }
        break;

    case MSG_GAME_START:
        if (msg->size >= 2) {
            nc->playerSlot = msg->payload[0];
            nc->startingGold = msg->payload[1];
            nc->currentGold = nc->startingGold;
            // Parse opponent name: [slot:1][gold:1][oppNameLen:1][oppName:N]
            nc->opponentName[0] = '\0';
            if (msg->size >= 3) {
                int oppNameLen = msg->payload[2];
                if (oppNameLen > 31) oppNameLen = 31;
                if (msg->size >= 3 + oppNameLen) {
                    memcpy(nc->opponentName, msg->payload + 3, oppNameLen);
                    nc->opponentName[oppNameLen] = '\0';
                }
            }
            nc->gameStarted = true;
            nc->state = NET_IN_GAME;
            printf("[Net] Game started! Slot=%d, Gold=%d, Opponent=%s\n",
                   nc->playerSlot, nc->startingGold, nc->opponentName);
        }
        break;

    case MSG_PREP_START:
        if (msg->size >= 4) {
            nc->currentRound = msg->payload[0];
            nc->isPveRound = msg->payload[1];
            nc->currentGold = ((int)msg->payload[2] << 8) | msg->payload[3];
            nc->prepStarted = true;
            nc->opponentReady = false;
            printf("[Net] Prep phase: round=%d, pve=%d, gold=%d\n",
                   nc->currentRound, nc->isPveRound, nc->currentGold);
        }
        break;

    case MSG_COMBAT_START:
        if (msg->size >= 2) {
            nc->currentRound = msg->payload[0];
            nc->combatNetUnitCount = msg->payload[1];
            if (nc->combatNetUnitCount > NET_MAX_UNITS) nc->combatNetUnitCount = NET_MAX_UNITS;
            if (msg->size >= 2 + nc->combatNetUnitCount * (int)sizeof(NetUnit)) {
                memcpy(nc->combatNetUnits, msg->payload + 2,
                       nc->combatNetUnitCount * sizeof(NetUnit));
            }
            nc->combatStarted = true;
            printf("[Net] Combat start: %d units\n", nc->combatNetUnitCount);
        }
        break;

    case MSG_ROUND_RESULT:
        if (msg->size >= 5) {
            nc->roundWinner = msg->payload[0];
            nc->roundIsPve = msg->payload[1];
            nc->pvpWins[0] = msg->payload[2];
            nc->pvpWins[1] = msg->payload[3];
            nc->currentRound = msg->payload[4];
            nc->roundResultReady = true;
            printf("[Net] Round result: winner=%d, pvpWins=%d-%d\n",
                   nc->roundWinner, nc->pvpWins[0], nc->pvpWins[1]);
        }
        break;

    case MSG_GAME_OVER:
        if (msg->size >= 3) {
            nc->gameWinner = msg->payload[0];
            nc->pvpWins[0] = msg->payload[1];
            nc->pvpWins[1] = msg->payload[2];
            nc->gameOver = true;
            printf("[Net] Game over: %s\n", nc->gameWinner == 0 ? "YOU WIN" : "YOU LOSE");
        }
        break;

    case MSG_SHOP_ROLL_RESULT: {
        int count = msg->size / 2;
        if (count > MAX_SHOP_SLOTS) count = MAX_SHOP_SLOTS;
        for (int i = 0; i < count; i++) {
            nc->serverShop[i].abilityId = (int8_t)msg->payload[i * 2];
            nc->serverShop[i].level = msg->payload[i * 2 + 1];
        }
        nc->shopUpdated = true;
    } break;

    case MSG_OPPONENT_READY:
        nc->opponentReady = true;
        printf("[Net] Opponent is ready!\n");
        break;

    case MSG_GOLD_UPDATE:
        if (msg->size >= 2) {
            nc->currentGold = ((int)msg->payload[0] << 8) | msg->payload[1];
            nc->goldUpdated = true;
        }
        break;

    case MSG_ERROR:
        if (msg->size > 0) {
            int len = msg->size < 127 ? msg->size : 127;
            memcpy(nc->errorMsg, msg->payload, len);
            nc->errorMsg[len] = '\0';
        }
        nc->state = NET_ERROR;
        printf("[Net] Server error: %s\n", nc->errorMsg);
        break;

    default:
        printf("[Net] Unknown server msg type 0x%02X\n", msg->type);
        break;
    }
}

void net_client_poll(NetClient *nc)
{
    if (nc->sockfd < 0) return;

    // Process all available messages
    NetMessage msg;
    int result;
    while ((result = net_recv_msg_nonblock(nc->sockfd, &msg)) > 0) {
        handle_server_msg(nc, &msg);
    }
    if (result < 0) {
        snprintf(nc->errorMsg, sizeof(nc->errorMsg), "Disconnected from server");
        nc->state = NET_ERROR;
        close(nc->sockfd);
        nc->sockfd = -1;
    }
}

void net_client_send_ready(NetClient *nc, const Unit units[], int unitCount)
{
    if (nc->sockfd < 0) return;
    NetUnit netUnits[NET_MAX_UNITS];
    int count = serialize_units(units, unitCount, netUnits, NET_MAX_UNITS);
    uint8_t payload[1 + sizeof(NetUnit) * NET_MAX_UNITS];
    payload[0] = (uint8_t)count;
    memcpy(payload + 1, netUnits, count * sizeof(NetUnit));
    net_send_msg(nc->sockfd, MSG_READY, payload, 1 + count * sizeof(NetUnit));
}

void net_client_send_roll(NetClient *nc)
{
    if (nc->sockfd < 0) return;
    net_send_msg(nc->sockfd, MSG_ROLL_SHOP, NULL, 0);
}

void net_client_send_buy(NetClient *nc, int shopSlot)
{
    if (nc->sockfd < 0) return;
    uint8_t payload[1] = { (uint8_t)shopSlot };
    net_send_msg(nc->sockfd, MSG_BUY_ABILITY, payload, 1);
}

void net_client_send_place_unit(NetClient *nc, int typeIndex, float posX, float posZ)
{
    if (nc->sockfd < 0) return;
    uint8_t payload[9];
    payload[0] = (uint8_t)typeIndex;
    memcpy(payload + 1, &posX, 4);
    memcpy(payload + 5, &posZ, 4);
    net_send_msg(nc->sockfd, MSG_PLACE_UNIT, payload, 9);
}

void net_client_send_remove_unit(NetClient *nc, int unitIndex)
{
    if (nc->sockfd < 0) return;
    uint8_t payload[1] = { (uint8_t)unitIndex };
    net_send_msg(nc->sockfd, MSG_REMOVE_UNIT, payload, 1);
}

void net_client_disconnect(NetClient *nc)
{
    if (nc->sockfd >= 0) {
        close(nc->sockfd);
        nc->sockfd = -1;
    }
    nc->state = NET_DISCONNECTED;
}

//------------------------------------------------------------------------------------
// Standalone leaderboard operations (short-lived blocking TCP)
//------------------------------------------------------------------------------------
int net_leaderboard_submit(const char *host, int port, const LeaderboardEntry *entry)
{
    int sockfd = net_shortlived_connect(host, port);
    if (sockfd < 0) {
        printf("[Leaderboard] Failed to connect for submit\n");
        return -1;
    }

    uint8_t payload[LEADERBOARD_ENTRY_NET_SIZE];
    serialize_leaderboard_entry(entry, payload, sizeof(payload));

    if (net_send_msg(sockfd, MSG_LEADERBOARD_SUBMIT, payload, LEADERBOARD_ENTRY_NET_SIZE) < 0) {
        printf("[Leaderboard] Failed to send submit\n");
        close(sockfd);
        return -1;
    }

    // Optionally read back the full leaderboard response (best-effort)
    close(sockfd);
    printf("[Leaderboard] Submitted entry for '%s' round %d\n", entry->playerName, entry->highestRound);
    return 0;
}

int net_leaderboard_fetch(const char *host, int port, Leaderboard *lb)
{
    int sockfd = net_shortlived_connect(host, port);
    if (sockfd < 0) {
        printf("[Leaderboard] Failed to connect for fetch\n");
        return -1;
    }

    if (net_send_msg(sockfd, MSG_LEADERBOARD_REQUEST, NULL, 0) < 0) {
        printf("[Leaderboard] Failed to send request\n");
        close(sockfd);
        return -1;
    }

    NetMessage msg;
    if (net_recv_msg(sockfd, &msg) < 0 || msg.type != MSG_LEADERBOARD_DATA) {
        printf("[Leaderboard] Failed to receive leaderboard data\n");
        close(sockfd);
        return -1;
    }

    close(sockfd);

    // Deserialize: [entryCount:1][entries...]
    if (msg.size < 1) return -1;
    int count = msg.payload[0];
    if (count > MAX_LEADERBOARD_ENTRIES) count = MAX_LEADERBOARD_ENTRIES;
    if (msg.size < 1 + count * LEADERBOARD_ENTRY_NET_SIZE) {
        printf("[Leaderboard] Truncated data: expected %d entries\n", count);
        return -1;
    }

    lb->entryCount = 0;
    for (int i = 0; i < count; i++) {
        if (deserialize_leaderboard_entry(
                msg.payload + 1 + i * LEADERBOARD_ENTRY_NET_SIZE,
                LEADERBOARD_ENTRY_NET_SIZE,
                &lb->entries[lb->entryCount]) == LEADERBOARD_ENTRY_NET_SIZE) {
            lb->entryCount++;
        }
    }

    printf("[Leaderboard] Fetched %d entries from server\n", lb->entryCount);
    return 0;
}

//------------------------------------------------------------------------------------
// NFC UID cache — prefetch & local check
//------------------------------------------------------------------------------------
int net_nfc_prefetch(const char *host, int port, NfcUidCache *cache)
{
    memset(cache, 0, sizeof(*cache));

    int sockfd = net_shortlived_connect(host, port);
    if (sockfd < 0) {
        printf("[NFC] Failed to connect for prefetch\n");
        return -1;
    }

    if (net_send_msg(sockfd, MSG_NFC_PREFETCH, NULL, 0) < 0) {
        printf("[NFC] Failed to send prefetch\n");
        close(sockfd);
        return -1;
    }

    NetMessage msg;
    if (net_recv_msg(sockfd, &msg) < 0 || msg.type != MSG_NFC_PREFETCH_DATA) {
        printf("[NFC] Failed to receive prefetch data\n");
        close(sockfd);
        return -1;
    }
    close(sockfd);

    // Parse: [count:2 LE][uids × (hexLen:1, hexChars:N)]
    if (msg.size < 2) return -1;
    int count = msg.payload[0] | (msg.payload[1] << 8);
    if (count > NFC_CACHE_MAX) count = NFC_CACHE_MAX;

    int off = 2;
    for (int i = 0; i < count && off < msg.size; i++) {
        int hexLen = msg.payload[off++];
        if (hexLen <= 0 || hexLen >= 15 || off + hexLen > msg.size) break;
        memcpy(cache->uids[cache->count], msg.payload + off, hexLen);
        cache->uids[cache->count][hexLen] = '\0';
        cache->count++;
        off += hexLen;
    }

    printf("[NFC] Prefetched %d known UIDs from server\n", cache->count);
    return 0;
}

bool nfc_cache_contains(const NfcUidCache *cache, const char *uidHex)
{
    for (int i = 0; i < cache->count; i++) {
        if (strcasecmp(cache->uids[i], uidHex) == 0) return true;
    }
    return false;
}

//------------------------------------------------------------------------------------
// NFC tag lookup (short-lived blocking TCP)
//------------------------------------------------------------------------------------
int net_nfc_lookup(const char *host, int port, const uint8_t *uid, int uidLen,
                   uint8_t *outStatus, uint8_t *outTypeIndex, uint8_t *outRarity,
                   AbilitySlot outAbilities[MAX_ABILITIES_PER_UNIT])
{
    if (uidLen < 4 || uidLen > NFC_UID_MAX_LEN) return -1;

    int sockfd = net_shortlived_connect(host, port);
    if (sockfd < 0) {
        printf("[NFC] Failed to connect for lookup\n");
        return -1;
    }

    uint8_t payload[1 + NFC_UID_MAX_LEN];
    payload[0] = (uint8_t)uidLen;
    memcpy(payload + 1, uid, uidLen);

    if (net_send_msg(sockfd, MSG_NFC_LOOKUP, payload, 1 + uidLen) < 0) {
        printf("[NFC] Failed to send lookup\n");
        close(sockfd);
        return -1;
    }

    NetMessage msg;
    if (net_recv_msg(sockfd, &msg) < 0 || msg.type != MSG_NFC_DATA) {
        printf("[NFC] Failed to receive NFC data\n");
        close(sockfd);
        return -1;
    }
    close(sockfd);

    // Parse response: [uidLen:1][uid:N][status:1][typeIndex:1][rarity:1][abilities × 4 × (id:1, level:1)]
    if (msg.size < 1 + uidLen + 3) return -1;
    *outStatus = msg.payload[1 + uidLen];
    *outTypeIndex = msg.payload[2 + uidLen];
    *outRarity = msg.payload[3 + uidLen];

    // Parse abilities if present
    int abOff = 1 + uidLen + 3;
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        if (abOff + 1 < msg.size) {
            outAbilities[a].abilityId = (int8_t)msg.payload[abOff++];
            outAbilities[a].level = msg.payload[abOff++];
        } else {
            outAbilities[a].abilityId = -1;
            outAbilities[a].level = 0;
        }
        outAbilities[a].cooldownRemaining = 0;
        outAbilities[a].triggered = false;
    }
    return 0;
}

//------------------------------------------------------------------------------------
// NFC ability update (short-lived blocking TCP)
//------------------------------------------------------------------------------------
int net_nfc_update_abilities(const char *host, int port, const uint8_t *uid, int uidLen,
                             const AbilitySlot abilities[], int abilityCount)
{
    if (uidLen < 4 || uidLen > NFC_UID_MAX_LEN) return -1;

    int sockfd = net_shortlived_connect(host, port);
    if (sockfd < 0) {
        printf("[NFC] Failed to connect for ability update\n");
        return -1;
    }

    // Payload: [uidLen:1][uid:N][abilityCount:1][abilities × (id:1, level:1)]
    uint8_t payload[1 + NFC_UID_MAX_LEN + 1 + MAX_ABILITIES_PER_UNIT * 2];
    int off = 0;
    payload[off++] = (uint8_t)uidLen;
    memcpy(payload + off, uid, uidLen);
    off += uidLen;
    payload[off++] = (uint8_t)abilityCount;
    for (int i = 0; i < abilityCount; i++) {
        payload[off++] = (uint8_t)(int8_t)abilities[i].abilityId;
        payload[off++] = (uint8_t)abilities[i].level;
    }

    if (net_send_msg(sockfd, MSG_NFC_ABILITY_UPDATE, payload, off) < 0) {
        printf("[NFC] Failed to send ability update\n");
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}

//------------------------------------------------------------------------------------
// NFC ability reset (short-lived blocking TCP)
//------------------------------------------------------------------------------------
int net_nfc_reset_abilities(const char *host, int port, const uint8_t *uid, int uidLen)
{
    if (uidLen < 4 || uidLen > NFC_UID_MAX_LEN) return -1;

    int sockfd = net_shortlived_connect(host, port);
    if (sockfd < 0) {
        printf("[NFC] Failed to connect for ability reset\n");
        return -1;
    }

    uint8_t payload[1 + NFC_UID_MAX_LEN];
    payload[0] = (uint8_t)uidLen;
    memcpy(payload + 1, uid, uidLen);

    if (net_send_msg(sockfd, MSG_NFC_ABILITY_RESET, payload, 1 + uidLen) < 0) {
        printf("[NFC] Failed to send ability reset\n");
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}
