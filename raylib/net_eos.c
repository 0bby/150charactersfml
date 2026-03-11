#ifdef USE_EOS
//------------------------------------------------------------------------------------
// EOS P2P Transport — relay-based multiplayer with Device ID auth
//
// Host runs GameSession via socketpair loopback (game_session.c unchanged).
// Joiner receives P2P messages and updates EosClient flags directly.
//------------------------------------------------------------------------------------

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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "net_eos.h"
#include "net_protocol.h"
#include "net_common.h"
#include "game_session.h"

#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_p2p.h>
#include <eos_lobby.h>
#include <eos_logging.h>

//------------------------------------------------------------------------------------
// EOS Credentials — safe to embed (Device ID is a public client flow)
// Replace these with your values from the Epic Dev Portal.
//------------------------------------------------------------------------------------
#ifndef EOS_PRODUCT_ID
  #define EOS_PRODUCT_ID      "74897b69f1c844319a966068a06e1248"
#endif
#ifndef EOS_SANDBOX_ID
  #define EOS_SANDBOX_ID      "b984292f2fa44134874b3f27a95f3dc1"
#endif
#ifndef EOS_DEPLOYMENT_ID
  #define EOS_DEPLOYMENT_ID   "963bdb9cdc8f4035adc0fc201e4a9134"
#endif
#ifndef EOS_CLIENT_ID
  #define EOS_CLIENT_ID       "xyza7891HUfCzUTRbmf9HghshVoFKrum"
#endif
#ifndef EOS_CLIENT_SECRET
  #define EOS_CLIENT_SECRET   "SksbbTK6RtGiaxsJFt6c18T9btaPLaINktxZchPlIgk"
#endif

#define EOS_SOCKET_NAME "RelicRivals"
#define EOS_P2P_CHANNEL 0
#define LOBBY_ATTR_CODE "LOBBY_CODE"

//------------------------------------------------------------------------------------
// Module state
//------------------------------------------------------------------------------------
static EOS_HPlatform         g_eosPlatform   = NULL;
static EOS_HConnect          g_eosConnect    = NULL;
static EOS_HP2P              g_eosP2P        = NULL;
static EOS_HLobby            g_eosLobby      = NULL;
static EOS_ProductUserId     g_localUserId   = NULL;
static bool                  g_eosLoggedIn   = false;
static bool                  g_eosInitialized = false;
static bool                  g_eosAltInstance = false;
static bool                  g_eosAltDeleteDone = false;

// Host state
static GameSession           g_hostSession;
static bool                  g_hostSessionActive = false;
static double                g_hostTickAccum = 0.0;  // accumulated time for decoupled tick
static double                g_hostLastTime = 0.0;   // last timestamp for decoupled tick
static int                   g_hostLocalSock[2]  = {-1, -1};  // host's own read/write pair
static int                   g_hostRemoteSock[2] = {-1, -1};  // remote player's read/write pair
static EOS_ProductUserId     g_hostRemoteUserId  = NULL;
static bool                  g_hostRemoteConnected = false;
static char                  g_hostPlayerName[32] = {0};
static char                  g_joinPlayerName[32] = {0};

// Joiner state
static EOS_ProductUserId     g_joinerHostUserId = NULL;
static EosClient            *g_activeEosClient = NULL; // set during lobby, used for disconnect callback

// Connection notification IDs
static EOS_NotificationId    g_peerConnReqNotif  = EOS_INVALID_NOTIFICATIONID;
static EOS_NotificationId    g_peerConnCloseNotif = EOS_INVALID_NOTIFICATIONID;

// Lobby ID for cleanup
static char                  g_currentLobbyId[256] = {0};

//------------------------------------------------------------------------------------
// Logging callback
//------------------------------------------------------------------------------------
static void EOS_CALL eos_log_cb(const EOS_LogMessage *msg)
{
    printf("[EOS %s] %s\n", msg->Category, msg->Message);
}

