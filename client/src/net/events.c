#include <client.h>

void on_client_socket_connect(Client *client) {
    pthread_mutex_lock(&client->net_lock);
    client->net_connected = 1;
    pthread_mutex_unlock(&client->net_lock);

    printf("connected to server! \n");
}

void on_client_socket_disconnect(Client *client) {
    pthread_mutex_lock(&client->net_lock);
    client->net_connected = 0;
    pthread_mutex_unlock(&client->net_lock);

    printf("disconnected from server! \n");

}

void on_client_socket_error(Client *client, const int error) {
    printf("socket error %d \n", error);
}

void on_client_socket_event(Client *client) {

}
