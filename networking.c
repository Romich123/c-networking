#include "networking.h"
#include <math.h>

bool InitNetwork(void) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
#endif
    return true;
}

void CleanupNetwork(void) {
#ifdef _WIN32
    WSACleanup();
#endif
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

static SocketMessage *TryParseMessage(char *buffer, uint64_t n, char **remainingBuffer, uint64_t *remainingSize) {
    *remainingBuffer = NULL;
    *remainingSize = 0;

    if (n < NETWORK_SOCKET_MESSAGE_HEADER)
        return NULL;

    SocketMessageHeader header;

    memcpy(&header.size, buffer, sizeof(header.size));
    memcpy(&header.messageType, buffer + sizeof(header.size), sizeof(messagetype_t));

    header.size = ntohll(header.size);
    header.messageType = ntohmt(header.messageType);

    uint64_t fullMessageSize = NETWORK_SOCKET_MESSAGE_HEADER + header.size;

    if (n < fullMessageSize)
        return NULL;

    SocketMessage *msg =
        malloc(sizeof(SocketMessageHeader) + header.size);

    if (!msg)
        return NULL;

    memcpy(msg, &header, sizeof(SocketMessageHeader));
    memcpy(msg->data, buffer + NETWORK_SOCKET_MESSAGE_HEADER, header.size);

    uint64_t leftover = n - fullMessageSize;

    if (leftover > 0) {
        *remainingBuffer = malloc(leftover);

        if (*remainingBuffer) {
            memcpy(*remainingBuffer, buffer + fullMessageSize, leftover);

            *remainingSize = leftover;
        }
    }

    return msg;
}

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

ServerInstance *Server_Create(uint16_t startPort, iclient_t maxClients) {
    socket_t server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (server_fd == INVALID_SOCKET)
        return NULL;

    SetNonBlocking(server_fd, true);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(startPort);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        CLOSE_SOCKET(server_fd);
        return NULL;
    }

    if (listen(server_fd, 3) == SOCKET_ERROR) {
        CLOSE_SOCKET(server_fd);
        return NULL;
    }

    ServerInstance *server = malloc(sizeof(ServerInstance));
    if (server == NULL) {
        CLOSE_SOCKET(server_fd);
        return NULL;
    }

    if (maxClients == 0) {
        maxClients = MAX_SERVER_CLIENTS;
    }

    server->clients = calloc(maxClients, sizeof(ServerClientData));

    if (server->clients == NULL) {
        CLOSE_SOCKET(server_fd);
        free(server);
        return NULL;
    }

    server->port = (uint16_t)ntohs(address.sin_port);
    server->maxClients = maxClients;
    server->socket = server_fd;
    server->activeClientsBefore = 0;

    return server;
}

void Server_CleanUp(ServerInstance *server) {
    for (iclient_t i = 0; i < server->activeClientsBefore; i++) {
        if (!server->clients[i].active)
            continue;

        Server_DisconnectClient(server, i);
    }

    free(server->clients);
    CLOSE_SOCKET(server->socket);
    free(server);
}

// accepts one client
// for multiple clients put this in a loop
// if no clients available returns null
ServerClientData *Server_AcceptNewClient(ServerInstance *server) {
    ServerClientData *clients = server->clients;

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);

    socket_t newSock = accept(server->socket, (struct sockaddr *)&addr, &addrLen);
    if (newSock == INVALID_SOCKET) {
        int err = GET_ERROR();

        if (err == WOULDBLOCK || err == EAGAIN) {
            return NULL;
        }

        return NULL;
    }

    SetNonBlocking(newSock, true);

    // find empty slot
    for (iclient_t i = 0; i < server->maxClients; i++) {
        if (!clients[i].active) {
            clients[i].socket = newSock;
            clients[i].index = i;
            clients[i].active = true;

            clients[i].recvBuffered = NULL;
            clients[i].recvBufferedSize = 0;
            clients[i].skipNextReceiveSize = 0;

            clients[i].sendBuffered = NULL;
            clients[i].sendBufferedSize = 0;

            if (i > server->activeClientsBefore)
                server->activeClientsBefore = i + 1;

            return &clients[i];
        }
    }

    // no slots available
    CLOSE_SOCKET(newSock);
    return NULL;
}

