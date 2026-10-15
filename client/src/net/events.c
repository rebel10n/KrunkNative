// Transport -> client connection events. The native TCP transport (net.c) calls
// these from its background thread, the web transport (net_web.c) from browser
// callbacks, so they must not touch the HTML UI or game state: they record a
// NetStatus under net_lock and client_menu_tick() (main thread) turns it into
// net_connected updates and the page's connection state (menu_ui.c).
#include <client.h>
#include <krunker_net.h>
#include <ui_logic.h> // hud_votekick_on_session_lost (a K.P.D call ends with the session)
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#include <errno.h>
#endif

static void net_set_status_code(Client *client, const NetStatus status, const char *error, const char *code) {
    pthread_mutex_lock(&client->net_lock);
    client->net_status = status;
    client->net_status_gave_up = 0;
    snprintf(client->net_status_code, sizeof(client->net_status_code), "%s", code ? code : "");
    strncpy(client->net_status_error, error ? error : "", sizeof(client->net_status_error) - 1);
    client->net_status_error[sizeof(client->net_status_error) - 1] = 0;
    pthread_mutex_unlock(&client->net_lock);
}

static void net_set_status(Client *client, const NetStatus status, const char *error) {
    net_set_status_code(client, status, error, status == NET_STATUS_FAILED ? "000001" : "");
    pthread_mutex_lock(&client->net_lock);
    client->net_status_dropped = 0;
    client->net_status_close_code = 0;
    client->net_status_close_reason[0] = 0;
    pthread_mutex_unlock(&client->net_lock);
}

void client_net_set_drop(Client *client, const char *tag, const int close_code, const char *fmt, ...) {
    pthread_mutex_lock(&client->net_lock);
    if (!client->net_drop_reason[0]) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(client->net_drop_reason, sizeof(client->net_drop_reason), fmt, args);
        va_end(args);
        snprintf(client->net_drop_tag, sizeof(client->net_drop_tag), "%s", tag ? tag : "");
        client->net_drop_close_code = close_code;
    }
    pthread_mutex_unlock(&client->net_lock);
}

void on_client_socket_connect(Client *client) {
    client->net_session_connected = 1;
    pthread_mutex_lock(&client->net_lock);
    client->net_drop_reason[0] = 0; // a new session: no drop explained yet
    client->net_drop_tag[0] = 0;
    client->net_drop_close_code = 0;
    client->net_close_intended = 0;
    pthread_mutex_unlock(&client->net_lock);
    net_set_status(client, NET_STATUS_CONNECTED, NULL);

    printf("connected to server %s:%u\n", client->net_address, (unsigned int) client->net_port);
}