//------------------------------------------------------------------------------------
// Windows socketpair emulation via localhost TCP loopback
//------------------------------------------------------------------------------------
static int make_socketpair(int sv[2])
{
#ifdef _WIN32
    // Emulate socketpair with localhost TCP
    int listener = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // auto-assign

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_socket(listener);
        return -1;
    }

    int addrLen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *)&addr, &addrLen) < 0) {
        close_socket(listener);
        return -1;
    }

    if (listen(listener, 1) < 0) {
        close_socket(listener);
        return -1;
    }

    int connector = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (connector < 0) {
        close_socket(listener);
        return -1;
    }

    if (connect(connector, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_socket(connector);
        close_socket(listener);
        return -1;
    }

    int acceptor = (int)accept(listener, NULL, NULL);
    close_socket(listener);
    if (acceptor < 0) {
        close_socket(connector);
        return -1;
    }

    // Disable Nagle
    int one = 1;
    setsockopt(connector, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(acceptor, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    sv[0] = acceptor;   // session reads/writes this end
    sv[1] = connector;  // EOS host reads/writes this end
    return 0;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    int one = 1;
    setsockopt(sv[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sv[1], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
#endif
}

//------------------------------------------------------------------------------------
// EOS P2P helpers
//------------------------------------------------------------------------------------
static EOS_P2P_SocketId make_socket_id(void)
{
    EOS_P2P_SocketId sid;
    memset(&sid, 0, sizeof(sid));
    sid.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
    strncpy(sid.SocketName, EOS_SOCKET_NAME, sizeof(sid.SocketName) - 1);
    return sid;
}

static int eos_p2p_send(EOS_ProductUserId remotePeer,
                         const void *data, uint32_t dataLen)
{
    EOS_P2P_SocketId sid = make_socket_id();
    EOS_P2P_SendPacketOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
    opts.LocalUserId = g_localUserId;
    opts.RemoteUserId = remotePeer;
    opts.SocketId = &sid;
    opts.Channel = EOS_P2P_CHANNEL;
    opts.DataLengthBytes = dataLen;
    opts.Data = data;
    opts.bAllowDelayedDelivery = EOS_TRUE;
    opts.Reliability = EOS_PR_ReliableOrdered;

    EOS_EResult r = EOS_P2P_SendPacket(g_eosP2P, &opts);
    if (r != EOS_Success) {
        printf("[EOS] SendPacket failed: %s\n", EOS_EResult_ToString(r));
        return -1;
    }
    return 0;
}

// Send a framed message (same wire format as TCP) over EOS P2P
static int eos_send_msg(EOS_ProductUserId remotePeer,
                         uint8_t type, const void *payload, uint16_t size)
{
    uint8_t buf[NET_HEADER_SIZE + NET_MAX_PAYLOAD];
    buf[0] = (NET_MAGIC >> 8) & 0xFF;
    buf[1] = NET_MAGIC & 0xFF;
    buf[2] = type;
    buf[3] = (size >> 8) & 0xFF;
    buf[4] = size & 0xFF;
    if (size > 0 && payload)
        memcpy(buf + NET_HEADER_SIZE, payload, size);
    return eos_p2p_send(remotePeer, buf, NET_HEADER_SIZE + size);
}

// Receive one P2P packet. Returns 1 if got a message, 0 if none, -1 on error.
static int eos_recv_msg(EOS_ProductUserId *outSender, NetMessage *msg)
{
    // Check if packet available
    EOS_P2P_GetNextReceivedPacketSizeOptions sizeOpts;
    memset(&sizeOpts, 0, sizeof(sizeOpts));
    sizeOpts.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
    sizeOpts.LocalUserId = g_localUserId;
    sizeOpts.RequestedChannel = &(uint8_t){EOS_P2P_CHANNEL};

    uint32_t packetSize = 0;
    EOS_EResult r = EOS_P2P_GetNextReceivedPacketSize(g_eosP2P, &sizeOpts, &packetSize);
    if (r != EOS_Success || packetSize == 0) return 0;
    if (packetSize < NET_HEADER_SIZE) return -1;

    uint8_t buf[NET_HEADER_SIZE + NET_MAX_PAYLOAD];
    EOS_P2P_ReceivePacketOptions recvOpts;
    memset(&recvOpts, 0, sizeof(recvOpts));
    recvOpts.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
    recvOpts.LocalUserId = g_localUserId;
    recvOpts.MaxDataSizeBytes = sizeof(buf);
    recvOpts.RequestedChannel = &(uint8_t){EOS_P2P_CHANNEL};

    EOS_ProductUserId sender = NULL;
    EOS_P2P_SocketId recvSid;
    uint8_t channel = 0;
    uint32_t bytesRead = 0;
    r = EOS_P2P_ReceivePacket(g_eosP2P, &recvOpts, &sender, &recvSid, &channel,
                               buf, &bytesRead);
    if (r != EOS_Success) return 0;

    // Validate header
    uint16_t magic = ((uint16_t)buf[0] << 8) | buf[1];
    if (magic != NET_MAGIC) return -1;

    msg->type = buf[2];
    msg->size = ((uint16_t)buf[3] << 8) | buf[4];
    if (msg->size > NET_MAX_PAYLOAD) return -1;
    if (bytesRead < (uint32_t)(NET_HEADER_SIZE + msg->size)) return -1;

    if (msg->size > 0)
        memcpy(msg->payload, buf + NET_HEADER_SIZE, msg->size);

    if (outSender) *outSender = sender;
    return 1;
}

//------------------------------------------------------------------------------------
// Forward declarations for callbacks
//------------------------------------------------------------------------------------
static void EOS_CALL on_device_id_created(const EOS_Connect_CreateDeviceIdCallbackInfo *data);
static void EOS_CALL on_connect_login(const EOS_Connect_LoginCallbackInfo *data);
static void EOS_CALL on_connect_create_user(const EOS_Connect_CreateUserCallbackInfo *data);
static void EOS_CALL on_peer_connection_request(const EOS_P2P_OnIncomingConnectionRequestInfo *data);
static void EOS_CALL on_peer_connection_closed(const EOS_P2P_OnRemoteConnectionClosedInfo *data);

static void do_connect_login(void)
{
    EOS_Connect_Credentials creds;
    memset(&creds, 0, sizeof(creds));
    creds.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
    creds.Type = EOS_ECT_DEVICEID_ACCESS_TOKEN;
    creds.Token = NULL;

    EOS_Connect_UserLoginInfo userInfo;
    memset(&userInfo, 0, sizeof(userInfo));
    userInfo.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
    userInfo.DisplayName = "Player";

    EOS_Connect_LoginOptions loginOpts;
    memset(&loginOpts, 0, sizeof(loginOpts));
    loginOpts.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
    loginOpts.Credentials = &creds;
    loginOpts.UserLoginInfo = &userInfo;

    EOS_Connect_Login(g_eosConnect, &loginOpts, NULL, on_connect_login);
}

static void EOS_CALL on_device_id_deleted(const EOS_Connect_DeleteDeviceIdCallbackInfo *data);

static void EOS_CALL on_device_id_created(const EOS_Connect_CreateDeviceIdCallbackInfo *data)
{
    printf("[EOS] on_device_id_created: result=%s\n", EOS_EResult_ToString(data->ResultCode));
    fflush(stdout);

    if (data->ResultCode == EOS_DuplicateNotAllowed && g_eosAltInstance && !g_eosAltDeleteDone) {
        // Alt instance: delete existing device ID and create a fresh one
        printf("[EOS] Alt instance: deleting existing device ID to get a new identity...\n");
        fflush(stdout);
        EOS_Connect_DeleteDeviceIdOptions delOpts;
        memset(&delOpts, 0, sizeof(delOpts));
        delOpts.ApiVersion = EOS_CONNECT_DELETEDEVICEID_API_LATEST;
        EOS_Connect_DeleteDeviceId(g_eosConnect, &delOpts, NULL, on_device_id_deleted);
        return;
    }

    if (data->ResultCode == EOS_Success ||
        data->ResultCode == EOS_DuplicateNotAllowed) {
        printf("[EOS] Device ID ready, logging in...\n");
        fflush(stdout);
        do_connect_login();
    } else {
        printf("[EOS] CreateDeviceId failed: %s\n", EOS_EResult_ToString(data->ResultCode));
        fflush(stdout);
    }
}

// Called after alt-instance login to wipe the credential from disk,
// so the normal instance (started later) creates its own fresh identity.
static void EOS_CALL on_post_login_device_id_deleted(
    const EOS_Connect_DeleteDeviceIdCallbackInfo *data)
{
    printf("[EOS] Alt post-login device ID cleanup: %s\n", EOS_EResult_ToString(data->ResultCode));
    printf("[EOS] Alt instance ready. Start the other instance now (without --eos-alt).\n");
    fflush(stdout);
}

static void EOS_CALL on_device_id_deleted(const EOS_Connect_DeleteDeviceIdCallbackInfo *data)
{
    printf("[EOS] on_device_id_deleted: result=%s\n", EOS_EResult_ToString(data->ResultCode));
    fflush(stdout);
    // Now create a fresh device ID — this gets a brand new identity
    g_eosAltDeleteDone = true; // won't re-enter the delete branch in on_device_id_created
    EOS_Connect_CreateDeviceIdOptions devOpts;
    memset(&devOpts, 0, sizeof(devOpts));
    devOpts.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
    devOpts.DeviceModel = "PC-Alt";
    EOS_Connect_CreateDeviceId(g_eosConnect, &devOpts, NULL, on_device_id_created);
}

static void register_p2p_notifications(void)
{
    EOS_P2P_SocketId sid = make_socket_id();
    {
        EOS_P2P_AddNotifyPeerConnectionRequestOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
        opts.LocalUserId = g_localUserId;
        opts.SocketId = &sid;
        g_peerConnReqNotif = EOS_P2P_AddNotifyPeerConnectionRequest(
            g_eosP2P, &opts, NULL, on_peer_connection_request);
    }
    {
        EOS_P2P_AddNotifyPeerConnectionClosedOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
        opts.LocalUserId = g_localUserId;
        opts.SocketId = &sid;
        g_peerConnCloseNotif = EOS_P2P_AddNotifyPeerConnectionClosed(
            g_eosP2P, &opts, NULL, on_peer_connection_closed);
    }
    printf("[EOS] P2P notifications registered\n");
}

// After alt-instance login, wipe the device credential from disk so the
// normal instance (started later) will create its own fresh identity.
static void alt_post_login_cleanup(void)
{
    if (!g_eosAltInstance) return;
    printf("[EOS] Alt: wiping device credential from disk (session stays in memory)...\n");
    fflush(stdout);
    EOS_Connect_DeleteDeviceIdOptions delOpts;
    memset(&delOpts, 0, sizeof(delOpts));
    delOpts.ApiVersion = EOS_CONNECT_DELETEDEVICEID_API_LATEST;
    EOS_Connect_DeleteDeviceId(g_eosConnect, &delOpts, NULL, on_post_login_device_id_deleted);
}

static void EOS_CALL on_connect_login(const EOS_Connect_LoginCallbackInfo *data)
{
    printf("[EOS] on_connect_login: result=%s\n", EOS_EResult_ToString(data->ResultCode));
    fflush(stdout);
    if (data->ResultCode == EOS_Success) {
        g_localUserId = data->LocalUserId;
        g_eosLoggedIn = true;
        register_p2p_notifications();
        char buf[64];
        int bufLen = sizeof(buf);
        EOS_ProductUserId_ToString(g_localUserId, buf, &bufLen);
        printf("[EOS] Logged in! UserId=%s\n", buf);
        fflush(stdout);
        alt_post_login_cleanup();
    } else if (data->ResultCode == EOS_InvalidUser) {
        // Need to create user first
        printf("[EOS] No user found, creating...\n");
        fflush(stdout);
        EOS_Connect_CreateUserOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
        opts.ContinuanceToken = data->ContinuanceToken;
        EOS_Connect_CreateUser(g_eosConnect, &opts, NULL, on_connect_create_user);
    } else {
        printf("[EOS] Login failed: %s\n", EOS_EResult_ToString(data->ResultCode));
        fflush(stdout);
    }
}

static void EOS_CALL on_connect_create_user(const EOS_Connect_CreateUserCallbackInfo *data)
{
    printf("[EOS] on_connect_create_user: result=%s\n", EOS_EResult_ToString(data->ResultCode));
    fflush(stdout);
    if (data->ResultCode == EOS_Success) {
        g_localUserId = data->LocalUserId;
        g_eosLoggedIn = true;
        register_p2p_notifications();
        printf("[EOS] User created and logged in!\n");
        fflush(stdout);
        alt_post_login_cleanup();
    } else {
        printf("[EOS] CreateUser failed: %s\n", EOS_EResult_ToString(data->ResultCode));
        fflush(stdout);
    }
}

//------------------------------------------------------------------------------------
// P2P connection callbacks
//------------------------------------------------------------------------------------
static void EOS_CALL on_peer_connection_request(
    const EOS_P2P_OnIncomingConnectionRequestInfo *data)
{
    char remoteBuf[64] = {0};
    int remoteBufLen = sizeof(remoteBuf);
    if (data->RemoteUserId)
        EOS_ProductUserId_ToString(data->RemoteUserId, remoteBuf, &remoteBufLen);
    printf("[EOS] Incoming P2P connection request from %s\n", remoteBuf);
    fflush(stdout);
    // Auto-accept
    EOS_P2P_SocketId sid = make_socket_id();
    EOS_P2P_AcceptConnectionOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
    opts.LocalUserId = g_localUserId;
    opts.RemoteUserId = data->RemoteUserId;
    opts.SocketId = &sid;
    EOS_P2P_AcceptConnection(g_eosP2P, &opts);
}

static void EOS_CALL on_peer_connection_closed(
    const EOS_P2P_OnRemoteConnectionClosedInfo *data)
{
    printf("[EOS] Peer disconnected (reason=%d)\n", (int)data->Reason);
    g_hostRemoteConnected = false;
    // Signal to client that peer disconnected
    if (g_activeEosClient) {
        g_activeEosClient->peerDisconnected = true;
    }
}

//------------------------------------------------------------------------------------
// Lobby callbacks
//------------------------------------------------------------------------------------
static EosClient *g_pendingLobbyClient = NULL;

static void EOS_CALL on_lobby_updated(const EOS_Lobby_UpdateLobbyCallbackInfo *data)
{
    if (data->ResultCode == EOS_Success) {
        printf("[EOS] Lobby attribute update SUCCESS (lobby: %s)\n", data->LobbyId);
    } else {
        printf("[EOS] Lobby attribute update FAILED: %s\n", EOS_EResult_ToString(data->ResultCode));
    }
    fflush(stdout);
}

static void EOS_CALL on_lobby_created(const EOS_Lobby_CreateLobbyCallbackInfo *data)
{
    EosClient *ec = g_pendingLobbyClient;
    if (!ec) return;

    if (data->ResultCode != EOS_Success) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg),
                 "Lobby create failed: %s", EOS_EResult_ToString(data->ResultCode));
        ec->state = EOS_STATE_ERROR;
        printf("[EOS] %s\n", ec->errorMsg);
        return;
    }

    strncpy(g_currentLobbyId, data->LobbyId, sizeof(g_currentLobbyId) - 1);
    printf("[EOS] Lobby created: %s (code: %s)\n", data->LobbyId, ec->lobbyCode);

    // Set the lobby code attribute
    EOS_Lobby_UpdateLobbyModificationOptions modOpts;
    memset(&modOpts, 0, sizeof(modOpts));
    modOpts.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
    modOpts.LocalUserId = g_localUserId;
    modOpts.LobbyId = data->LobbyId;

    EOS_HLobbyModification modHandle = NULL;
    EOS_EResult r = EOS_Lobby_UpdateLobbyModification(g_eosLobby, &modOpts, &modHandle);
    if (r != EOS_Success) {
        printf("[EOS] UpdateLobbyModification failed: %s\n", EOS_EResult_ToString(r));
        ec->state = EOS_STATE_IN_LOBBY;
        return;
    }

    // Add LOBBY_CODE attribute
    EOS_Lobby_AttributeData attrData;
    memset(&attrData, 0, sizeof(attrData));
    attrData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    attrData.Key = LOBBY_ATTR_CODE;
    attrData.ValueType = EOS_AT_STRING;
    attrData.Value.AsUtf8 = ec->lobbyCode;

    EOS_LobbyModification_AddAttributeOptions addAttrOpts;
    memset(&addAttrOpts, 0, sizeof(addAttrOpts));
    addAttrOpts.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
    addAttrOpts.Attribute = &attrData;
    addAttrOpts.Visibility = EOS_LAT_PUBLIC;

    r = EOS_LobbyModification_AddAttribute(modHandle, &addAttrOpts);
    printf("[EOS] AddAttribute result: %s\n", EOS_EResult_ToString(r));
    fflush(stdout);

    EOS_Lobby_UpdateLobbyOptions updateOpts;
    memset(&updateOpts, 0, sizeof(updateOpts));
    updateOpts.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
    updateOpts.LobbyModificationHandle = modHandle;

    EOS_Lobby_UpdateLobby(g_eosLobby, &updateOpts, NULL, on_lobby_updated);
    EOS_LobbyModification_Release(modHandle);

    ec->state = EOS_STATE_IN_LOBBY;
    printf("[EOS] Lobby code attribute queued: %s\n", ec->lobbyCode);
    fflush(stdout);
}

// Called when a new member joins the lobby (host side)
static EOS_NotificationId g_lobbyMemberNotif = EOS_INVALID_NOTIFICATIONID;

static void EOS_CALL on_lobby_member_status_received(
    const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo *data)
{
    char targetBuf[64] = {0};
    int targetBufLen = sizeof(targetBuf);
    if (data->TargetUserId)
        EOS_ProductUserId_ToString(data->TargetUserId, targetBuf, &targetBufLen);
    printf("[EOS] LobbyMemberStatus: status=%d target=%s lobby=%s\n",
           (int)data->CurrentStatus, targetBuf,
           data->LobbyId ? data->LobbyId : "(null)");
    fflush(stdout);

    if (data->CurrentStatus != EOS_LMS_JOINED) return;

    EosClient *ec = g_pendingLobbyClient;
    if (!ec || !ec->isHost) {
        printf("[EOS] Ignoring member join: ec=%p isHost=%d\n", (void*)ec, ec ? ec->isHost : -1);
        fflush(stdout);
        return;
    }

    printf("[EOS] Remote player joined lobby!\n");

    // Get the remote user's ProductUserId
    g_hostRemoteUserId = data->TargetUserId;
    g_hostRemoteConnected = true;

    // Set up socketpairs for the game session
    if (make_socketpair(g_hostLocalSock) < 0 || make_socketpair(g_hostRemoteSock) < 0) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg), "Failed to create socketpair");
        ec->state = EOS_STATE_ERROR;
        return;
    }

    // Make all ends non-blocking
    net_set_nonblocking(g_hostLocalSock[0]);
    net_set_nonblocking(g_hostLocalSock[1]);
    net_set_nonblocking(g_hostRemoteSock[0]);
    net_set_nonblocking(g_hostRemoteSock[1]);

    // Initialize game session — session reads/writes sv[0] ends
    session_init(&g_hostSession, g_hostLocalSock[0]);
    snprintf(g_hostSession.players[0].name,
             sizeof(g_hostSession.players[0].name), "%s", g_hostPlayerName);

    // Feed a synthetic JOIN message into localSock[1] so session sees it
    // (session_init already sent LOBBY_CODE to localSock[0])

    // Add player 2 on remoteSock[0]
    snprintf(g_hostSession.players[1].name,
             sizeof(g_hostSession.players[1].name), "%s", "Opponent");
    session_add_player(&g_hostSession, g_hostRemoteSock[0]);
    g_hostSessionActive = true;
    g_hostTickAccum = 0.0;
    g_hostLastTime = 0.0;

    ec->state = EOS_STATE_IN_GAME;
    printf("[EOS] Game session started (host)\n");
}

