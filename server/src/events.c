#include <server.h>
#include <server_log.h>

void on_client_connect(ClientConnection *client) {
    server_log("%s connected from %s", client->name, inet_ntoa(client->addr.sin_addr));
}

void on_client_disconnect(ClientConnection *client) {
    server_log("%s disconnected", client->name[0] ? client->name : "client");
}

void on_client_event(ClientConnection *client) {

}
