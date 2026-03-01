#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

#include "../raylib/net_protocol.h"
#include "../raylib/net_common.h"
#include "../raylib/leaderboard.h"
#include "nfc_store.h"
#include "game_session.h"

//------------------------------------------------------------------------------------
// Server Configuration
//------------------------------------------------------------------------------------
#define MAX_SESSIONS 16
#define SERVER_TICK_RATE 60  // ticks per second
#define TICK_INTERVAL_US (1000000 / SERVER_TICK_RATE)

static volatile int running = 1;
static void sigint_handler(int sig) { (void)sig; running = 0; }

//------------------------------------------------------------------------------------
// Global leaderboard
//------------------------------------------------------------------------------------
#define GLOBAL_LEADERBOARD_FILE "global_leaderboard.json"
static Leaderboard globalLeaderboard;

//------------------------------------------------------------------------------------
// Global NFC tag store
//------------------------------------------------------------------------------------
#define NFC_TAGS_FILE "nfc_tags.json"
static NfcStore nfcStore;

static void send_leaderboard_data(int sockfd)
{
    // Payload: [entryCount:1][entries × 55 bytes]
    uint8_t payload[1 + MAX_LEADERBOARD_ENTRIES * LEADERBOARD_ENTRY_NET_SIZE];
    int count = globalLeaderboard.entryCount;
    payload[0] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        serialize_leaderboard_entry(&globalLeaderboard.entries[i],
            payload + 1 + i * LEADERBOARD_ENTRY_NET_SIZE,
            LEADERBOARD_ENTRY_NET_SIZE);
    }
    net_send_msg(sockfd, MSG_LEADERBOARD_DATA, payload, 1 + count * LEADERBOARD_ENTRY_NET_SIZE);
}

//------------------------------------------------------------------------------------
// Session management
//------------------------------------------------------------------------------------
static GameSession sessions[MAX_SESSIONS];
static int sessionCount = 0;

static GameSession *find_session_by_code(const char *code)
{
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i].state == SESSION_WAITING &&
            strncmp(sessions[i].lobbyCode, code, LOBBY_CODE_LEN) == 0)
            return &sessions[i];
    }
    return NULL;
}

static GameSession *create_session(int sockfd)
{
    // Reuse dead slots
    for (int i = 0; i < sessionCount; i++) {
        if (sessions[i].state == SESSION_DEAD) {
            session_init(&sessions[i], sockfd);
            return &sessions[i];
        }
    }
    if (sessionCount >= MAX_SESSIONS) return NULL;
    session_init(&sessions[sessionCount], sockfd);
    return &sessions[sessionCount++];
}

