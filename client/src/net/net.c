#ifdef WIN32
#include <winsock2.h>
#define NET_ERRNO WSAGetLastError()
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#define NET_ERRNO errno
#endif

#include <client.h>

int net_poll(Client *client, const int socket) {
    char buffer[1024];
    int read;

    while ((read = recv(socket, buffer, sizeof(buffer), 0)) > 0) {
        // TODO: process data
    }

#ifdef WIN32
    if (!read || read < 0 && NET_ERRNO == WSAEWOULDBLOCK) return 1;
#else
    if (!read || read < 0 && (NET_ERRNO == EAGAIN || NET_ERRNO == EWOULDBLOCK)) return 1;
#endif

    return 0;
}

void net_main(NetMainArgs *args) {
#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    struct sockaddr_in server_address;

    // TODO: better address parsing
    server_address.sin_addr.s_addr = inet_addr(args->address);
    server_address.sin_port = htons(21015);
    server_address.sin_family = AF_INET;

    const int client_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (client_socket < 0 || connect(client_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0) {
        on_client_socket_error(args->client, NET_ERRNO);
        return;
    }

#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(client_socket, FIONBIO, &non_blocking);
#else
    int flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    on_client_socket_connect(args->client);

    while (1) {
        if (!net_poll(args->client, client_socket)) break;
    }

    on_client_socket_disconnect(args->client);
}