// Joiner: lobby search complete
static void EOS_CALL on_lobby_search_find(const EOS_LobbySearch_FindCallbackInfo *data);
static void EOS_CALL on_lobby_joined(const EOS_Lobby_JoinLobbyByIdCallbackInfo *data);

static EOS_HLobbySearch g_lobbySearchHandle = NULL;

static void EOS_CALL on_lobby_search_find(const EOS_LobbySearch_FindCallbackInfo *data)
{
    EosClient *ec = g_pendingLobbyClient;
    if (!ec) return;

    if (data->ResultCode != EOS_Success) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg),
                 "Lobby search failed: %s", EOS_EResult_ToString(data->ResultCode));
        ec->state = EOS_STATE_ERROR;
        if (g_lobbySearchHandle) {
            EOS_LobbySearch_Release(g_lobbySearchHandle);
            g_lobbySearchHandle = NULL;
        }
        return;
    }

    // Get result count
    EOS_LobbySearch_GetSearchResultCountOptions countOpts;
    memset(&countOpts, 0, sizeof(countOpts));
    countOpts.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
    uint32_t resultCount = EOS_LobbySearch_GetSearchResultCount(g_lobbySearchHandle, &countOpts);
    printf("[EOS] Lobby search returned %u results (code: %s)\n", resultCount, ec->lobbyCode);
    fflush(stdout);

    if (resultCount == 0) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg), "No lobby found with that code");
        ec->state = EOS_STATE_ERROR;
        EOS_LobbySearch_Release(g_lobbySearchHandle);
        g_lobbySearchHandle = NULL;
        return;
    }

    // Get first result
    printf("[EOS] Step 1: CopySearchResultByIndex...\n"); fflush(stdout);
    EOS_LobbySearch_CopySearchResultByIndexOptions copyOpts;
    memset(&copyOpts, 0, sizeof(copyOpts));
    copyOpts.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
    copyOpts.LobbyIndex = 0;

    EOS_HLobbyDetails detailsHandle = NULL;
    EOS_EResult r = EOS_LobbySearch_CopySearchResultByIndex(g_lobbySearchHandle, &copyOpts, &detailsHandle);
    printf("[EOS] Step 1 result: %s (handle=%p)\n", EOS_EResult_ToString(r), (void*)detailsHandle); fflush(stdout);
    EOS_LobbySearch_Release(g_lobbySearchHandle);
    g_lobbySearchHandle = NULL;

    if (r != EOS_Success || !detailsHandle) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg), "Failed to get lobby details");
        ec->state = EOS_STATE_ERROR;
        return;
    }

    // Get lobby info to extract lobby ID
    printf("[EOS] Step 2: CopyInfo...\n"); fflush(stdout);
    EOS_LobbyDetails_CopyInfoOptions infoOpts;
    memset(&infoOpts, 0, sizeof(infoOpts));
    infoOpts.ApiVersion = EOS_LOBBYDETAILS_COPYINFO_API_LATEST;

    EOS_LobbyDetails_Info *lobbyInfo = NULL;
    r = EOS_LobbyDetails_CopyInfo(detailsHandle, &infoOpts, &lobbyInfo);
    printf("[EOS] Step 2 result: %s (info=%p)\n", EOS_EResult_ToString(r), (void*)lobbyInfo); fflush(stdout);
    if (r != EOS_Success || !lobbyInfo) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg), "Failed to copy lobby info");
        ec->state = EOS_STATE_ERROR;
        EOS_LobbyDetails_Release(detailsHandle);
        return;
    }

    printf("[EOS] Step 2b: LobbyId=%s\n", lobbyInfo->LobbyId ? lobbyInfo->LobbyId : "(null)"); fflush(stdout);
    strncpy(g_currentLobbyId, lobbyInfo->LobbyId, sizeof(g_currentLobbyId) - 1);

    // Get the lobby owner (host) ProductUserId
    printf("[EOS] Step 3: GetLobbyOwner...\n"); fflush(stdout);
    EOS_LobbyDetails_GetLobbyOwnerOptions ownerOpts;
    memset(&ownerOpts, 0, sizeof(ownerOpts));
    ownerOpts.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
    g_joinerHostUserId = EOS_LobbyDetails_GetLobbyOwner(detailsHandle, &ownerOpts);

    if (g_joinerHostUserId) {
        char ownerBuf[64]; int ownerBufLen = sizeof(ownerBuf);
        EOS_ProductUserId_ToString(g_joinerHostUserId, ownerBuf, &ownerBufLen);
        char localBuf[64]; int localBufLen = sizeof(localBuf);
        EOS_ProductUserId_ToString(g_localUserId, localBuf, &localBufLen);
        printf("[EOS] Step 3: owner=%s local=%s same=%d\n", ownerBuf, localBuf,
               EOS_ProductUserId_IsValid(g_joinerHostUserId) && (g_joinerHostUserId == g_localUserId));
        fflush(stdout);

        // Warn if same user (same-machine testing with Device ID)
        if (g_joinerHostUserId == g_localUserId) {
            printf("[EOS] WARNING: Host and joiner are the same EOS user!\n");
            printf("[EOS] Same-machine testing requires two different Epic accounts or different device IDs.\n");
            fflush(stdout);
        }
    } else {
        printf("[EOS] Step 3: owner is NULL!\n"); fflush(stdout);
    }

    EOS_LobbyDetails_Info_Release(lobbyInfo);
    EOS_LobbyDetails_Release(detailsHandle);

    // Join the lobby using the saved lobby ID
    printf("[EOS] Step 4: JoinLobbyById (lobbyId=%s)...\n", g_currentLobbyId); fflush(stdout);
    EOS_Lobby_JoinLobbyByIdOptions joinByIdOpts;
    memset(&joinByIdOpts, 0, sizeof(joinByIdOpts));
    joinByIdOpts.ApiVersion = EOS_LOBBY_JOINLOBBYBYID_API_LATEST;
    joinByIdOpts.LobbyId = g_currentLobbyId;
    joinByIdOpts.LocalUserId = g_localUserId;

    EOS_Lobby_JoinLobbyById(g_eosLobby, &joinByIdOpts, NULL, on_lobby_joined);
    printf("[EOS] Step 4: JoinLobbyById call returned\n"); fflush(stdout);
}

