// Client side of the map-logic WP-B world packets (shared/src/world.c, zone.c, build.c build them).
// The authority's state is mirrored into the client's own Game (zone circle, builds), so prediction
// and rendering read the same data the server simulates.
#include <world_events.h>
#include <world.h>
#include <stdio.h>
#include <string.h>

static struct {
    int has;
    NetLogTimePacket last;
} g_log_time;

static ClientWorldAnimFn g_anim_handler;

void client_world_set_anim_handler(const ClientWorldAnimFn fn) { g_anim_handler = fn; }

void client_world_reset(void) { memset(&g_log_time, 0, sizeof(g_log_time)); }

int client_world_log_time(int *time_ms, int *time_log, int *kind, int *show_ms) {
    if (!g_log_time.has) return 0;
    if (time_ms) *time_ms = g_log_time.last.time_ms;
    if (time_log) *time_log = g_log_time.last.time_log;
    if (kind) *kind = g_log_time.last.kind;
    if (show_ms) *show_ms = g_log_time.last.show_ms;
    return 1;
}

#define WORLD_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

int client_world_handle_event(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_ZONE: { // "zn": Game.updateZone -> zone.update(0, value)
            WORLD_READ(NetZonePacket, packet);
            if (client->game.ready && !game_is_authority(&client->game)) zone_apply_packet(&client->game, &packet);
            return 1;
        }
        case NET_PACKET_LOG_TIME: { // "gt" time, zoneIndex / the speed-run "t"
            WORLD_READ(NetLogTimePacket, packet);
            g_log_time.has = 1;
            g_log_time.last = packet;
            return 1;
        }
        case NET_PACKET_OBJECT_ANIM: { // "ana" uid, clip, raw, isObj
            WORLD_READ(NetObjectAnimPacket, packet);
            packet.clip[sizeof(packet.clip) - 1] = 0;
            Map *map = client->game.ready ? client->game.map : NULL;
            if (!packet.is_object || !map || packet.uid < 0 || (size_t) packet.uid >= map->object_count) return 1;
            if (g_anim_handler) g_anim_handler(client, map->objects[packet.uid], packet.clip, packet.raw);
            return 1;
        }
        case NET_PACKET_BUILD: { // "bo": Game.buildObj (shared/src/build.c)
            WORLD_READ(NetBuildPacket, packet);
            if (client->game.ready && !game_is_authority(&client->game)) world_apply_build_packet(&client->game, &packet);
            return 1;
        }
        default:
            return 0;
    }
}