void Server_DisconnectClient(ServerInstance *server, iclient_t clientIndex) {
    ServerClientData *client = &server->clients[clientIndex];

    if (client->recvBuffered != NULL)
        free(client->recvBuffered);

    if (client->sendBuffered != NULL)
        free(client->sendBuffered);

    shutdown(client->socket, SHUT_WR);
    CLOSE_SOCKET(client->socket);
    client->recvBuffered = NULL;
    client->sendBuffered = NULL;
    client->recvBufferedSize = 0;
    client->skipNextReceiveSize = 0;
    client->active = false;
}

// returns one message
// for multiple messages put this in a loop
// if no message available returns null
SocketMessage *Server_ListenToClient(ServerInstance *server, iclient_t clientIndex) {
    ServerClientData *client = &server->clients[clientIndex];

    char buffer[SERVER_LISTEN_BUFFER_PER_CLIENT];

    uint64_t n;
    int recvResult;

    if (client->recvBuffered != NULL) {
        char *remaining = NULL;
        uint64_t remainingSize = 0;

        SocketMessage *msg = TryParseMessage(client->recvBuffered, client->recvBufferedSize, &remaining, &remainingSize);

        if (msg != NULL) {
            free(client->recvBuffered);

            client->recvBuffered = remaining;
            client->recvBufferedSize = remainingSize;

            return msg;
        }

        // something very wrong happened
        // probably user change data directly
        if (client->recvBufferedSize > sizeof(buffer)) {
            free(client->recvBuffered);
            client->recvBuffered = NULL;
            client->recvBufferedSize = 0;
            return NULL;
        }

        recvResult = recv(client->socket, (char *)buffer + client->recvBufferedSize, sizeof(buffer) - client->recvBufferedSize, 0);

        if (recvResult == 0) {
            Server_DisconnectClient(server, clientIndex);
            return NULL;
        } else if (recvResult == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err != WOULDBLOCK && err != EAGAIN) {
                Server_DisconnectClient(server, clientIndex);
                return NULL;
            }

            // buffered message is incomplete
            // so if nothing new is received then it is still incomplete
            return NULL;
        }

        memcpy(buffer, client->recvBuffered, client->recvBufferedSize);

        n = client->recvBufferedSize + recvResult;

        free(client->recvBuffered);
        client->recvBuffered = NULL;
        client->recvBufferedSize = 0;
    } else {
        recvResult = recv(client->socket, buffer, sizeof(buffer), 0);

        if (recvResult == 0) {
            Server_DisconnectClient(server, clientIndex);
            return NULL;
        } else if (recvResult == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err != WOULDBLOCK && err != EAGAIN) {
                Server_DisconnectClient(server, clientIndex);
                return NULL;
            }

            // buffered message is incomplete
            // so if nothing new is received then it is still incomplete
            return NULL;
        }

        n = recvResult;
    }

    uint64_t offset = client->skipNextReceiveSize;

    if (offset >= n) {
        client->skipNextReceiveSize -= n;
        return NULL;
    }
    client->skipNextReceiveSize = 0;

    if (offset + NETWORK_SOCKET_MESSAGE_HEADER > n) {
        uint64_t bufferSize = n - offset;

        client->recvBuffered = malloc(bufferSize);
        if (client->recvBuffered == NULL) {
            printf("Couldn't allocate %zu bytes\n", bufferSize);
            return NULL;
        }
        memcpy(client->recvBuffered, &buffer[offset], bufferSize);

        client->recvBufferedSize = bufferSize;
        return NULL;
    }

    SocketMessageHeader header;
    memcpy(&header.size, &buffer[offset], sizeof(header.size));
    memcpy(&header.messageType, &buffer[offset + sizeof(header.size)], sizeof(messagetype_t));

    header.size = ntohll(header.size);
    header.messageType = ntohmt(header.messageType);

    if (header.size > SIZE_MAX - NETWORK_SOCKET_MESSAGE_HEADER) {
        Server_DisconnectClient(server, clientIndex);
        return NULL;
    }

    uint64_t fullMessageSize = header.size + NETWORK_SOCKET_MESSAGE_HEADER;

    // couldn't possible fit this
    if (fullMessageSize > sizeof(buffer)) {
        //                     full msg size - already read part
        client->skipNextReceiveSize = fullMessageSize - (sizeof(buffer) - offset);
        printf("!!! Message too big (length=%zu) from client index=%d\n", fullMessageSize, client->index);
        return NULL;
    }

    // message outside of bounds of buffer
    if (offset + fullMessageSize > n) {
        uint64_t bufferSize = n - offset;

        client->recvBuffered = malloc(bufferSize);

        if (client->recvBuffered == NULL) {
            printf("Couldn't allocate %zu bytes\n", bufferSize);
            return NULL;
        }

        memcpy(client->recvBuffered, &buffer[offset], bufferSize);

        client->recvBufferedSize = bufferSize;
        return NULL;
    }

    SocketMessage *result = malloc(sizeof(SocketMessageHeader) + header.size);
    if (result == NULL) {
        printf("Couldn't allocate %zu bytes\n", sizeof(SocketMessageHeader) + header.size);
        return NULL;
    }

    memcpy(result, &header, sizeof(SocketMessageHeader));
    memcpy(result->data, &buffer[offset + sizeof(SocketMessageHeader)], header.size);

    offset += fullMessageSize;

    // data left
    if (offset < n) {
        uint64_t bufferSize = n - offset;

        client->recvBuffered = malloc(bufferSize);
        if (client->recvBuffered == NULL) {
            printf("Couldn't allocate %zu bytes\n", bufferSize);
            free(result);
            return NULL;
        }
        memcpy(client->recvBuffered, &buffer[offset], bufferSize);

        client->recvBufferedSize = bufferSize;
    }

    return result;
}