static void EOS_CALL on_lobby_joined(const EOS_Lobby_JoinLobbyByIdCallbackInfo *data)
{
    printf("[EOS] on_lobby_joined callback: result=%s\n", EOS_EResult_ToString(data->ResultCode));
    fflush(stdout);

    EosClient *ec = g_pendingLobbyClient;
    if (!ec) { printf("[EOS] on_lobby_joined: no pending client!\n"); fflush(stdout); return; }

    if (data->ResultCode != EOS_Success) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg),
                 "Join lobby failed: %s", EOS_EResult_ToString(data->ResultCode));
        ec->state = EOS_STATE_ERROR;
        printf("[EOS] %s\n", ec->errorMsg); fflush(stdout);
        return;
    }

    printf("[EOS] Joined lobby! Sending hello P2P to host...\n"); fflush(stdout);
    ec->state = EOS_STATE_IN_GAME;
    ec->playerSlot = 1;

    // Send a "hello" P2P packet to the host to establish the connection
    // (This triggers the host's connection request callback)
    uint8_t hello = 0;
    int sendResult = eos_send_msg(g_joinerHostUserId, MSG_JOIN, &hello, 0);
    printf("[EOS] Hello P2P send result: %d\n", sendResult); fflush(stdout);
}

//------------------------------------------------------------------------------------
// Public API — Lifecycle
//------------------------------------------------------------------------------------
int eos_init(bool altInstance)
{
    if (g_eosInitialized) return 0;

    g_eosAltInstance = altInstance;
    net_platform_init();

    // Initialize EOS SDK
    EOS_InitializeOptions initOpts;
    memset(&initOpts, 0, sizeof(initOpts));
    initOpts.ApiVersion = EOS_INITIALIZE_API_LATEST;
    initOpts.ProductName = "RelicRivals";
    initOpts.ProductVersion = "1.0";

    EOS_EResult r = EOS_Initialize(&initOpts);
    if (r != EOS_Success && r != EOS_AlreadyConfigured) {
        printf("[EOS] Initialize failed: %s\n", EOS_EResult_ToString(r));
        return -1;
    }

    // Set up logging
    EOS_Logging_SetCallback(eos_log_cb);
    EOS_Logging_SetLogLevel(EOS_LC_ALL_CATEGORIES, EOS_LOG_Warning);

    // Create platform
    EOS_Platform_Options platOpts;
    memset(&platOpts, 0, sizeof(platOpts));
    platOpts.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    platOpts.ProductId = EOS_PRODUCT_ID;
    platOpts.SandboxId = EOS_SANDBOX_ID;
    platOpts.DeploymentId = EOS_DEPLOYMENT_ID;
    platOpts.ClientCredentials.ClientId = EOS_CLIENT_ID;
    platOpts.ClientCredentials.ClientSecret = EOS_CLIENT_SECRET;
    platOpts.bIsServer = EOS_FALSE;
    platOpts.Flags = EOS_PF_DISABLE_OVERLAY;

    g_eosPlatform = EOS_Platform_Create(&platOpts);
    if (!g_eosPlatform) {
        printf("[EOS] Platform_Create failed!\n");
        return -1;
    }

    g_eosConnect = EOS_Platform_GetConnectInterface(g_eosPlatform);
    g_eosP2P = EOS_Platform_GetP2PInterface(g_eosPlatform);
    g_eosLobby = EOS_Platform_GetLobbyInterface(g_eosPlatform);

    // P2P notifications are registered after login succeeds (register_p2p_notifications)

    // Create Device ID (or reuse existing)
    EOS_Connect_CreateDeviceIdOptions devOpts;
    memset(&devOpts, 0, sizeof(devOpts));
    devOpts.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
    devOpts.DeviceModel = "PC";

    EOS_Connect_CreateDeviceId(g_eosConnect, &devOpts, NULL, on_device_id_created);

    g_eosInitialized = true;
    printf("[EOS] SDK initialized, authenticating...\n");
    fflush(stdout);
    return 0;
}

