#pragma once

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

typedef struct {
    socket_t sock;
    uint32_t id;
    bool active;
} Client;

typedef struct {
    size_t length;
    char type;
} MessageHeader;

#define MAX_SERVER_CLIENTS 8
Client clients[MAX_SERVER_CLIENTS];
uint32_t next_client_id = 1;

bool InitNetwork(void) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
#endif
    return true;
}

static bool SetNonBlocking(socket_t sock, bool nonblocking) {
#ifdef _WIN32
    u_long mode = nonblocking ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0)
        return false;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

void ServerBroadcast(const void *data, size_t len) {
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (clients[i].active) {
            send(clients[i].sock, data, len, 0);
        }
    }
}

// i don't know what i am doing
typedef struct SocketMessage {
    char *message;
    size_t messageSize;
    // if it is client socket, then this set to 0
    uint32_t clientId;
} SocketMessage;

typedef struct SocketMessagesDynArray {
    SocketMessage *messages;
    uint32_t length;
    uint32_t capacity;
} SocketMessagesDynArray;

static SocketMessagesDynArray socketMessages = {0};

static bool _EnsureSocketMessagesCapacity(uint32_t needed) {
    if (needed <= socketMessages.capacity)
        return true;

    uint32_t new_capacity = socketMessages.capacity == 0 ? 8 : socketMessages.capacity * 2;
    while (new_capacity < needed)
        new_capacity *= 2;

    SocketMessage *new_array = realloc(socketMessages.messages,
                                       new_capacity * sizeof(SocketMessage));
    if (!new_array)
        return false;

    socketMessages.messages = new_array;
    socketMessages.capacity = new_capacity;
    return true;
}

static bool PushServerMessage(const char *data, size_t size) {
    if (!_EnsureSocketMessagesCapacity(socketMessages.length + 1))
        return false;

    char *copy = malloc(size);
    if (!copy)
        return false;

    memcpy(copy, data, size);

    SocketMessage *msg = &(socketMessages.messages[socketMessages.length++]);
    msg->clientId = 0;
    msg->message = copy;
    msg->messageSize = size;

    return true;
}

static bool PushClientMessage(uint32_t clientId, const char *data, size_t size) {
    if (!_EnsureSocketMessagesCapacity(socketMessages.length + 1))
        return false;

    char *copy = malloc(size);
    if (!copy)
        return false;

    memcpy(copy, data, size);

    SocketMessage *msg = &(socketMessages.messages[socketMessages.length++]);
    msg->clientId = clientId;
    msg->message = copy;
    msg->messageSize = size;

    return true;
}

socket_t CreateServer(int port) {
    socket_t server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd == INVALID_SOCKET)
        return INVALID_SOCKET;

    SetNonBlocking(server_fd, true);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        CLOSE_SOCKET(server_fd);
        return INVALID_SOCKET;
    }

    if (listen(server_fd, 3) == SOCKET_ERROR) {
        CLOSE_SOCKET(server_fd);
        return INVALID_SOCKET;
    }

    return server_fd;
}

void AcceptNewClient(socket_t server_fd) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    socket_t new_sock = accept(server_fd, (struct sockaddr *)&addr, &addr_len);
    if (new_sock == INVALID_SOCKET) {
        int err = GET_ERROR();
        // === THIS CHECK ONLY WORKS IF NON-BLOCKING ===
        if (err == WOULDBLOCK || err == EAGAIN) {
            return; // No pending connections, that's fine
        }

        return;
    }

    SetNonBlocking(new_sock, true);

    // Find empty slot
    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].sock = new_sock;
            clients[i].id = next_client_id++;
            clients[i].active = true;

            printf("Client #%u connected from %s:%d\n",
                   clients[i].id,
                   inet_ntoa(addr.sin_addr),
                   ntohs(addr.sin_port));

            char sdata[3];
            sdata[0] = sizeof(sdata);
            sdata[1] = CLIENT_UPDATE_ID;
            sdata[2] = (char)clients[i].id;
            send(new_sock, sdata, sizeof(sdata), 0);

            // kinda a hack to notify client that server player exists
            // i don't want to assume that server is also a player
            // because it disallows creating just a server without a client
            // probably wouldn't do that, but there is possibility
            sdata[0] = sizeof(sdata);
            sdata[1] = CLIENT_CONNECTED;
            sdata[2] = 0;
            send(new_sock, sdata, sizeof(sdata), 0);

            sdata[0] = sizeof(sdata);
            sdata[1] = CLIENT_CONNECTED;
            sdata[2] = (char)clients[i].id;
            ServerBroadcast(sdata, sizeof(sdata));

            char selfMessage[1 + 1 + sizeof(socket_t)];
            selfMessage[0] = CLIENT_CONNECTED;
            selfMessage[1] = (char)clients[i].id;
            *(socket_t *)(selfMessage + 2) = new_sock;
            PushClientMessage(clients[i].id, selfMessage, sizeof(selfMessage));

            return;
        }
    }

    // No slots available
    printf("Server full, rejecting connection\n");
    CLOSE_SOCKET(new_sock);
}

