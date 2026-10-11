// Transport -> client connection events. The native TCP transport (net.c) calls
// these from its background thread, the web transport (net_web.c) from browser
// callbacks, so they must not touch the HTML UI or game state: they record a
// NetStatus under net_lock and client_menu_tick() (main thread) turns it into
// net_connected updates and the page's connection state (menu_ui.c).
#include <client.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#include <errno.h>
#endif

static void net_set_status_code(Client *client, const NetStatus status, const char *error, const char *code) {
    pthread_mutex_lock(&client->net_lock);
    client->net_status = status;
    snprintf(client->net_status_code, sizeof(client->net_status_code), "%s", code ? code : "");
    strncpy(client->net_status_error, error ? error : "", sizeof(client->net_status_error) - 1);
    client->net_status_error[sizeof(client->net_status_error) - 1] = 0;
    pthread_mutex_unlock(&client->net_lock);
}

static void net_set_status(Client *client, const NetStatus status, const char *error) {
    net_set_status_code(client, status, error, status == NET_STATUS_FAILED ? "000001" : "");
}

void on_client_socket_connect(Client *client) {
    client->net_session_connected = 1;
    net_set_status(client, NET_STATUS_CONNECTED, NULL);

    printf("connected to server %s:%u\n", client->net_address, (unsigned int) client->net_port);
}

void on_client_socket_disconnect(Client *client) {
    // A connect attempt that never got a session is reported by on_client_socket_failed().
    if (!client->net_session_connected) return;

    client->net_session_connected = 0;

    char reason[sizeof(client->net_disconnect_reason)];
    char code[sizeof(client->net_disconnect_code)];
    pthread_mutex_lock(&client->net_lock);
    snprintf(reason, sizeof(reason), "%s", client->net_disconnect_reason);
    snprintf(code, sizeof(code), "%s", client->net_disconnect_code);
    client->net_disconnect_reason[0] = 0;
    client->net_disconnect_code[0] = 0;
    pthread_mutex_unlock(&client->net_lock);

    // Krunker gW(): a server error (idle kick, ...) closes for good and stays on the
    // error screen; only the page's reconnect link (or going offline) tries again.
    if (reason[0]) client->net_hold_retry = 1;
    net_set_status_code(client, NET_STATUS_FAILED, reason[0] ? reason : "Disconnected from server", // server ERROR (e.g. idle kick) wins
                        reason[0] ? (code[0] ? code : "000000") : "000000");

    printf("disconnected from server\n");
}

void on_client_socket_failed(Client *client, const char *message) {
    static char last_logged[160];

    client->net_session_connected = 0;
    net_set_status(client, NET_STATUS_FAILED, message);

    // The transport retries every couple of seconds: log each distinct message once.
    if (strncmp(last_logged, message, sizeof(last_logged) - 1) != 0) {
        strncpy(last_logged, message, sizeof(last_logged) - 1);
        last_logged[sizeof(last_logged) - 1] = 0;
        fprintf(stderr, "%s\n", message);
    }
}

void on_client_socket_error(Client *client, const int error) {
    char message[160];
    char reason[96];

#ifdef WIN32
    snprintf(reason, sizeof(reason), "error %d", error);
#else
    snprintf(reason, sizeof(reason), "%s", strerror(error));
#endif

    snprintf(message, sizeof(message), "Could not connect to %s:%u (%s)", client->net_address, (unsigned int) client->net_port, reason);
    on_client_socket_failed(client, message);
}

void on_client_socket_event(Client *client) {
    (void) client;
}

void client_net_disconnect(Client *client, const char *reason, const char *code) {
    if (client->offline) return;

    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    if (socket >= 0) {
        snprintf(client->net_disconnect_reason, sizeof(client->net_disconnect_reason), "%s", reason && reason[0] ? reason : "Disconnected");
        snprintf(client->net_disconnect_code, sizeof(client->net_disconnect_code), "%s", code && code[0] ? code : "000000");
    }
    pthread_mutex_unlock(&client->net_lock);
    if (socket < 0) return;

    client->net_hold_retry = 1;
#ifdef __EMSCRIPTEN__
    net_web_disconnect(client); // net_web_on_close -> on_client_socket_disconnect()
#else
    net_socket_shutdown(socket); // the net thread's recv() returns 0 -> on_client_socket_disconnect()
#endif
}

void client_net_request_reconnect(Client *client) {
    client->net_hold_retry = 0;
#ifdef __EMSCRIPTEN__
    net_web_request_reconnect(client);
#else
    client->net_retry_now = 1; // net_main() polls this inside its retry sleep
#endif
}