void eos_tick(void)
{
    if (g_eosPlatform)
        EOS_Platform_Tick(g_eosPlatform);
}

void eos_shutdown(void)
{
    if (g_peerConnReqNotif != EOS_INVALID_NOTIFICATIONID)
        EOS_P2P_RemoveNotifyPeerConnectionRequest(g_eosP2P, g_peerConnReqNotif);
    if (g_peerConnCloseNotif != EOS_INVALID_NOTIFICATIONID)
        EOS_P2P_RemoveNotifyPeerConnectionClosed(g_eosP2P, g_peerConnCloseNotif);

    if (g_lobbyMemberNotif != EOS_INVALID_NOTIFICATIONID)
        EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(g_eosLobby, g_lobbyMemberNotif);

    if (g_eosPlatform) {
        EOS_Platform_Release(g_eosPlatform);
        g_eosPlatform = NULL;
    }
    EOS_Shutdown();
    g_eosInitialized = false;
    g_eosLoggedIn = false;
    g_localUserId = NULL;
}

bool eos_is_logged_in(void)
{
    return g_eosLoggedIn;
}

//------------------------------------------------------------------------------------
// Public API — Client
//------------------------------------------------------------------------------------
void eos_client_init(EosClient *ec)
{
    memset(ec, 0, sizeof(*ec));
    ec->state = EOS_STATE_UNINIT;
    for (int i = 0; i < MAX_SHOP_SLOTS; i++)
        ec->serverShop[i].abilityId = -1;
}

