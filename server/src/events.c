#include <server.h>

void on_client_connect(ClientConnection *client) {
    replxx_print(g_replxx, "%s connected from %s \n", client->name, inet_ntoa(client->addr.sin_addr));
}

void on_client_disconnect(ClientConnection *client) {
    replxx_print(g_replxx, "%s disconnected \n", client->name[0] ? client->name : "client");
}

void on_client_event(ClientConnection *client) {

}
