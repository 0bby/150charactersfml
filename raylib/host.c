#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOGDI
  #define NOUSER
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close_socket closesocket
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <errno.h>
  #define close_socket close
#endif

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "host.h"
#include "net_protocol.h"
#include "net_common.h"
#include "game_session.h"

static volatile bool hostRunning = false;
static pthread_t hostThread;
static int listenSock = -1;

static void *host_thread_fn(void *arg)
{
    int port = *(int *)arg;

    GameSession session;
    bool sessionActive = false;
    int playerCount = 0;

    // Main server loop
    while (hostRunning) {
        // Accept new connections
        if (playerCount < 2) {
            struct sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            int clientSock = accept(listenSock, (struct sockaddr *)&clientAddr, &addrLen);
            if (clientSock >= 0) {
                // Set TCP_NODELAY
                int one = 1;
                setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

                // Set non-blocking for polling
                net_set_nonblocking(clientSock);

                // Read JOIN message (blocking briefly)
                NetMessage joinMsg;
                // Give client a moment to send JOIN — peek with timeout
                int attempts = 0;
                bool gotJoin = false;
                while (attempts < 100 && hostRunning) {  // ~1 second max
                    int r = net_recv_msg_nonblock(clientSock, &joinMsg);
                    if (r == 1 && joinMsg.type == MSG_JOIN) {
                        gotJoin = true;
                        break;
                    } else if (r < 0) {
                        break;
                    }
#ifdef _WIN32
                    Sleep(10);
#else
                    usleep(10000);
#endif
                    attempts++;
                }

                if (!gotJoin) {
                    close_socket(clientSock);
                    continue;
                }

                // Parse player name from JOIN payload
                char playerName[32] = {0};
                if (joinMsg.size > LOBBY_CODE_LEN) {
                    int nameLen = joinMsg.payload[LOBBY_CODE_LEN];
                    if (nameLen > 31) nameLen = 31;
                    if (joinMsg.size >= LOBBY_CODE_LEN + 1 + nameLen) {
                        memcpy(playerName, joinMsg.payload + LOBBY_CODE_LEN + 1, nameLen);
                        playerName[nameLen] = '\0';
                    }
                }

                if (playerCount == 0) {
                    session_init(&session, clientSock);
                    snprintf(session.players[0].name, sizeof(session.players[0].name), "%s", playerName);
                    sessionActive = true;
                    playerCount = 1;
                    printf("[Host] Player 0 connected: %s\n", playerName);
                } else if (playerCount == 1 && sessionActive) {
                    snprintf(session.players[1].name, sizeof(session.players[1].name), "%s", playerName);
                    session_add_player(&session, clientSock);
                    playerCount = 2;
                    printf("[Host] Player 1 connected: %s\n", playerName);
                }
            }
        }

        // Tick the session
        if (sessionActive) {
            // Poll messages from connected players
            for (int p = 0; p < 2; p++) {
                if (!session.players[p].connected) continue;
                NetMessage msg;
                int r = net_recv_msg_nonblock(session.players[p].sockfd, &msg);
                if (r == 1) {
                    session_handle_msg(&session, p, &msg);
                }
            }

            int dead = session_tick(&session, 1.0f / 60.0f);
            if (dead) {
                printf("[Host] Session ended\n");
                sessionActive = false;
                // Don't exit — host stays running for potential reconnect
            }
        }

        // Sleep ~16ms for ~60Hz tick rate
#ifdef _WIN32
        Sleep(16);
#else
        usleep(16000);
#endif
    }

    // Cleanup: close player sockets
    if (sessionActive) {
        for (int p = 0; p < 2; p++) {
            if (session.players[p].connected) {
                close_socket(session.players[p].sockfd);
            }
        }
    }

    (void)port;
    return NULL;
}

int host_start(int port)
{
    if (hostRunning) return 0;  // already running

    net_platform_init();

    // Create listening socket
    listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0) {
        printf("[Host] Failed to create socket\n");
        return -1;
    }

    // Allow address reuse
    int one = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenSock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[Host] Failed to bind port %d\n", port);
        close_socket(listenSock);
        listenSock = -1;
        return -1;
    }

    if (listen(listenSock, 4) < 0) {
        printf("[Host] Failed to listen\n");
        close_socket(listenSock);
        listenSock = -1;
        return -1;
    }

    // Set listening socket non-blocking so accept() doesn't block the thread
    net_set_nonblocking(listenSock);

    hostRunning = true;

    static int portArg;
    portArg = port;
    if (pthread_create(&hostThread, NULL, host_thread_fn, &portArg) != 0) {
        printf("[Host] Failed to create thread\n");
        hostRunning = false;
        close_socket(listenSock);
        listenSock = -1;
        return -1;
    }

    printf("[Host] Server started on port %d\n", port);
    return 0;
}

void host_stop(void)
{
    if (!hostRunning) return;

    hostRunning = false;
    pthread_join(hostThread, NULL);

    if (listenSock >= 0) {
        close_socket(listenSock);
        listenSock = -1;
    }

    printf("[Host] Server stopped\n");
}

bool host_is_running(void)
{
    return hostRunning;
}