void on_client_socket_disconnect(Client *client) {
    // A connect attempt that never got a session is reported by on_client_socket_failed().
    if (!client->net_session_connected) return;

    client->net_session_connected = 0;
    hud_votekick_on_session_lost(); // a K.P.D call in flight: its answer can no longer arrive (the phone down)

    char reason[sizeof(client->net_disconnect_reason)];
    char code[sizeof(client->net_disconnect_code)];
    char drop[sizeof(client->net_drop_reason)], tag[sizeof(client->net_drop_tag)];
    pthread_mutex_lock(&client->net_lock);
    snprintf(reason, sizeof(reason), "%s", client->net_disconnect_reason);
    snprintf(code, sizeof(code), "%s", client->net_disconnect_code);
    snprintf(drop, sizeof(drop), "%s", client->net_drop_reason);
    snprintf(tag, sizeof(tag), "%s", client->net_drop_tag);
    const int close_code = client->net_drop_close_code;
    const int intended = client->net_close_intended;
    client->net_close_intended = 0;
    client->net_disconnect_reason[0] = 0;
    client->net_disconnect_code[0] = 0;
    client->net_drop_reason[0] = 0;
    client->net_drop_tag[0] = 0;
    client->net_drop_close_code = 0;
    pthread_mutex_unlock(&client->net_lock);

    if (reason[0]) {
        // Krunker gW(): a server error (idle kick, ...) closes for good and stays on the
        // error screen; only the page's reconnect link (or going offline) tries again.
        client->net_hold_retry = 1;
        net_set_status_code(client, NET_STATUS_FAILED, reason, code[0] ? code : "000000"); // server ERROR (e.g. idle kick) wins
        printf("disconnected from server: %s (%s)\n", reason, code[0] ? code : "000000");
        return;
    }

    if (intended) { // a server switch: the next session follows at once, the page shows its loading spinner
        net_set_status_code(client, NET_STATUS_CONNECTING, "", "");
        printf("switching servers (the session closed on purpose)\n");
        return;
    }

    // The transport lost the session without a word from the server: say what it saw (v7's
    // io_disconnect(null, "000001", closeEvent): "Code: <close code> | Reason: <close reason>").
    // A matchmade session (a join ticket) is rejoined with a fresh ticket by servers_ui.c
    // (servers_ui_connection_lost): the transport holds so the old ticket is not resent meanwhile.
    char message[sizeof(client->net_status_error)];
    snprintf(message, sizeof(message), "Disconnected: %s", drop[0] ? drop : "the connection was closed");
    pthread_mutex_lock(&client->net_lock);
    const int ticket = client->net_join.client_key[0] != 0;
    pthread_mutex_unlock(&client->net_lock);
    if (ticket && !client->offline && !client->net_should_quit) client->net_hold_retry = 1;
    net_set_status_code(client, NET_STATUS_FAILED, message, "000001");
    pthread_mutex_lock(&client->net_lock);
    client->net_status_dropped = 1;
    client->net_status_close_code = close_code;
    snprintf(client->net_status_close_reason, sizeof(client->net_status_close_reason), "%s", tag[0] ? tag : "closed");
    pthread_mutex_unlock(&client->net_lock);

    printf("disconnected from server: %s\n", drop[0] ? drop : "connection closed");
}

void on_client_socket_failed(Client *client, const char *message) {
    static char last_logged[160];

    client->net_session_connected = 0;
    hud_votekick_on_session_lost(); // a call made on the lost session gets no answer (the phone down)
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
    if (krunker_net_active()) { // an official krunker.io game (idle kick, going offline)
        (void) code;
        krunker_net_leave(client, reason && reason[0] ? reason : "Disconnected");
        client->net_hold_retry = 1;
        return;
    }

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
    if (krunker_net_active()) { // official krunker.io: seek the same game again
        krunker_net_reconnect(client);
        return;
    }
#ifdef __EMSCRIPTEN__
    net_web_request_reconnect(client);
#else
    client->net_retry_now = 1; // net_main() polls this inside its retry sleep
#endif
}

// The retry budget is spent (NET_RETRY_BUDGET failed attempts in a row): the transport holds
// (net_hold_retry, like a server ERROR: only the page's Retry starts it again), so the page shows
// the disconnected screen with its Retry button. Any other failure is reported with the transport
// still retrying, which the page shows as the loading spinner.
void client_net_give_up(Client *client, const int attempts) {
    client->net_hold_retry = 1;
    char last[sizeof(client->net_status_error)], message[sizeof(client->net_status_error)];
    pthread_mutex_lock(&client->net_lock);
    snprintf(last, sizeof(last), "%s", client->net_status_error);
    pthread_mutex_unlock(&client->net_lock);
    // the last attempt's own text, unless it already says "Disconnected" (a lost session's reason)
    if (last[0] && strncmp(last, "Disconnected", 12) != 0) snprintf(message, sizeof(message), "Disconnected: gave up after %d attempts (%s)", attempts, last);
    else if (last[0]) snprintf(message, sizeof(message), "%s; gave up after %d attempts", last, attempts);
    else snprintf(message, sizeof(message), "Disconnected: gave up after %d attempts", attempts);
    net_set_status_code(client, NET_STATUS_FAILED, message, "000001");
    pthread_mutex_lock(&client->net_lock);
    client->net_status_dropped = 0; // not a lost session for servers_ui.c: the give-up is final
    client->net_status_close_code = 0;
    client->net_status_close_reason[0] = 0;
    client->net_status_gave_up = 1;
    pthread_mutex_unlock(&client->net_lock);
    printf("[net] gave up after %d attempts: %s\n", attempts, last[0] ? last : "no answer");
}