// the same as `Server_ListenToClient`
// goes from first to last client and returns first message
SocketMessageFromClient Server_Listen(ServerInstance *server) {
    for (iclient_t i = 0; i < server->activeClientsBefore; i++) {
        if (!server->clients[i].active)
            continue;

        SocketMessage *message = Server_ListenToClient(server, i);

        if (message != NULL)
            return (SocketMessageFromClient){
                .clientIndex = i,
                .message = message};
    }

    return (SocketMessageFromClient){
        .clientIndex = 0,
        .message = NULL};
}

// this function fully trusts `size` given
// so you should check it
int Server_SendTo(ServerInstance *server, iclient_t clientIndex, uint64_t size, messagetype_t msgType, const char *message) {
    uint64_t fullMessageSize = NETWORK_SOCKET_MESSAGE_HEADER + size;

    ServerClientData *client = &server->clients[clientIndex];
    socket_t socket = server->clients[clientIndex].socket;

    uint64_t networkSize = htonll(size);
    msgType = htonmt(msgType);

    uint64_t bufferSize;
    char *buffer;

    if (client->sendBuffered != NULL) {
        bufferSize = client->sendBufferedSize + fullMessageSize;

        buffer = malloc(bufferSize);

        if (buffer == NULL) {
            printf("Couldn't allocate %llu bytes\n", client->sendBufferedSize + fullMessageSize);
            return ENOMEM;
        }

        memcpy(buffer, client->sendBuffered, client->sendBufferedSize);
        memcpy(buffer + client->sendBufferedSize, &networkSize, sizeof(networkSize));
        memcpy(buffer + client->sendBufferedSize + sizeof(networkSize), &msgType, sizeof(msgType));
        memcpy(buffer + client->sendBufferedSize + sizeof(networkSize) + sizeof(msgType), message, size);

        free(client->sendBuffered);
        client->sendBuffered = NULL;
        client->sendBufferedSize = 0;
    } else {
        bufferSize = fullMessageSize;
        buffer = malloc(bufferSize);

        if (buffer == NULL) {
            printf("Couldn't allocate %llu bytes\n", fullMessageSize);
            return ENOMEM;
        }

        memcpy(buffer, &networkSize, sizeof(networkSize));
        memcpy(buffer + sizeof(networkSize), &msgType, sizeof(msgType));
        memcpy(buffer + sizeof(networkSize) + sizeof(msgType), message, size);
    }

    uint64_t sent = 0;
    while (sent < bufferSize) {
        int result = send(socket, (buffer + sent), bufferSize - sent, 0);

        if (result > 0) {
            sent += result;
        } else if (result == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err == WOULDBLOCK || err == EAGAIN) {
                uint64_t sizeLeft = bufferSize - sent;
                char *bufferedSend = malloc(sizeLeft);

                if (bufferedSend == NULL) {
                    printf("Couldn't allocate %llu bytes\n", sizeLeft);
                    free(buffer);
                    return ENOMEM;
                }

                memcpy(bufferedSend, buffer + sent, sizeLeft);
                free(buffer);

                client->sendBuffered = bufferedSend;
                client->sendBufferedSize = sizeLeft;

                return WOULDBLOCK;
            }

            free(buffer);
            return err;
        }
    }

    free(buffer);

    return 0;
}