static void generate_lobby_code(char code[LOBBY_CODE_LEN + 1])
{
    const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < LOBBY_CODE_LEN; i++)
        code[i] = chars[rand() % (sizeof(chars) - 1)];
    code[LOBBY_CODE_LEN] = '\0';
}

int eos_host_lobby(EosClient *ec, const char *playerName)
{
    if (!g_eosLoggedIn) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg), "EOS not logged in yet");
        ec->state = EOS_STATE_ERROR;
        return -1;
    }

    ec->isHost = true;
    ec->playerSlot = 0;
    generate_lobby_code(ec->lobbyCode);
    strncpy(g_hostPlayerName, playerName ? playerName : "Host", sizeof(g_hostPlayerName) - 1);

    g_pendingLobbyClient = ec;
    g_activeEosClient = ec;

    // Register for lobby member notifications
    EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions notifOpts;
    memset(&notifOpts, 0, sizeof(notifOpts));
    notifOpts.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
    g_lobbyMemberNotif = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(
        g_eosLobby, &notifOpts, NULL, on_lobby_member_status_received);

    // Create the lobby
    EOS_Lobby_CreateLobbyOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
    opts.LocalUserId = g_localUserId;
    opts.MaxLobbyMembers = 2;
    opts.PermissionLevel = EOS_LPL_PUBLICADVERTISED;
    opts.bPresenceEnabled = EOS_FALSE;
    opts.bAllowInvites = EOS_FALSE;
    opts.BucketId = "RelicRivals";

    EOS_Lobby_CreateLobby(g_eosLobby, &opts, NULL, on_lobby_created);

    ec->state = EOS_STATE_LOGGING_IN; // waiting for lobby creation
    return 0;
}

int eos_join_lobby(EosClient *ec, const char *lobbyCode, const char *playerName)
{
    if (!g_eosLoggedIn) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg), "EOS not logged in yet");
        ec->state = EOS_STATE_ERROR;
        return -1;
    }

    ec->isHost = false;
    ec->playerSlot = 1;
    strncpy(ec->lobbyCode, lobbyCode, LOBBY_CODE_LEN);
    ec->lobbyCode[LOBBY_CODE_LEN] = '\0';
    strncpy(g_joinPlayerName, playerName ? playerName : "Player", sizeof(g_joinPlayerName) - 1);

    g_pendingLobbyClient = ec;
    g_activeEosClient = ec;

    // Create lobby search
    EOS_Lobby_CreateLobbySearchOptions searchCreateOpts;
    memset(&searchCreateOpts, 0, sizeof(searchCreateOpts));
    searchCreateOpts.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
    searchCreateOpts.MaxResults = 1;

    EOS_EResult r = EOS_Lobby_CreateLobbySearch(g_eosLobby, &searchCreateOpts, &g_lobbySearchHandle);
    if (r != EOS_Success) {
        snprintf(ec->errorMsg, sizeof(ec->errorMsg),
                 "Failed to create search: %s", EOS_EResult_ToString(r));
        ec->state = EOS_STATE_ERROR;
        return -1;
    }

    // Set search parameter: LOBBY_CODE == lobbyCode
    EOS_Lobby_AttributeData attrData;
    memset(&attrData, 0, sizeof(attrData));
    attrData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
    attrData.Key = LOBBY_ATTR_CODE;
    attrData.ValueType = EOS_AT_STRING;
    attrData.Value.AsUtf8 = lobbyCode;

    EOS_LobbySearch_SetParameterOptions paramOpts;
    memset(&paramOpts, 0, sizeof(paramOpts));
    paramOpts.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
    paramOpts.Parameter = &attrData;
    paramOpts.ComparisonOp = EOS_CO_EQUAL;

    EOS_LobbySearch_SetParameter(g_lobbySearchHandle, &paramOpts);

    // Execute search
    EOS_LobbySearch_FindOptions findOpts;
    memset(&findOpts, 0, sizeof(findOpts));
    findOpts.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
    findOpts.LocalUserId = g_localUserId;

    EOS_LobbySearch_Find(g_lobbySearchHandle, &findOpts, NULL, on_lobby_search_find);

    ec->state = EOS_STATE_LOGGING_IN; // waiting for search
    return 0;
}

