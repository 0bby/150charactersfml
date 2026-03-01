#include "game_session.h"
#include "../raylib/combat_sim.h"
#include "../raylib/helpers.h"
#include "../raylib/synergies.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

//------------------------------------------------------------------------------------
// Internal helpers
//------------------------------------------------------------------------------------
static void generate_lobby_code(char code[LOBBY_CODE_LEN + 1])
{
    const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no ambiguous chars
    for (int i = 0; i < LOBBY_CODE_LEN; i++)
        code[i] = chars[rand() % (sizeof(chars) - 1)];
    code[LOBBY_CODE_LEN] = '\0';
}

static void setup_pve_enemies(Unit combatUnits[], int *combatUnitCount,
                              const Unit playerUnits[], int playerUnitCount,
                              int waveIndex)
{
    // Copy player's blue army first (reset health to max)
    int count = 0;
    for (int i = 0; i < playerUnitCount && count < MAX_UNITS; i++) {
        if (!playerUnits[i].active) continue;
        combatUnits[count] = playerUnits[i];
        combatUnits[count].team = TEAM_BLUE;
        combatUnits[count].currentHealth = UNIT_STATS[playerUnits[i].typeIndex].health;
        combatUnits[count].targetIndex = -1;
        combatUnits[count].attackCooldown = 0;
        combatUnits[count].nextAbilitySlot = 0;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            combatUnits[count].abilities[a].cooldownRemaining = 0;
            combatUnits[count].abilities[a].triggered = false;
        }
        count++;
    }
    *combatUnitCount = count;

    // Use the solo wave system for red PVE enemies
    SpawnWave(combatUnits, combatUnitCount, waveIndex, 2);
}

static void setup_pvp_combat(Unit combatUnits[], int *combatUnitCount,
                             const Unit p0Units[], int p0Count,
                             const Unit p1Units[], int p1Count)
{
    int count = 0;

    // Player 0's army as blue (reset health to max)
    for (int i = 0; i < p0Count && count < MAX_UNITS; i++) {
        if (!p0Units[i].active) continue;
        combatUnits[count] = p0Units[i];
        combatUnits[count].team = TEAM_BLUE;
        combatUnits[count].currentHealth = UNIT_STATS[p0Units[i].typeIndex].health;
        combatUnits[count].targetIndex = -1;
        combatUnits[count].attackCooldown = 0;
        combatUnits[count].nextAbilitySlot = 0;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            combatUnits[count].abilities[a].cooldownRemaining = 0;
            combatUnits[count].abilities[a].triggered = false;
        }
        count++;
    }

    // Player 1's army as red (reflected about Z=0 halfway line, reset health to max)
    for (int i = 0; i < p1Count && count < MAX_UNITS; i++) {
        if (!p1Units[i].active) continue;
        combatUnits[count] = p1Units[i];
        combatUnits[count].team = TEAM_RED;
        combatUnits[count].currentHealth = UNIT_STATS[p1Units[i].typeIndex].health;
        combatUnits[count].position.z = -combatUnits[count].position.z;
        combatUnits[count].facingAngle = 180.0f - combatUnits[count].facingAngle;
        combatUnits[count].targetIndex = -1;
        combatUnits[count].attackCooldown = 0;
        combatUnits[count].nextAbilitySlot = 0;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            combatUnits[count].abilities[a].cooldownRemaining = 0;
            combatUnits[count].abilities[a].triggered = false;
        }
        count++;
    }

    *combatUnitCount = count;
}

// Send serialized combat units to a player (from their perspective)
static void send_combat_start(GameSession *s, int playerIdx,
                              const Unit units[], int unitCount)
{
    NetUnit netUnits[NET_MAX_UNITS];
    int count = serialize_units(units, unitCount, netUnits, NET_MAX_UNITS);
    uint8_t payload[2 + sizeof(NetUnit) * NET_MAX_UNITS];
    payload[0] = (uint8_t)s->currentRound;
    payload[1] = (uint8_t)count;
    memcpy(payload + 2, netUnits, count * sizeof(NetUnit));
    net_send_msg(s->players[playerIdx].sockfd, MSG_COMBAT_START,
                 payload, 2 + count * sizeof(NetUnit));
}

