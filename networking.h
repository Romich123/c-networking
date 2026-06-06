#pragma once

#ifndef SOCKETS_CONTENT_HEADER
#define SOCKETS_CONTENT_HEADER

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOUSER
#define NOGDI

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define CLOSE_SOCKET(s) closesocket(s)
#define GET_ERROR() WSAGetLastError()
#define WOULDBLOCK WSAEWOULDBLOCK
#define INPROGRESS WSAEWOULDBLOCK // On Windows, same as WOULDBLOCK
#define SHUT_WR SD_SEND

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define htonll(x) (x)
#define ntohll(x) (x)
#else
#define htonll(x) (((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) (((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif

#else

#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <endian.h>
typedef int socket_t;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define GET_ERROR() errno
#define WOULDBLOCK EWOULDBLOCK
#define INPROGRESS EINPROGRESS
#define htonll htobe64
#define ntohll be64toh

#endif

typedef uint16_t iclient_t;
// should be unsigned
typedef uint8_t messagetype_t;

// should be optimized away
// so shouldn't cause any performance change
// clang-format off
#define ntohmt(x)                            \
    (sizeof(messagetype_t) == 1 ? (x)      : \
     sizeof(messagetype_t) == 2 ? ntohs(x) : \
     sizeof(messagetype_t) == 4 ? ntohl(x) : \
                                  ntohll(x))

#define htonmt(x)                            \
    (sizeof(messagetype_t) == 1 ? (x)      : \
     sizeof(messagetype_t) == 2 ? htons(x) : \
     sizeof(messagetype_t) == 4 ? htonl(x) : \
                                  htonll(x))
// clang-format on

_Static_assert(
    sizeof(messagetype_t) == 1 ||
        sizeof(messagetype_t) == 2 ||
        sizeof(messagetype_t) == 4 ||
        sizeof(messagetype_t) == 8,
    "Unsupported messagetype_t size");

// it underflows, so this is maximum addressable index
#define MAX_SERVER_CLIENTS ((iclient_t)(-1))
#define SERVER_LISTEN_BUFFER_PER_CLIENT (8 * 1024)

#define CLIENT_LISTEN_BUFFER (8 * 1024)

typedef struct ServerAddress {
    char ip[INET6_ADDRSTRLEN];
    uint16_t port;
    bool valid;
} ServerAddress;

typedef struct ServerClientData {
    socket_t socket;
    iclient_t index;
    bool active;
    // recv can return incomplete data
    // so this will save some between listen calls
    char *recvBuffered;
    size_t recvBufferedSize;
    size_t skipNextReceiveSize;
    // if send returns WOULDBLOCK
    char *sendBuffered;
    size_t sendBufferedSize;
} ServerClientData;

typedef struct ServerInstance {
    socket_t socket;
    ServerClientData *clients;
    // shouldn't change it after creation
    iclient_t maxClients;
    // stored for performance reasons
    // so instead of looping through all clients
    // it could loop until this
    iclient_t activeClientsBefore;
    // could also save minimal inactive client?
    // probably won't matter as much
    // because this all is intended for game networking
    // and usually player are connected for a long time
    uint16_t port;
} ServerInstance;

typedef struct SocketMessageHeader {
    // stores length of data only, not including header
    uint64_t size;
    messagetype_t messageType;
} SocketMessageHeader;

#define NETWORK_SOCKET_MESSAGE_HEADER (sizeof(uint64_t) + sizeof(messagetype_t))

typedef struct SocketMessage {
    // stores length of data only, not including header
    uint64_t size;
    messagetype_t messageType;
    char data[];
} SocketMessage;

typedef struct SocketMessageFromClient {
    iclient_t clientIndex;
    SocketMessage *message;
} SocketMessageFromClient;

ServerAddress ParseAddressIPv4(const char *address);

bool InitNetwork(void);
void CleanupNetwork(void);

// If `maxClients` = 0, then it set to `MAX_SERVER_CLIENTS`
ServerInstance *Server_Create(uint16_t port, iclient_t maxClients);
void Server_CleanUp(ServerInstance *server);

ServerClientData *Server_AcceptNewClient(ServerInstance *server);
void Server_DisconnectClient(ServerInstance *server, iclient_t clientIndex);

SocketMessage *Server_ListenToClient(ServerInstance *server, iclient_t clientIndex);
SocketMessageFromClient Server_Listen(ServerInstance *server);

int Server_Broadcast(ServerInstance *server, uint64_t size, messagetype_t msgType, const char *message);
int Server_SendTo(ServerInstance *server, iclient_t clientIndex, uint64_t size, messagetype_t msgType, const char *message);

typedef struct ClientInstance {
    socket_t socket;

    // recv can return incomplete data
    // so this will save some between listen calls
    char *recvBuffered;
    size_t recvBufferedSize;
    size_t skipNextReceiveSize;
    // if send returns WOULDBLOCK
    char *sendBuffered;
    size_t sendBufferedSize;
} ClientInstance;

ClientInstance *Client_Create(ServerAddress address);
void Client_CleanUp(ClientInstance *client);

SocketMessage *Client_Listen(ClientInstance *client);
int Client_Send(ClientInstance *client, uint64_t size, messagetype_t msgType, const char *message);

#endif