//------------------------------------------------------------------------------------
// Handle incoming server messages (joiner path — same as net_client.c)
//------------------------------------------------------------------------------------
static void eos_handle_server_msg(EosClient *ec, const NetMessage *msg)
{
    switch (msg->type) {
    case MSG_LOBBY_CODE:
        if (msg->size >= LOBBY_CODE_LEN) {
            memcpy(ec->lobbyCode, msg->payload, LOBBY_CODE_LEN);
            ec->lobbyCode[LOBBY_CODE_LEN] = '\0';
        }
        break;

    case MSG_GAME_START:
        if (msg->size >= 2) {
            ec->playerSlot = msg->payload[0];
            ec->startingGold = msg->payload[1];
            ec->currentGold = ec->startingGold;
            ec->opponentName[0] = '\0';
            if (msg->size >= 3) {
                int oppNameLen = msg->payload[2];
                if (oppNameLen > 31) oppNameLen = 31;
                if (msg->size >= 3 + oppNameLen) {
                    memcpy(ec->opponentName, msg->payload + 3, oppNameLen);
                    ec->opponentName[oppNameLen] = '\0';
                }
            }
            ec->gameStarted = true;
            ec->state = EOS_STATE_IN_GAME;
        }
        break;

    case MSG_PREP_START:
        if (msg->size >= 4) {
            ec->currentRound = msg->payload[0];
            ec->isPveRound = msg->payload[1];
            ec->currentGold = ((int)msg->payload[2] << 8) | msg->payload[3];
            ec->prepStarted = true;
            ec->opponentReady = false;
        }
        break;

    case MSG_COMBAT_START:
        if (msg->size >= 6) {
            ec->currentRound = msg->payload[0];
            memcpy(&ec->combatSeed, msg->payload + 1, 4);
            ec->combatNetUnitCount = msg->payload[5];
            if (ec->combatNetUnitCount > NET_MAX_UNITS) ec->combatNetUnitCount = NET_MAX_UNITS;
            if (msg->size >= 6 + ec->combatNetUnitCount * (int)sizeof(NetUnit))
                memcpy(ec->combatNetUnits, msg->payload + 6,
                       ec->combatNetUnitCount * sizeof(NetUnit));
            ec->combatStarted = true;
        }
        break;

    case MSG_ROUND_RESULT:
        if (msg->size >= 6) {
            ec->roundWinner = msg->payload[0];
            ec->roundIsPve = msg->payload[1];
            ec->playerHealth[0] = msg->payload[2];
            ec->playerHealth[1] = msg->payload[3];
            ec->currentRound = msg->payload[4];
            ec->lastRoundDamage = msg->payload[5];
            ec->roundResultReady = true;
        }
        break;

    case MSG_GAME_OVER:
        if (msg->size >= 3) {
            ec->gameWinner = msg->payload[0];
            ec->playerHealth[0] = msg->payload[1];
            ec->playerHealth[1] = msg->payload[2];
            ec->gameOver = true;
        }
        break;

    case MSG_SHOP_ROLL_RESULT: {
        int count = msg->size / 2;
        if (count > MAX_SHOP_SLOTS) count = MAX_SHOP_SLOTS;
        for (int i = 0; i < count; i++) {
            ec->serverShop[i].abilityId = (int8_t)msg->payload[i * 2];
            ec->serverShop[i].level = msg->payload[i * 2 + 1];
        }
        ec->shopUpdated = true;
    } break;

    case MSG_OPPONENT_READY:
        ec->opponentReady = true;
        ec->prepTimeRemaining = 0;
        if (msg->size >= 2)
            ec->prepTimeRemaining = ((int)msg->payload[0] << 8) | msg->payload[1];
        break;

    case MSG_GOLD_UPDATE:
        if (msg->size >= 2) {
            ec->currentGold = ((int)msg->payload[0] << 8) | msg->payload[1];
            ec->goldUpdated = true;
        }
        break;

    case MSG_COMBAT_SYNC:
        if (msg->size >= 3) {
            ec->combatSyncTick = ((uint16_t)msg->payload[0] << 8) | msg->payload[1];
            ec->combatSyncCount = msg->payload[2];
            if (ec->combatSyncCount > NET_MAX_UNITS) ec->combatSyncCount = NET_MAX_UNITS;
            int syncDataSize = ec->combatSyncCount * (int)sizeof(SyncUnit);
            if (msg->size >= 3 + syncDataSize) {
                memcpy(ec->combatSyncUnits, msg->payload + 3, syncDataSize);
            }
            // Read state hash if present (4 bytes after sync units)
            ec->combatSyncHash = 0;
            if (msg->size >= 3 + syncDataSize + 4) {
                memcpy(&ec->combatSyncHash, msg->payload + 3 + syncDataSize, 4);
            }
            ec->combatSyncReady = true;
        }
        break;

    case MSG_ERROR:
        if (msg->size > 0) {
            int len = msg->size < 127 ? msg->size : 127;
            memcpy(ec->errorMsg, msg->payload, len);
            ec->errorMsg[len] = '\0';
        }
        ec->state = EOS_STATE_ERROR;
        break;

    default:
        break;
    }
}