//------------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------------
void session_init(GameSession *s, int player0_sock)
{
    memset(s, 0, sizeof(*s));
    generate_lobby_code(s->lobbyCode);
    s->state = SESSION_WAITING;
    s->players[0].sockfd = player0_sock;
    s->players[0].connected = true;
    s->players[0].gold = 10;
    s->players[1].connected = false;
    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) {
        s->players[0].inventory[i].abilityId = -1;
        s->players[1].inventory[i].abilityId = -1;
    }
    for (int i = 0; i < MAX_SHOP_SLOTS; i++) {
        s->players[0].shop[i].abilityId = -1;
        s->players[1].shop[i].abilityId = -1;
    }

    // Send lobby code to player 0
    net_send_msg(player0_sock, MSG_LOBBY_CODE, s->lobbyCode, LOBBY_CODE_LEN);
    printf("[Session %s] Created, waiting for opponent\n", s->lobbyCode);
}

int session_add_player(GameSession *s, int player1_sock)
{
    if (s->state != SESSION_WAITING) return -1;
    s->players[1].sockfd = player1_sock;
    s->players[1].connected = true;
    s->players[1].gold = 10;

    // Send game start to both players with opponent name
    for (int p = 0; p < 2; p++) {
        int other = 1 - p;
        int oppNameLen = (int)strlen(s->players[other].name);
        if (oppNameLen > 31) oppNameLen = 31;
        uint8_t payload[3 + 32];
        payload[0] = (uint8_t)p;  // player slot
        payload[1] = 10;          // starting gold
        payload[2] = (uint8_t)oppNameLen;
        memcpy(payload + 3, s->players[other].name, oppNameLen);
        net_send_msg(s->players[p].sockfd, MSG_GAME_START, payload, 3 + oppNameLen);
    }

    printf("[Session %s] Both players connected, starting game\n", s->lobbyCode);
    session_start_prep(s);
    return 0;
}

void session_start_prep(GameSession *s)
{
    s->state = SESSION_PREP;
    s->players[0].ready = false;
    s->players[1].ready = false;
    s->prepTimer = PREP_TIMER;

    for (int p = 0; p < 2; p++) {
        if (!s->players[p].connected) continue;

        // Free shop roll
        RollShop(s->players[p].shop, &s->players[p].gold, 0);

        // Send prep start: round number, gold
        uint8_t payload[16];
        payload[0] = (uint8_t)s->currentRound;
        payload[1] = 0; // always PVP in multiplayer
        // Gold as 16-bit
        payload[2] = (s->players[p].gold >> 8) & 0xFF;
        payload[3] = s->players[p].gold & 0xFF;
        net_send_msg(s->players[p].sockfd, MSG_PREP_START, payload, 4);

        session_send_shop(s, p);
    }
}

void session_send_shop(GameSession *s, int playerIdx)
{
    uint8_t buf[MAX_SHOP_SLOTS * 2];
    int len = serialize_shop(s->players[playerIdx].shop, MAX_SHOP_SLOTS, buf, sizeof(buf));
    net_send_msg(s->players[playerIdx].sockfd, MSG_SHOP_ROLL_RESULT, buf, len);
}

void session_start_combat(GameSession *s)
{
    s->state = SESSION_COMBAT;
    memset(s->combatModifiers, 0, sizeof(s->combatModifiers));
    memset(s->combatProjectiles, 0, sizeof(s->combatProjectiles));
    memset(s->combatFissures, 0, sizeof(s->combatFissures));

    // All multiplayer rounds are PVP
    // Server simulates p0=blue vs p1=red
    setup_pvp_combat(s->combatUnits, &s->combatUnitCount,
                     s->players[0].units, s->players[0].unitCount,
                     s->players[1].units, s->players[1].unitCount);
    ApplySynergies(s->combatUnits, s->combatUnitCount);

    // Player 0 sees: their army (blue) vs p1 mirror (red)
    send_combat_start(s, 0, s->combatUnits, s->combatUnitCount);

    // Player 1 sees: their army (blue) vs p0 mirror (red)
    Unit p1View[MAX_UNITS];
    int p1ViewCount;
    setup_pvp_combat(p1View, &p1ViewCount,
                     s->players[1].units, s->players[1].unitCount,
                     s->players[0].units, s->players[0].unitCount);
    ApplySynergies(p1View, p1ViewCount);
    send_combat_start(s, 1, p1View, p1ViewCount);
}