//------------------------------------------------------------------------------------
// Handle new client connection
//------------------------------------------------------------------------------------
static void handle_new_client(int clientfd, struct sockaddr_in *addr)
{
    printf("[Server] New connection from %s:%d (fd=%d)\n",
           inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), clientfd);

    // Set TCP_NODELAY for low latency
    int one = 1;
    setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    net_set_nonblocking(clientfd);

    // Wait for first message (with short timeout via blocking peek)
    // Temporarily set blocking for the handshake
    int flags = fcntl(clientfd, F_GETFL, 0);
    fcntl(clientfd, F_SETFL, flags & ~O_NONBLOCK);

    // Set a 5-second timeout for the first message
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    NetMessage msg;
    if (net_recv_msg(clientfd, &msg) < 0) {
        printf("[Server] Client fd=%d didn't send valid message, closing\n", clientfd);
        close(clientfd);
        return;
    }

    // Handle leaderboard messages (stateless, short-lived connections)
    if (msg.type == MSG_LEADERBOARD_SUBMIT) {
        LeaderboardEntry entry;
        if (msg.size >= LEADERBOARD_ENTRY_NET_SIZE &&
            deserialize_leaderboard_entry(msg.payload, msg.size, &entry) > 0) {
            InsertLeaderboardEntry(&globalLeaderboard, &entry);
            SaveLeaderboard(&globalLeaderboard, GLOBAL_LEADERBOARD_FILE);
            printf("[Server] Leaderboard submit from '%s' (round %d), total=%d\n",
                   entry.playerName, entry.highestRound, globalLeaderboard.entryCount);
            send_leaderboard_data(clientfd);
        }
        close(clientfd);
        return;
    }

    if (msg.type == MSG_LEADERBOARD_REQUEST) {
        printf("[Server] Leaderboard request, sending %d entries\n", globalLeaderboard.entryCount);
        send_leaderboard_data(clientfd);
        close(clientfd);
        return;
    }

    // Handle NFC messages (stateless, short-lived connections)
    if (msg.type == MSG_NFC_LOOKUP) {
        if (msg.size >= 1) {
            uint8_t uidLen = msg.payload[0];
            if (uidLen >= 4 && uidLen <= NFC_UID_MAX_LEN && msg.size >= 1 + uidLen) {
                // Convert binary UID to hex string
                char uidHex[NFC_UID_HEX_MAX] = {0};
                for (int i = 0; i < uidLen; i++)
                    sprintf(uidHex + i * 2, "%02X", msg.payload[1 + i]);

                NfcTagEntry *entry = NfcStoreLookup(&nfcStore, uidHex);
                // Response: [uidLen:1][uid:N][status:1][typeIndex:1][rarity:1][abilities × 4 × (id:1, level:1)]
                uint8_t resp[1 + NFC_UID_MAX_LEN + 3 + NFC_MAX_ABILITIES * 2];
                resp[0] = uidLen;
                memcpy(resp + 1, msg.payload + 1, uidLen);
                int off = 1 + uidLen;
                if (entry) {
                    resp[off++] = NFC_STATUS_OK;
                    resp[off++] = entry->typeIndex;
                    resp[off++] = entry->rarity;
                    for (int a = 0; a < NFC_MAX_ABILITIES; a++) {
                        resp[off++] = (uint8_t)(int8_t)entry->abilities[a].abilityId;
                        resp[off++] = entry->abilities[a].level;
                    }
                    printf("[Server] NFC lookup %s -> type=%d rarity=%d\n", uidHex, entry->typeIndex, entry->rarity);
                } else {
                    resp[off++] = NFC_STATUS_NOT_FOUND;
                    resp[off++] = 0;
                    resp[off++] = 0;
                    for (int a = 0; a < NFC_MAX_ABILITIES; a++) {
                        resp[off++] = 0xFF; // -1 as uint8_t
                        resp[off++] = 0;
                    }
                    printf("[Server] NFC lookup %s -> not found\n", uidHex);
                }
                net_send_msg(clientfd, MSG_NFC_DATA, resp, off);
            }
        }
        close(clientfd);
        return;
    }

    if (msg.type == MSG_NFC_REGISTER) {
        if (msg.size >= 3) {
            uint8_t uidLen = msg.payload[0];
            if (uidLen >= 4 && uidLen <= NFC_UID_MAX_LEN && msg.size >= 1 + uidLen + 2) {
                uint8_t typeIndex = msg.payload[1 + uidLen];
                uint8_t rarity = msg.payload[2 + uidLen];

                char uidHex[NFC_UID_HEX_MAX] = {0};
                for (int i = 0; i < uidLen; i++)
                    sprintf(uidHex + i * 2, "%02X", msg.payload[1 + i]);

                int result = NfcStoreRegister(&nfcStore, uidHex, typeIndex, rarity);
                NfcStoreSave(&nfcStore, NFC_TAGS_FILE);

                uint8_t resp[1 + NFC_UID_MAX_LEN + 3];
                resp[0] = uidLen;
                memcpy(resp + 1, msg.payload + 1, uidLen);
                resp[1 + uidLen] = (result >= 0) ? NFC_STATUS_OK : NFC_STATUS_ERROR;
                resp[2 + uidLen] = typeIndex;
                resp[3 + uidLen] = rarity;

                const char *action = (result == 1) ? "updated" : (result == 0) ? "registered" : "FAILED (store full)";
                printf("[Server] NFC register %s type=%d rarity=%d -> %s\n", uidHex, typeIndex, rarity, action);
                net_send_msg(clientfd, MSG_NFC_DATA, resp, 1 + uidLen + 3);
            }
        }
        close(clientfd);
        return;
    }

    if (msg.type == MSG_NFC_ABILITY_UPDATE) {
        if (msg.size >= 2) {
            uint8_t uidLen = msg.payload[0];
            if (uidLen >= 4 && uidLen <= NFC_UID_MAX_LEN && msg.size >= 1 + uidLen + 1) {
                char uidHex[NFC_UID_HEX_MAX] = {0};
                for (int i = 0; i < uidLen; i++)
                    sprintf(uidHex + i * 2, "%02X", msg.payload[1 + i]);

                int abCount = msg.payload[1 + uidLen];
                if (abCount > NFC_MAX_ABILITIES) abCount = NFC_MAX_ABILITIES;

                NfcAbility abilities[NFC_MAX_ABILITIES];
                for (int a = 0; a < NFC_MAX_ABILITIES; a++) {
                    abilities[a].abilityId = -1;
                    abilities[a].level = 0;
                }
                int off = 2 + uidLen;
                for (int a = 0; a < abCount && off + 1 < msg.size; a++) {
                    abilities[a].abilityId = (int8_t)msg.payload[off++];
                    abilities[a].level = msg.payload[off++];
                }

                int result = NfcStoreUpdateAbilities(&nfcStore, uidHex, abilities, abCount);
                if (result == 0) {
                    NfcStoreSave(&nfcStore, NFC_TAGS_FILE);
                    printf("[Server] NFC ability update %s -> %d abilities\n", uidHex, abCount);
                } else {
                    printf("[Server] NFC ability update %s -> tag not found\n", uidHex);
                }
            }
        }
        close(clientfd);
        return;
    }

    if (msg.type == MSG_NFC_ABILITY_RESET) {
        if (msg.size >= 1) {
            uint8_t uidLen = msg.payload[0];
            if (uidLen >= 4 && uidLen <= NFC_UID_MAX_LEN && msg.size >= 1 + uidLen) {
                char uidHex[NFC_UID_HEX_MAX] = {0};
                for (int i = 0; i < uidLen; i++)
                    sprintf(uidHex + i * 2, "%02X", msg.payload[1 + i]);

                int result = NfcStoreResetAbilities(&nfcStore, uidHex);
                if (result == 0) {
                    NfcStoreSave(&nfcStore, NFC_TAGS_FILE);
                    printf("[Server] NFC ability reset %s -> ok\n", uidHex);
                } else {
                    printf("[Server] NFC ability reset %s -> tag not found\n", uidHex);
                }
            }
        }
        close(clientfd);
        return;
    }

    if (msg.type != MSG_JOIN) {
        printf("[Server] Client fd=%d sent unexpected msg type 0x%02X, closing\n", clientfd, msg.type);
        close(clientfd);
        return;
    }

    // Reset timeout and set non-blocking
    tv.tv_sec = 0; tv.tv_usec = 0;
    setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    net_set_nonblocking(clientfd);

    // Extract player name from JOIN payload: [lobbyCode:4][nameLen:1][name:N]
    char playerName[32] = {0};
    if (msg.size >= LOBBY_CODE_LEN + 1) {
        int nameLen = msg.payload[LOBBY_CODE_LEN];
        if (nameLen > 15) nameLen = 15;
        if (msg.size >= LOBBY_CODE_LEN + 1 + nameLen) {
            memcpy(playerName, msg.payload + LOBBY_CODE_LEN + 1, nameLen);
            playerName[nameLen] = '\0';
        }
    }
    if (!playerName[0]) strncpy(playerName, "Player", sizeof(playerName) - 1);

    // Check if joining existing lobby or creating new one
    char code[LOBBY_CODE_LEN + 1] = {0};
    if (msg.size >= LOBBY_CODE_LEN) {
        memcpy(code, msg.payload, LOBBY_CODE_LEN);
        code[LOBBY_CODE_LEN] = '\0';
    }

    bool isJoin = (code[0] != '\0' && code[0] != '0');

    if (isJoin) {
        GameSession *s = find_session_by_code(code);
        if (s) {
            strncpy(s->players[1].name, playerName, sizeof(s->players[1].name) - 1);
            session_add_player(s, clientfd);
            printf("[Server] Player '%s' joined lobby %s\n", playerName, code);
        } else {
            const char *err = "Lobby not found";
            net_send_msg(clientfd, MSG_ERROR, err, strlen(err));
            close(clientfd);
            printf("[Server] Lobby %s not found\n", code);
        }
    } else {
        GameSession *s = create_session(clientfd);
        if (s) {
            strncpy(s->players[0].name, playerName, sizeof(s->players[0].name) - 1);
            printf("[Server] Player '%s' created lobby %s\n", playerName, s->lobbyCode);
        } else {
            const char *err = "Server full";
            net_send_msg(clientfd, MSG_ERROR, err, strlen(err));
            close(clientfd);
            printf("[Server] Cannot create session — server full\n");
        }
    }
}