int Server_Broadcast(ServerInstance *server, uint64_t size, messagetype_t msgType, const char *message) {
    int lastError = 0;

    for (iclient_t i = 0; i < server->activeClientsBefore; i++) {
        if (!server->clients[i].active)
            continue;

        int error = Server_SendTo(server, i, size, msgType, message);

        if (error != 0) {
            lastError = error;
        }
    }

    return lastError;
}

ClientInstance *Client_Create(ServerAddress address) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        return NULL;
    }

    if (!SetNonBlocking(sock, true)) {
        CLOSE_SOCKET(sock);
        return NULL;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(address.port);

    if (inet_pton(AF_INET, address.ip, &server_addr.sin_addr) <= 0) {
        CLOSE_SOCKET(sock);
        return NULL;
    }

    int connectResult = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));

    int err = GET_ERROR();
    if (connectResult == 0 || err == INPROGRESS || err == WOULDBLOCK) {
        ClientInstance *result = calloc(1, sizeof(ClientInstance));
        result->socket = sock;

        return result;
    }

    CLOSE_SOCKET(sock);
    return NULL;
}

void Client_CleanUp(ClientInstance *client) {
    if (client->recvBuffered != NULL)
        free(client->recvBuffered);

    if (client->sendBuffered != NULL)
        free(client->sendBuffered);

    CLOSE_SOCKET(client->socket);
    free(client);
}

// returns one message
// for multiple messages put this in a loop
// if no message available returns null
SocketMessage *Client_Listen(ClientInstance *client) {
    char buffer[CLIENT_LISTEN_BUFFER];

    uint64_t n;
    int recvResult;

    if (client->recvBuffered != NULL) {
        char *remaining = NULL;
        uint64_t remainingSize = 0;

        SocketMessage *msg = TryParseMessage(client->recvBuffered, client->recvBufferedSize, &remaining, &remainingSize);

        if (msg != NULL) {
            free(client->recvBuffered);

            client->recvBuffered = remaining;
            client->recvBufferedSize = remainingSize;

            return msg;
        }

        if (client->recvBufferedSize > sizeof(buffer)) {
            free(client->recvBuffered);

            client->recvBuffered = NULL;
            client->recvBufferedSize = 0;
            return NULL;
        }

        recvResult = recv(client->socket, buffer + client->recvBufferedSize, sizeof(buffer) - client->recvBufferedSize, 0);

        if (recvResult == 0) {
            return NULL;
        } else if (recvResult == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err != WOULDBLOCK && err != EAGAIN) {
                return NULL;
            }

            return NULL;
        }

        memcpy(buffer, client->recvBuffered, client->recvBufferedSize);

        n = client->recvBufferedSize + recvResult;

        free(client->recvBuffered);
        client->recvBuffered = NULL;
        client->recvBufferedSize = 0;
    } else {
        recvResult = recv(client->socket, buffer, sizeof(buffer), 0);

        if (recvResult == 0) {
            return NULL;
        } else if (recvResult == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err != WOULDBLOCK && err != EAGAIN) {
                return NULL;
            }

            return NULL;
        }

        n = recvResult;
    }

    uint64_t offset = client->skipNextReceiveSize;

    if (offset >= n) {
        client->skipNextReceiveSize -= n;
        return NULL;
    }

    client->skipNextReceiveSize = 0;

    if (offset + NETWORK_SOCKET_MESSAGE_HEADER > n) {
        uint64_t bufferSize = n - offset;

        client->recvBuffered = malloc(bufferSize);
        if (client->recvBuffered == NULL) {
            printf("Couldn't allocate %zu bytes\n", bufferSize);
            return NULL;
        }

        memcpy(client->recvBuffered, &buffer[offset], bufferSize);
        client->recvBufferedSize = bufferSize;

        return NULL;
    }

    SocketMessageHeader header;
    memcpy(&header.size, &buffer[offset], sizeof(header.size));
    memcpy(&header.messageType, &buffer[offset + sizeof(header.size)], sizeof(messagetype_t));

    header.size = ntohll(header.size);
    header.messageType = ntohmt(header.messageType);

    if (header.size > SIZE_MAX - NETWORK_SOCKET_MESSAGE_HEADER) {
        return NULL;
    }

    uint64_t fullMessageSize =
        header.size + NETWORK_SOCKET_MESSAGE_HEADER;

    if (fullMessageSize > sizeof(buffer)) {
        client->skipNextReceiveSize = fullMessageSize - (sizeof(buffer) - offset);

        printf("!!! Message too big (length=%zu)\n", (size_t)fullMessageSize);

        return NULL;
    }

    if (offset + fullMessageSize > n) {
        uint64_t bufferSize = n - offset;

        client->recvBuffered = malloc(bufferSize);

        if (client->recvBuffered == NULL) {
            printf("Couldn't allocate %zu bytes\n", bufferSize);
            return NULL;
        }

        memcpy(client->recvBuffered, &buffer[offset], bufferSize);
        client->recvBufferedSize = bufferSize;

        return NULL;
    }

    SocketMessage *result =
        malloc(sizeof(SocketMessageHeader) + header.size);

    if (result == NULL) {
        printf("Couldn't allocate %zu bytes\n", sizeof(SocketMessageHeader) + (size_t)header.size);
        return NULL;
    }

    memcpy(result, &header, sizeof(SocketMessageHeader));
    memcpy(result->data, &buffer[offset + sizeof(SocketMessageHeader)], header.size);

    offset += fullMessageSize;

    if (offset < n) {
        uint64_t bufferSize = n - offset;

        client->recvBuffered = malloc(bufferSize);

        if (client->recvBuffered == NULL) {
            printf("Couldn't allocate %zu bytes\n", bufferSize);
            free(result);
            return NULL;
        }

        memcpy(client->recvBuffered, &buffer[offset], bufferSize);
        client->recvBufferedSize = bufferSize;
    }

    return result;
}