void session_handle_msg(GameSession *s, int playerIdx, const NetMessage *msg)
{
    PlayerState *player = &s->players[playerIdx];

    switch (msg->type) {
    case MSG_READY: {
        if (s->state != SESSION_PREP) break;
        // Deserialize player's army from payload
        if (msg->size >= 1) {
            uint8_t unitCount = msg->payload[0];
            if (unitCount > 0 && msg->size >= 1 + unitCount * sizeof(NetUnit)) {
                const NetUnit *netUnits = (const NetUnit *)(msg->payload + 1);
                player->unitCount = deserialize_units(netUnits, unitCount,
                    player->units, MAX_UNITS);
            }
        }
        player->ready = true;
        printf("[Session %s] Player %d ready (%d units)\n",
               s->lobbyCode, playerIdx, player->unitCount);

        // Notify other player
        int other = 1 - playerIdx;
        if (s->players[other].connected)
            net_send_msg(s->players[other].sockfd, MSG_OPPONENT_READY, NULL, 0);

        // Both ready? Start combat
        if (s->players[0].ready && s->players[1].ready)
            session_start_combat(s);
    } break;

    case MSG_ROLL_SHOP: {
        if (s->state != SESSION_PREP) break;
        int rollCost = 2;
        if (player->gold >= rollCost) {
            RollShop(player->shop, &player->gold, rollCost);
            session_send_shop(s, playerIdx);
            // Send gold update
            uint8_t goldBuf[2] = { (player->gold >> 8) & 0xFF, player->gold & 0xFF };
            net_send_msg(player->sockfd, MSG_GOLD_UPDATE, goldBuf, 2);
        }
    } break;

    case MSG_BUY_ABILITY: {
        if (s->state != SESSION_PREP) break;
        if (msg->size < 1) break;
        int slotIdx = msg->payload[0];
        if (slotIdx < 0 || slotIdx >= MAX_SHOP_SLOTS) break;
        BuyAbility(&player->shop[slotIdx], player->inventory,
                   player->units, player->unitCount, &player->gold);
        // Send updated shop and gold
        session_send_shop(s, playerIdx);
        uint8_t goldBuf[2] = { (player->gold >> 8) & 0xFF, player->gold & 0xFF };
        net_send_msg(player->sockfd, MSG_GOLD_UPDATE, goldBuf, 2);
    } break;

    case MSG_PLACE_UNIT: {
        if (s->state != SESSION_PREP) break;
        if (msg->size < 9) break; // 1 byte type + 4 bytes posX + 4 bytes posZ
        int typeIdx = msg->payload[0];
        float posX, posZ;
        memcpy(&posX, msg->payload + 1, 4);
        memcpy(&posZ, msg->payload + 5, 4);
        if (SpawnUnit(player->units, &player->unitCount, typeIdx, TEAM_BLUE)) {
            player->units[player->unitCount - 1].position.x = posX;
            player->units[player->unitCount - 1].position.z = posZ;
        }
    } break;

    case MSG_REMOVE_UNIT: {
        if (s->state != SESSION_PREP) break;
        if (msg->size < 1) break;
        int unitIdx = msg->payload[0];
        if (unitIdx >= 0 && unitIdx < player->unitCount && player->units[unitIdx].active) {
            // Return abilities to inventory
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                if (player->units[unitIdx].abilities[a].abilityId < 0) continue;
                for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
                    if (player->inventory[inv].abilityId < 0) {
                        player->inventory[inv].abilityId = player->units[unitIdx].abilities[a].abilityId;
                        player->inventory[inv].level = player->units[unitIdx].abilities[a].level;
                        break;
                    }
                }
            }
            player->units[unitIdx].active = false;
        }
    } break;

    case MSG_ASSIGN_ABILITY: {
        if (s->state != SESSION_PREP) break;
        if (msg->size < 3) break;
        int invSlot = msg->payload[0];
        int unitIdx = msg->payload[1];
        int abilSlot = msg->payload[2];
        if (invSlot < 0 || invSlot >= MAX_INVENTORY_SLOTS) break;
        if (unitIdx < 0 || unitIdx >= player->unitCount) break;
        if (abilSlot < 0 || abilSlot >= MAX_ABILITIES_PER_UNIT) break;
        if (player->inventory[invSlot].abilityId < 0) break;

        // Swap
        int oldId = player->units[unitIdx].abilities[abilSlot].abilityId;
        int oldLv = player->units[unitIdx].abilities[abilSlot].level;
        player->units[unitIdx].abilities[abilSlot].abilityId = player->inventory[invSlot].abilityId;
        player->units[unitIdx].abilities[abilSlot].level = player->inventory[invSlot].level;
        player->inventory[invSlot].abilityId = oldId;
        player->inventory[invSlot].level = oldLv;
    } break;

    default:
        printf("[Session %s] Unknown msg type 0x%02X from player %d\n",
               s->lobbyCode, msg->type, playerIdx);
        break;
    }
}

