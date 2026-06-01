/*
 * This does NOT do little-endian/big-endian conversions
 */

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

#else

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
typedef int socket_t;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define GET_ERROR() errno
#define WOULDBLOCK EWOULDBLOCK
#define INPROGRESS EINPROGRESS

#endif

typedef uint16_t iclient_t;
typedef uint8_t messagetype_t;

#define MAX_SERVER_CLIENTS (1 << (sizeof(iclient_t) * 8))
#define SERVER_LISTEN_BUFFER_PER_CLIENT (8 * 1024)
#define MAX_SOCKET_MESSAGE_SIZE (1024)

bool InitNetwork(void);
void CleanupNetwork(void);

typedef struct ServerAddress {
    char ip[INET6_ADDRSTRLEN];
    uint16_t port;
    bool valid;
} ServerAddress;

ServerAddress ParseAddressFromString(const char *address);

typedef struct ServerClientData {
    socket_t socket;
    iclient_t index;
    bool active;
    // recv can return incomplete data
    // so this will save some between listen calls
    char *bufferedMessage;
    size_t bufferedMessageSize;
    size_t skipNextSize;
} ServerClientData;

typedef struct ServerInstance {
    socket_t socket;
    ServerClientData *clients;
    iclient_t maxClients;
    uint16_t port;
} ServerInstance;

typedef struct SocketMessageHeader {
    // stores length of data only, not including header
    size_t length;
    messagetype_t messageType;
} SocketMessageHeader;

typedef struct SocketMessage {
    struct SocketMessageHeader;
    char data[];
} SocketMessage;

// If `maxClients` = 0, then it set to `MAX_SERVER_CLIENTS`
// If `port` = 0, then available port chosen (based on OS)
ServerInstance *Server_Create(uint16_t port, iclient_t maxClients);
void Server_CleanUp(ServerInstance *server);

ServerClientData *Server_AcceptNewClient(ServerInstance *server);
void Server_Listen(ServerInstance *server);

bool Server_Broadcast(ServerInstance *server);
bool Server_SendTo(ServerInstance *server, ServerClientData *client, char *data, size_t len);

typedef struct ClientInstance {
    socket_t socket;
} ClientInstance;

ClientInstance Client_Create(ServerAddress address);
void Client_Listen(ServerInstance *server);
void Client_CleanUp(ClientInstance *client);

bool Client_Send();

void Debug_PrintMessage(char *buffer, size_t offset);

#endif