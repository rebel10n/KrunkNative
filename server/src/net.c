#include <server.h>
#include <errno.h>
#include <string.h>

#ifdef WIN32
#define NET_ERRNO WSAGetLastError()
#else
#include <unistd.h>
#define NET_ERRNO errno
#endif

#define NET_ERROR(...) { replxx_print(g_replxx, __VA_ARGS__); g_server->should_quit = 1; return; }

void net_add_client(ClientConnection *client) {
    ClientConnection *new_clients = realloc(g_server->clients, (g_server->client_count + 1) * sizeof(ClientConnection));

    if (!new_clients) {
#ifdef WIN32
        closesocket(client->fd);
#else
        close(client->fd);
#endif

        free(client);

        return;
    }

    g_server->clients = new_clients;
    g_server->clients[g_server->client_count++] = *client;

    on_client_connect(client);
}

void net_remove_client(ClientConnection *client) {
    size_t index = -1;

    for (size_t i = 0; i < g_server->client_count; i++) {
        if (&g_server->clients[i] == client) {
            index = i;
            break;
        }
    }

    if (index == -1) return;

    on_client_disconnect(client);

    memcpy(&g_server->clients[index], &g_server->clients[index + 1], (g_server->client_count - index + 1) * sizeof(ClientConnection));
    g_server->client_count--;

    ClientConnection *new_clients = realloc(g_server->clients, (g_server->client_count - 1) * sizeof(ClientConnection));
    if (!new_clients) return;

    g_server->clients = new_clients;
}

int net_poll_client(ClientConnection *client) {
    char buffer[1024];
    int read;

    while ((read = recv(client->fd, buffer, sizeof(buffer), 0)) > 0) {
        // TODO: process data
    }

#ifdef WIN32
    if (!read || read < 0 && NET_ERRNO == WSAEWOULDBLOCK) return 1;
#else
    if (!read || read < 0 && (NET_ERRNO == EAGAIN || NET_ERRNO == EWOULDBLOCK)) return 1;
#endif

    net_remove_client(client);
    return 0;
}

void net_main() {

#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    const int server_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket < 0) NET_ERROR("Failed to create server socket, quitting...");

    struct sockaddr_in listen_addr;

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(g_server->config.listen_port);

    if (bind(server_socket, (struct sockaddr *) &listen_addr, sizeof(listen_addr)) < 0) {
        NET_ERROR("Failed to bind to port %d (is it in use?), quitting...", g_server->config.listen_port);
    }

    if (listen(server_socket, 5) < 0) NET_ERROR("Failed to listen on port %d, quitting...", g_server->config.listen_port);

#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(server_socket, FIONBIO, &non_blocking);
#else
    int flags = fcntl(server_socket, F_GETFL, 0);
    fcntl(server_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    replxx_print(g_replxx, "Listening on port %d \n", g_server->config.listen_port);

    while (1) {
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (!net_poll_client(&g_server->clients[i])) i--;
        }

        struct sockaddr_in client_addr;
        int client_addr_size = sizeof(client_addr);

        const int client = (int) accept(server_socket, (struct  sockaddr *) &client_addr, &client_addr_size);

        if (client < 0) {
#ifdef WIN32
            if (NET_ERRNO == WSAEWOULDBLOCK) continue;
#else
            if (NET_ERRNO == EAGAIN || NET_ERRNO == EWOULDBLOCK) continue;
#endif

            break;
        }

        ClientConnection conn = {};

        conn.fd = client;
        conn.addr = client_addr;

        net_add_client(&conn);
    }
}

void net_cleanup() {
#ifdef WIN32
    WSACleanup();
#endif
}