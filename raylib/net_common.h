#pragma once
#include "net_protocol.h"
#include "game.h"

// Send/receive a complete message over a TCP socket.
// Returns 0 on success, -1 on error/disconnect.
int net_send_msg(int sockfd, uint8_t type, const void *payload, uint16_t size);
int net_recv_msg(int sockfd, NetMessage *msg);

// Non-blocking receive: returns 1 if a message was read, 0 if nothing available, -1 on error.
int net_recv_msg_nonblock(int sockfd, NetMessage *msg);

// Serialize local units into NetUnit array for transmission.
// Returns number of units written.
int serialize_units(const Unit units[], int unitCount, NetUnit out[], int maxOut);

// Deserialize NetUnit array into local Unit array.
// Returns number of units written.
int deserialize_units(const NetUnit in[], int inCount, Unit units[], int maxUnits);

// Serialize shop slots into buffer. Returns bytes written.
int serialize_shop(const ShopSlot slots[], int count, uint8_t *buf, int bufSize);

// Deserialize shop slots from buffer. Returns bytes consumed.
int deserialize_shop(const uint8_t *buf, int bufSize, ShopSlot slots[], int maxSlots);

// Set socket to non-blocking mode.
void net_set_nonblocking(int sockfd);

// Short-lived blocking TCP connect with 3-second timeout (for leaderboard, NFC, etc.)
// Returns socket fd on success, -1 on error.
int net_shortlived_connect(const char *host, int port);