//------------------------------------------------------------------------------------
// Main server loop
//------------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    int port = NET_PORT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL));

    // Load global leaderboard
    LoadLeaderboard(&globalLeaderboard, GLOBAL_LEADERBOARD_FILE);
    printf("Loaded %d leaderboard entries from %s\n", globalLeaderboard.entryCount, GLOBAL_LEADERBOARD_FILE);

    // Load NFC tag store
    NfcStoreLoad(&nfcStore, NFC_TAGS_FILE);
    printf("Loaded %d NFC tags from %s\n", nfcStore.tagCount, NFC_TAGS_FILE);

    // Create listening socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in servaddr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port),
    };

    if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind"); close(listenfd); return 1;
    }
    if (listen(listenfd, 16) < 0) {
        perror("listen"); close(listenfd); return 1;
    }

    net_set_nonblocking(listenfd);

    printf("=== Autochess Multiplayer Server ===\n");
    printf("Listening on port %d\n", port);
    printf("Press Ctrl+C to stop\n\n");

    struct timespec lastTick;
    clock_gettime(CLOCK_MONOTONIC, &lastTick);

    while (running) {
        // Accept new connections (non-blocking)
        struct sockaddr_in clientaddr;
        socklen_t addrlen = sizeof(clientaddr);
        int clientfd = accept(listenfd, (struct sockaddr *)&clientaddr, &addrlen);
        if (clientfd >= 0) {
            handle_new_client(clientfd, &clientaddr);
        }

        // Calculate dt
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (now.tv_sec - lastTick.tv_sec) +
                   (now.tv_nsec - lastTick.tv_nsec) / 1e9f;
        lastTick = now;

        // Tick all sessions
        for (int i = 0; i < sessionCount; i++) {
            if (sessions[i].state == SESSION_DEAD) continue;
            session_tick(&sessions[i], dt);
        }

        // Sleep to maintain tick rate
        usleep(TICK_INTERVAL_US);
    }

    printf("\n[Server] Shutting down...\n");
    SaveLeaderboard(&globalLeaderboard, GLOBAL_LEADERBOARD_FILE);
    printf("[Server] Saved %d leaderboard entries\n", globalLeaderboard.entryCount);
    NfcStoreSave(&nfcStore, NFC_TAGS_FILE);
    printf("[Server] Saved %d NFC tags\n", nfcStore.tagCount);

    // Close all sessions
    for (int i = 0; i < sessionCount; i++) {
        for (int p = 0; p < 2; p++) {
            if (sessions[i].players[p].connected) {
                close(sessions[i].players[p].sockfd);
            }
        }
    }
    close(listenfd);

    return 0;
}