int Client_Send(ClientInstance *client, uint64_t size, messagetype_t msgType, const char *message) {
    uint64_t fullMessageSize = NETWORK_SOCKET_MESSAGE_HEADER + size;

    uint64_t networkSize = htonll(size);
    msgType = htonmt(msgType);

    uint64_t bufferSize;
    char *buffer;

    if (client->sendBuffered != NULL) {
        bufferSize = client->sendBufferedSize + fullMessageSize;

        buffer = malloc(bufferSize);

        if (buffer == NULL) {
            printf("Couldn't allocate %llu bytes\n", (unsigned long long)bufferSize);
            return ENOMEM;
        }

        memcpy(buffer, client->sendBuffered, client->sendBufferedSize);
        memcpy(buffer + client->sendBufferedSize, &networkSize, sizeof(networkSize));
        memcpy(buffer + client->sendBufferedSize + sizeof(networkSize), &msgType, sizeof(msgType));
        memcpy(buffer + client->sendBufferedSize + sizeof(networkSize) + sizeof(msgType), message, size);

        free(client->sendBuffered);
        client->sendBuffered = NULL;
    } else {
        bufferSize = fullMessageSize;

        buffer = malloc(bufferSize);

        if (buffer == NULL) {
            printf("Couldn't allocate %llu bytes\n", (unsigned long long)fullMessageSize);
            return ENOMEM;
        }

        memcpy(buffer, &networkSize, sizeof(networkSize));
        memcpy(buffer + sizeof(networkSize), &msgType, sizeof(msgType));
        memcpy(buffer + sizeof(networkSize) + sizeof(msgType), message, size);
    }

    uint64_t sent = 0;

    while (sent < bufferSize) {
        int result = send(client->socket, buffer + sent, bufferSize - sent, 0);

        if (result > 0) {
            sent += result;
        } else if (result == SOCKET_ERROR) {
            int err = GET_ERROR();

            if (err == WOULDBLOCK || err == EAGAIN) {
                uint64_t sizeLeft = bufferSize - sent;

                char *bufferedSend = malloc(sizeLeft);

                if (bufferedSend == NULL) {
                    free(buffer);
                    return ENOMEM;
                }

                memcpy(bufferedSend, buffer + sent, sizeLeft);

                free(buffer);

                client->sendBuffered = bufferedSend;
                client->sendBufferedSize = sizeLeft;

                return WOULDBLOCK;
            }

            free(buffer);
            return err;
        }
    }

    free(buffer);
    return 0;
}