socket_t ConnectToServerNonBlocking(const char *address) {
    char ip[INET6_ADDRSTRLEN];
    int port;

    if (sscanf(address, "%15[^:]:%d", ip, &port) != 2) {
        return INVALID_SOCKET;
    }

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    // Set non-blocking BEFORE connect
    if (!SetNonBlocking(sock, true)) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }

    int result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    if (result == 0) {
        // Connected immediately (rare, but possible for localhost)
        return sock;
    }

    int err = GET_ERROR();
    if (err == INPROGRESS || err == WOULDBLOCK) {
        // Normal case: connection in progress
        return sock;
    }

    // Real error
    CLOSE_SOCKET(sock);
    return INVALID_SOCKET;
}

typedef enum {
    SOCKET_ERRORED = -1,
    SOCKET_PENDING = 0,
    SOCKET_CONNECTED = 1
} SocketStatus;

SocketStatus IsSocketConnected(socket_t sock) {
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);

    struct timeval tv = {0, 0}; // Non-blocking check (poll)
    int ret = select((int)sock + 1, NULL, &writefds, NULL, &tv);

    if (ret < 0) {
        return SOCKET_ERRORED; // select() error
    }

    if (ret == 0) {
        return SOCKET_PENDING; // Not ready yet
    }

    // Socket is writable - but was it an error or success?
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len) < 0) {
        return -1;
    }

    if (so_error != 0) {
#ifdef _WIN32
        WSASetLastError(so_error);
#else
        errno = so_error;
#endif
        return SOCKET_ERRORED; // Connection failed (so_error has the actual error code)
    }

    return SOCKET_CONNECTED; // Success!
}

// zero is cool
// rename because i confuse it with broadcast
int SendDataWithType(socket_t sock, char messageType, const void *data, size_t len) {
    char *buffer = malloc(sizeof(size_t) + 1 + len);

    if (buffer == NULL) {
        return -1;
    }

    memcpy(buffer + sizeof(size_t) + 1, data, len);
    *(size_t *)(buffer) = len;
    *(char *)((size_t *)(buffer) + 1) = messageType;

    size_t sent = 0;

    len += sizeof(size_t) + 1;
    while (sent < len) {
        int result = send(sock, buffer + sent, (int)(len - sent), 0);

        if (result > 0) {
            sent += result;
        } else if (result == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err == WOULDBLOCK || err == EAGAIN) {
                free(buffer);
                return WOULDBLOCK;
            }

            free(buffer);
            return err;
        }
    }

    free(buffer);
    return 0;
}

static void ClearSocketMessages(void) {
    for (uint32_t i = 0; i < socketMessages.length; i++) {
        free(socketMessages.messages[i].message);
        socketMessages.messages[i].message = NULL;
    }
    socketMessages.length = 0;
}

void ServerSocketUpdate(socket_t server_fd) {
    ClearSocketMessages();

    AcceptNewClient(server_fd);

    for (int i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (!clients[i].active)
            continue;

        char buffer[1024 * 8];
        int n = recv(clients[i].sock, buffer, sizeof(buffer), 0);

        if (n > 0) {
            int offset = 0;
            while (offset < n) {
                size_t length = *((size_t *)(buffer + offset));

                // we ignore the first byte
                // because it is just for size
                PushClientMessage(clients[i].id, &(buffer[offset + sizeof(size_t) + 1]), length - sizeof(size_t) - 1);

                offset += length;
            }
        } else if (n == 0) {
            printf("Client #%u disconnected\n", clients[i].id);
            shutdown(clients[i].sock, SHUT_WR);
            CLOSE_SOCKET(clients[i].sock);
            clients[i].active = false;

            // It is always true, but it looks clearer
        } else if (n == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err != WOULDBLOCK && err != EAGAIN) {
                printf("Client #%u error: %d\n", clients[i].id, err);
                CLOSE_SOCKET(clients[i].sock);
                clients[i].active = false;
            }
            // WOULDBLOCK/EAGAIN = no data, normal for non-blocking
        }
    }
}

// true - everythin is cool, false - ? you decide really
bool ClientSocketUpdate(socket_t client_fd) {
    // xd
    ClearSocketMessages();

    char buffer[1024];
    int n = recv(client_fd, buffer, sizeof(buffer), 0);

    if (n > 0) {
        int offset = 0;
        while (offset < n) {
            size_t length = *((size_t *)(buffer + offset));

            // we ignore the first byte
            // because it is just for size
            PushServerMessage(&(buffer[offset + 1]), (size_t)(length - 1));

            offset += length;
        }

        return true;
    } else if (n == 0) {
        printf("Server disconnected\n");
        CLOSE_SOCKET(client_fd);

        // It is always true, but it looks clearer
    } else if (n == SOCKET_ERROR) {
        int err = GET_ERROR();

        if (err == WOULDBLOCK || err == EAGAIN) {
            // WOULDBLOCK/EAGAIN = no data, normal for non-blocking
            return true;
        }

        printf("Server error: %d\n", err);
        CLOSE_SOCKET(client_fd);
    }
    return false;
}

void _ShutdownSocketMessages(void) {
    ClearSocketMessages();
    free(socketMessages.messages);
    socketMessages.messages = NULL;
    socketMessages.capacity = 0;
}

void CleanupNetwork(void) {
    _ShutdownSocketMessages();
#ifdef _WIN32
    WSACleanup();
#endif
}