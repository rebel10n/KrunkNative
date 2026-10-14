// NET_PACKET_JOIN (shared/include/shared.h): sent by both transports right after HELLO.
// client->net_join carries what the brain handed out (game id + join ticket from
// /mm/seek-game, servers_ui.c) and the social session token (account_ui.c); the name is
// the guest nickname wish. All empty (offline, direct connects as a guest) is a plain
// guest join, which every KrunkNative server accepts.
#include <client.h>
#include <backend_ui.h> // servers_ui_brain_mode
#include <stdio.h>
#include <string.h>

void client_net_send_join(Client *client, const int socket) {
    NetJoinPacket join;
    pthread_mutex_lock(&client->net_lock);
    join = client->net_join;
    pthread_mutex_unlock(&client->net_lock);
    // The session token never goes to a server the brain did not hand out (finder list,
    // direct connect): online_mode servers verify accounts through the auth service's
    // join / hasJoined handshake instead (online_mode.c); every server takes guests.
    if (!servers_ui_brain_mode()) memset(join.session, 0, sizeof(join.session));
    // the menu nickname, unless it is the default or the account's (accounts ignore it)
    if (!join.session[0] && strcmp(client->menu_name, "Guest") != 0) snprintf(join.name, sizeof(join.name), "%s", client->menu_name);
    join.game_id[sizeof(join.game_id) - 1] = 0;
    join.client_key[sizeof(join.client_key) - 1] = 0;
    join.session[sizeof(join.session) - 1] = 0;
    join.name[sizeof(join.name) - 1] = 0;
    // Without a password the JOIN stays NET_JOIN_PACKET_V2_SIZE: servers built before the
    // password field drop any other length, and their deferred ENT then waits out the whole
    // join_timeout (5 s) before spawning us as a guest.
    const size_t length = join.password[0] ? sizeof(join) : NET_JOIN_PACKET_V2_SIZE;
    client_net_send_packet(socket, NET_PACKET_JOIN, &join, length);
}