//------------------------------------------------------------------------------------
// Host: drain socketpair and forward messages
//------------------------------------------------------------------------------------
// Platform-specific high-resolution timer for decoupled tick
#ifdef _WIN32
static double host_get_time(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double host_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif
static void host_drain_and_forward(EosClient *ec)
{
    if (!g_hostSessionActive) return;

    // 1) Receive P2P packets from remote → write into remoteSock[1] for session to read
    {
        NetMessage msg;
        EOS_ProductUserId sender = NULL;
        while (eos_recv_msg(&sender, &msg) == 1) {
            net_send_msg(g_hostRemoteSock[1], msg.type, msg.payload, msg.size);
        }
    }

    // 2) Poll messages from both socketpairs (session may have written)
    for (int p = 0; p < 2; p++) {
        if (!g_hostSession.players[p].connected) continue;
        NetMessage msg;
        int r = net_recv_msg_nonblock(g_hostSession.players[p].sockfd, &msg);
        if (r == 1) {
            session_handle_msg(&g_hostSession, p, &msg);
        }
    }

    // 3) Tick the session — decoupled from render FPS
    //    Accumulate real elapsed time, run fixed 1/60 ticks to catch up
    {
        double now = host_get_time();
        if (g_hostLastTime == 0.0) g_hostLastTime = now;
        double elapsed = now - g_hostLastTime;
        g_hostLastTime = now;
        if (elapsed > 0.1) elapsed = 0.1; // cap at 100ms to prevent spiral
        g_hostTickAccum += elapsed;
        const double tickDt = 1.0 / 60.0;
        int dead = 0;
        while (g_hostTickAccum >= tickDt && !dead) {
            g_hostTickAccum -= tickDt;
            dead = session_tick(&g_hostSession, (float)tickDt);
        }
        if (dead) {
            printf("[EOS] Host session ended (dead=%d)\n", dead);
            fflush(stdout);
            g_hostSessionActive = false;
        }
    }

    // 4) Drain localSock[1] — these are messages the session sent to player 0 (host)
    {
        NetMessage msg;
        int r;
        while ((r = net_recv_msg_nonblock(g_hostLocalSock[1], &msg)) > 0) {
            eos_handle_server_msg(ec, &msg);
        }
    }

    // 5) Drain remoteSock[1] — these are messages the session sent to player 1 (remote)
    {
        NetMessage msg;
        int r;
        while ((r = net_recv_msg_nonblock(g_hostRemoteSock[1], &msg)) > 0) {
            if (g_hostRemoteConnected && g_hostRemoteUserId) {
                eos_send_msg(g_hostRemoteUserId, msg.type, msg.payload, msg.size);
            }
        }
    }
}

//------------------------------------------------------------------------------------
// Public API — Polling & Send
//------------------------------------------------------------------------------------
void eos_client_poll(EosClient *ec)
{
    if (ec->state < EOS_STATE_IN_GAME && ec->state != EOS_STATE_ERROR) return;

    if (ec->isHost) {
        host_drain_and_forward(ec);
    } else {
        // Joiner: receive P2P packets directly
        NetMessage msg;
        EOS_ProductUserId sender = NULL;
        int r;
        while ((r = eos_recv_msg(&sender, &msg)) > 0) {
            eos_handle_server_msg(ec, &msg);
        }
        if (r < 0) {
            snprintf(ec->errorMsg, sizeof(ec->errorMsg), "P2P receive error");
            ec->state = EOS_STATE_ERROR;
        }
    }
}

void eos_client_send_ready(EosClient *ec, const Unit units[], int unitCount)
{
    NetUnit netUnits[NET_MAX_UNITS];
    int count = serialize_units(units, unitCount, netUnits, NET_MAX_UNITS);
    uint8_t payload[1 + sizeof(NetUnit) * NET_MAX_UNITS];
    payload[0] = (uint8_t)count;
    memcpy(payload + 1, netUnits, count * sizeof(NetUnit));
    uint16_t size = (uint16_t)(1 + count * sizeof(NetUnit));

    if (ec->isHost) {
        // Write into localSock[1] for session to read from localSock[0]
        net_send_msg(g_hostLocalSock[1], MSG_READY, payload, size);
    } else {
        eos_send_msg(g_joinerHostUserId, MSG_READY, payload, size);
    }
}

void eos_client_send_roll(EosClient *ec)
{
    if (ec->isHost) {
        net_send_msg(g_hostLocalSock[1], MSG_ROLL_SHOP, NULL, 0);
    } else {
        eos_send_msg(g_joinerHostUserId, MSG_ROLL_SHOP, NULL, 0);
    }
}

void eos_client_send_buy(EosClient *ec, int shopSlot)
{
    uint8_t payload[1] = { (uint8_t)shopSlot };
    if (ec->isHost) {
        net_send_msg(g_hostLocalSock[1], MSG_BUY_ABILITY, payload, 1);
    } else {
        eos_send_msg(g_joinerHostUserId, MSG_BUY_ABILITY, payload, 1);
    }
}

void eos_client_disconnect(EosClient *ec)
{
    // Clean up session
    if (g_hostSessionActive) {
        if (g_hostLocalSock[0] >= 0)  { close_socket(g_hostLocalSock[0]);  g_hostLocalSock[0]  = -1; }
        if (g_hostLocalSock[1] >= 0)  { close_socket(g_hostLocalSock[1]);  g_hostLocalSock[1]  = -1; }
        if (g_hostRemoteSock[0] >= 0) { close_socket(g_hostRemoteSock[0]); g_hostRemoteSock[0] = -1; }
        if (g_hostRemoteSock[1] >= 0) { close_socket(g_hostRemoteSock[1]); g_hostRemoteSock[1] = -1; }
        g_hostSessionActive = false;
    }

    // Close P2P connection
    if (g_localUserId) {
        EOS_P2P_SocketId sid = make_socket_id();
        EOS_P2P_CloseConnectionsOptions closeOpts;
        memset(&closeOpts, 0, sizeof(closeOpts));
        closeOpts.ApiVersion = EOS_P2P_CLOSECONNECTIONS_API_LATEST;
        closeOpts.LocalUserId = g_localUserId;
        closeOpts.SocketId = &sid;
        EOS_P2P_CloseConnections(g_eosP2P, &closeOpts);
    }

    // Leave lobby
    if (g_currentLobbyId[0] && g_localUserId) {
        EOS_Lobby_LeaveLobbyOptions leaveOpts;
        memset(&leaveOpts, 0, sizeof(leaveOpts));
        leaveOpts.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
        leaveOpts.LocalUserId = g_localUserId;
        leaveOpts.LobbyId = g_currentLobbyId;
        EOS_Lobby_LeaveLobby(g_eosLobby, &leaveOpts, NULL, NULL);
        g_currentLobbyId[0] = '\0';
    }

    if (g_lobbyMemberNotif != EOS_INVALID_NOTIFICATIONID) {
        EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(g_eosLobby, g_lobbyMemberNotif);
        g_lobbyMemberNotif = EOS_INVALID_NOTIFICATIONID;
    }

    g_hostRemoteConnected = false;
    g_hostRemoteUserId = NULL;
    g_joinerHostUserId = NULL;
    g_pendingLobbyClient = NULL;

    ec->state = EOS_STATE_UNINIT;
}

#endif // USE_EOS
