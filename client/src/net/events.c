#include <client.h>

void on_client_socket_connect(Client *client) {
    printf("connected to server! \n");
}

void on_client_socket_disconnect(Client *client) {
    printf("disconnected from server! \n");

}

void on_client_socket_error(Client *client, const int error) {
    printf("socket error %d \n", error);
}

void on_client_socket_event(Client *client) {

}