int session_tick(GameSession *s, float dt)
{
    // Check for disconnects
    for (int p = 0; p < 2; p++) {
        if (!s->players[p].connected) continue;
        // Quick check if socket is still alive
        char peek;
        int r = recv(s->players[p].sockfd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) {
            // Disconnected
            printf("[Session %s] Player %d disconnected\n", s->lobbyCode, p);
            s->players[p].connected = false;
            close(s->players[p].sockfd);
            // Notify other player they win
            int other = 1 - p;
            if (s->players[other].connected) {
                uint8_t payload[5];
                payload[0] = (uint8_t)other; // winner
                payload[1] = (s->pvpWins[0] >> 8) & 0xFF;
                payload[2] = s->pvpWins[0] & 0xFF;
                payload[3] = (s->pvpWins[1] >> 8) & 0xFF;
                payload[4] = s->pvpWins[1] & 0xFF;
                net_send_msg(s->players[other].sockfd, MSG_GAME_OVER, payload, 5);
            }
            s->state = SESSION_DEAD;
            return 1;
        }
    }

    switch (s->state) {
    case SESSION_PREP: {
        s->prepTimer -= dt;
        if (s->prepTimer <= 0) {
            // Auto-ready players who haven't pressed ready
            for (int p = 0; p < 2; p++) {
                if (!s->players[p].ready) {
                    s->players[p].ready = true;
                    printf("[Session %s] Player %d auto-readied (timer expired)\n",
                           s->lobbyCode, p);
                }
            }
            session_start_combat(s);
        }

        // Poll for messages from both players
        for (int p = 0; p < 2; p++) {
            if (!s->players[p].connected) continue;
            NetMessage msg;
            int r = net_recv_msg_nonblock(s->players[p].sockfd, &msg);
            if (r == 1) session_handle_msg(s, p, &msg);
            else if (r < 0) {
                s->players[p].connected = false;
                close(s->players[p].sockfd);
            }
        }
    } break;

    case SESSION_COMBAT: {
        // Run headless combat simulation
        int result = CombatTick(s->combatUnits, s->combatUnitCount,
                                s->combatModifiers, s->combatProjectiles,
                                s->combatFissures, COMBAT_DT, NULL, NULL);
        if (result > 0) {
            int winner = -1; // -1 = draw
            if (result == 1) winner = 0;       // blue wins = player 0
            else if (result == 2) winner = 1;  // red wins = player 1

            if (winner >= 0) {
                s->pvpWins[winner]++;
            }

            s->currentRound++;

            // Send round result to both players
            for (int p = 0; p < 2; p++) {
                if (!s->players[p].connected) continue;
                uint8_t payload[6];
                // PVP: player p is always "blue" from their view
                if (winner == p) payload[0] = 0;
                else if (winner == (1-p)) payload[0] = 1;
                else payload[0] = 2;
                payload[1] = 0; // always PVP
                payload[2] = (uint8_t)s->pvpWins[0];
                payload[3] = (uint8_t)s->pvpWins[1];
                payload[4] = (uint8_t)s->currentRound;
                net_send_msg(s->players[p].sockfd, MSG_ROUND_RESULT, payload, 5);
            }

            // Check game over
            if (s->pvpWins[0] >= MAX_PVP_WINS || s->pvpWins[1] >= MAX_PVP_WINS) {
                int gameWinner = (s->pvpWins[0] >= MAX_PVP_WINS) ? 0 : 1;
                for (int p = 0; p < 2; p++) {
                    if (!s->players[p].connected) continue;
                    uint8_t payload[5];
                    payload[0] = (gameWinner == p) ? 0 : 1; // 0=you win, 1=you lose
                    payload[1] = (uint8_t)s->pvpWins[0];
                    payload[2] = (uint8_t)s->pvpWins[1];
                    net_send_msg(s->players[p].sockfd, MSG_GAME_OVER, payload, 3);
                }
                s->state = SESSION_DEAD;
                return 1;
            }

            // Give gold and move to next prep
            int bonusGold = 5;
            for (int p = 0; p < 2; p++) {
                s->players[p].gold += bonusGold;
            }

            session_start_prep(s);
        }
    } break;

    case SESSION_WAITING:
        // Poll for messages from player 0 (they might disconnect)
        if (s->players[0].connected) {
            NetMessage msg;
            int r = net_recv_msg_nonblock(s->players[0].sockfd, &msg);
            if (r < 0) {
                s->players[0].connected = false;
                close(s->players[0].sockfd);
                s->state = SESSION_DEAD;
                return 1;
            }
        }
        break;

    default:
        break;
    }

    return 0;
}
