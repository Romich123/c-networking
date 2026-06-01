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

ServerAddress ParseAddressFromString(const char *address) {
    ServerAddress result = {0};

    char *end;
    while (address)
        result.valid = sscanf(address, "%15[^:]:%d", result.ip, &result.port) == 2;
    return result;
}

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

    ServerInstance *server = calloc(sizeof(ServerInstance), 1);
    if (server == NULL) {
        return NULL;
    }

    if (maxClients == 0) {
        maxClients = MAX_SERVER_CLIENTS;
    }

    server->port = (uint16_t)ntohs(address.sin_port);
    server->maxClients = maxClients;
    server->socket = server_fd;
    server->clients = calloc(maxClients, sizeof(ServerClientData));

    if (server->clients == NULL) {
        free(server);
        return NULL;
    }

    return server;
}

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
    for (iclient_t i = 0; i < MAX_SERVER_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].socket = newSock;
            clients[i].index = i;
            clients[i].active = true;
            clients[i].bufferedMessage = NULL;

            return &clients[i];
        }
    }

    // no slots available
    CLOSE_SOCKET(newSock);
    return NULL;
}

SocketMessage *Server_ListenToClient(ServerInstance *server, iclient_t clientIndex) {
    ServerClientData *client = &server->clients[clientIndex];

    char buffer[SERVER_LISTEN_BUFFER_PER_CLIENT];

    int n;
    if (client->bufferedMessage != NULL) {
        memcpy(buffer, client->bufferedMessage, client->bufferedMessageSize);

        n = client->bufferedMessageSize;
        n += recv(client->socket, (char *)buffer + client->bufferedMessageSize, sizeof(buffer) - client->bufferedMessageSize, 0);

        free(client->bufferedMessage);
        client->bufferedMessage = NULL;
        client->bufferedMessageSize = 0;
    } else {
        n = recv(client->socket, buffer, sizeof(buffer), 0);
    }

    if (n > 0) {
        int offset = client->skipNextSize;
        client->skipNextSize = 0;

        while (offset < n) {
            SocketMessageHeader header = *(SocketMessageHeader *)&(buffer[offset]);
            header.length = ntohll(header.length);
            header.messageType = (messagetype_t)ntohll((uint64_t)header.messageType);

            size_t fullLength = header.length + sizeof(SocketMessageHeader);

            // couldn't possible fit this
            if (fullLength > SERVER_LISTEN_BUFFER_PER_CLIENT) {
                //                    full msg size - already read part
                client->skipNextSize = fullLength - (SERVER_LISTEN_BUFFER_PER_CLIENT - offset);
                printf("!!! Message too big (length=%d) from client index=%d", fullLength, client->index);
                return NULL;
            }

            // message outside of bounds of buffer
            if (offset + fullLength > SERVER_LISTEN_BUFFER_PER_CLIENT) {
                size_t bufferSize = SERVER_LISTEN_BUFFER_PER_CLIENT - (fullLength + offset);

                client->bufferedMessage = malloc(bufferSize);
                if (client->bufferedMessage == NULL) {
                    printf("Could allocate %d bytes", bufferSize);
                    return NULL;
                }
                memcpy(client->bufferedMessage, &buffer[offset], bufferSize);

                client->bufferedMessageSize = bufferSize;
                return NULL;
            }

            SocketMessage *result = malloc(sizeof(SocketMessageHeader) + header.length);
            if (result == NULL) {
                printf("Could allocate %d bytes", sizeof(SocketMessageHeader) + header.length);
                return NULL;
            }

            memcpy(result, &header, sizeof(SocketMessageHeader));
            memcpy(result->data, &buffer[offset], header.length);

            offset += fullLength;
        }

    } else if (n == 0) {
        shutdown(client->socket, SHUT_WR);
        CLOSE_SOCKET(client->socket);
        client->active = false;

        // It is always true, but it looks clearer
    } else if (n == SOCKET_ERROR) {
        int err = GET_ERROR();

        if (err != WOULDBLOCK && err != EAGAIN) {
            printf("Client #%u error: %d\n", client->index, err);
            CLOSE_SOCKET(client->socket);
            client->active = false;
        }
    }
}

void Server_CleanUp(ServerInstance *server) {
}

void Server_Listen(ServerInstance *server) {
    for (iclient_t i = 0; i < server->maxClients; i++) {
        if (!server->clients[i].active)
            continue;

        Server_ListenToClient(server, i);
    }
}