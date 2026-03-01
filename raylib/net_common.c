#include "net_common.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

//------------------------------------------------------------------------------------
// Low-level send/recv helpers (handle partial reads/writes)
//------------------------------------------------------------------------------------
static int send_all(int sockfd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(sockfd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(int sockfd, void *buf, int len)
{
    char *p = (char *)buf;
    int got = 0;
    while (got < len) {
        int n = recv(sockfd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

//------------------------------------------------------------------------------------
// Message send/recv
//------------------------------------------------------------------------------------
int net_send_msg(int sockfd, uint8_t type, const void *payload, uint16_t size)
{
    uint8_t header[NET_HEADER_SIZE];
    header[0] = (NET_MAGIC >> 8) & 0xFF;
    header[1] = NET_MAGIC & 0xFF;
    header[2] = type;
    header[3] = (size >> 8) & 0xFF;
    header[4] = size & 0xFF;
    if (send_all(sockfd, header, NET_HEADER_SIZE) < 0) return -1;
    if (size > 0 && payload) {
        if (send_all(sockfd, payload, size) < 0) return -1;
    }
    return 0;
}

int net_recv_msg(int sockfd, NetMessage *msg)
{
    uint8_t header[NET_HEADER_SIZE];
    if (recv_all(sockfd, header, NET_HEADER_SIZE) < 0) return -1;
    uint16_t magic = ((uint16_t)header[0] << 8) | header[1];
    if (magic != NET_MAGIC) return -1;
    msg->type = header[2];
    msg->size = ((uint16_t)header[3] << 8) | header[4];
    if (msg->size > NET_MAX_PAYLOAD) return -1;
    if (msg->size > 0) {
        if (recv_all(sockfd, msg->payload, msg->size) < 0) return -1;
    }
    return 0;
}

int net_recv_msg_nonblock(int sockfd, NetMessage *msg)
{
    // Peek at header first
    uint8_t header[NET_HEADER_SIZE];
    int n = recv(sockfd, header, NET_HEADER_SIZE, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return -1; // disconnected
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (n < NET_HEADER_SIZE) return 0; // partial header, wait

    uint16_t magic = ((uint16_t)header[0] << 8) | header[1];
    if (magic != NET_MAGIC) return -1;
    uint16_t size = ((uint16_t)header[3] << 8) | header[4];
    if (size > NET_MAX_PAYLOAD) return -1;

    // Check if full message is available
    int totalSize = NET_HEADER_SIZE + size;
    if (size > 0) {
        uint8_t checkBuf[NET_HEADER_SIZE + NET_MAX_PAYLOAD];
        n = recv(sockfd, checkBuf, totalSize, MSG_PEEK | MSG_DONTWAIT);
        if (n < totalSize) return 0; // not enough data yet
    }

    // Full message available — do blocking read to consume it
    if (recv_all(sockfd, header, NET_HEADER_SIZE) < 0) return -1;
    msg->type = header[2];
    msg->size = ((uint16_t)header[3] << 8) | header[4];
    if (msg->size > 0) {
        if (recv_all(sockfd, msg->payload, msg->size) < 0) return -1;
    }
    return 1;
}

//------------------------------------------------------------------------------------
// Unit serialization
//------------------------------------------------------------------------------------
int serialize_units(const Unit units[], int unitCount, NetUnit out[], int maxOut)
{
    int count = 0;
    for (int i = 0; i < unitCount && count < maxOut; i++) {
        if (!units[i].active) continue;
        NetUnit *nu = &out[count];
        nu->typeIndex = (uint8_t)units[i].typeIndex;
        nu->team = (uint8_t)units[i].team;
        nu->rarity = units[i].rarity;
        nu->posX = units[i].position.x;
        nu->posZ = units[i].position.z;
        nu->currentHealth = units[i].currentHealth;
        nu->facingAngle = units[i].facingAngle;
        for (int a = 0; a < 4; a++) {
            nu->abilities[a].abilityId = (int8_t)units[i].abilities[a].abilityId;
            nu->abilities[a].level = (uint8_t)units[i].abilities[a].level;
        }
        count++;
    }
    return count;
}

int deserialize_units(const NetUnit in[], int inCount, Unit units[], int maxUnits)
{
    int count = 0;
    for (int i = 0; i < inCount && count < maxUnits; i++) {
        const NetUnit *nu = &in[i];
        units[count] = (Unit){
            .typeIndex = nu->typeIndex,
            .position = (Vector3){ nu->posX, 0.0f, nu->posZ },
            .team = (Team)nu->team,
            .rarity = nu->rarity,
            .currentHealth = nu->currentHealth,
            .attackCooldown = 0.0f,
            .targetIndex = -1,
            .active = true,
            .selected = false,
            .dragging = false,
            .facingAngle = nu->facingAngle,
#ifndef SERVER_BUILD
            .currentAnim = ANIM_IDLE,
            .animFrame = 0,
#endif
            .scaleOverride = 1.0f,
            .speedMultiplier = 1.0f,
            .hpMultiplier = 1.0f,
            .dmgMultiplier = 1.0f,
            .shieldHP = 0.0f,
            .abilityCastDelay = 0.0f,
            .chargeTarget = -1,
        };
        for (int a = 0; a < 4; a++) {
            units[count].abilities[a] = (AbilitySlot){
                .abilityId = nu->abilities[a].abilityId,
                .level = nu->abilities[a].level,
                .cooldownRemaining = 0,
                .triggered = false,
            };
        }
        units[count].nextAbilitySlot = 0;
        count++;
    }
    return count;
}

//------------------------------------------------------------------------------------
// Shop serialization
//------------------------------------------------------------------------------------
int serialize_shop(const ShopSlot slots[], int count, uint8_t *buf, int bufSize)
{
    int needed = count * 2; // 1 byte abilityId + 1 byte level per slot
    if (needed > bufSize) return 0;
    for (int i = 0; i < count; i++) {
        buf[i * 2]     = (uint8_t)(slots[i].abilityId & 0xFF);
        buf[i * 2 + 1] = (uint8_t)(slots[i].level & 0xFF);
    }
    return needed;
}

int deserialize_shop(const uint8_t *buf, int bufSize, ShopSlot slots[], int maxSlots)
{
    int count = bufSize / 2;
    if (count > maxSlots) count = maxSlots;
    for (int i = 0; i < count; i++) {
        slots[i].abilityId = (int8_t)buf[i * 2];
        slots[i].level = buf[i * 2 + 1];
    }
    return count * 2;
}

//------------------------------------------------------------------------------------
// Socket utilities
//------------------------------------------------------------------------------------
void net_set_nonblocking(int sockfd)
{
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

//------------------------------------------------------------------------------------
// Short-lived blocking TCP connect (leaderboard, NFC, etc.)
//------------------------------------------------------------------------------------
int net_shortlived_connect(const char *host, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    struct hostent *he = gethostbyname(host);
    if (!he) { close(sockfd); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    // 3-second send/recv timeout
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return sockfd;
}
