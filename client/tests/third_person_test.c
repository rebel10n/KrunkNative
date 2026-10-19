// The KrunkNative third-person camera (client/include/third_person.h, doc/v7/decisions.md
// "Third person"):
//   - world_solid_fraction (shared/src/world_lookup.c): boxes, map triangles ("tris"), terrain;
//     on maps without triangles it is exactly the death chase cam's old box + terrain clamp
//   - the camera spot is pulled in front of a wall behind the player and never ends in one
//     (synthetic walls, every spawn of Kanji and Sandstorm, backed against walls in 8 directions)
//   - v10's camera rig (pchObjc under the yaw object): pitching swings the camera over / under
//     the eye in the yaw's vertical plane, 5 to the right whatever the pitch, never sideways
//   - the server's permission (chat_commands.c `thirdperson`): refused on a public server with a
//     notify and an "off", granted offline, to devs, to a custom game's host and with knTPCam
// GL-free: third_person_cam.c is included. Run from the repository root.
#include "../src/third_person_cam.c"
#include <bots.h>
#include <chat_commands.h>
#include <game_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static char g_notify[256], g_reply[256];
static int g_cheat_uid = -1, g_cheat_flag = -1, g_cheat_on = -1;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) length;
    if (type == NET_PACKET_NOTIFY) snprintf(g_notify, sizeof(g_notify), "%s", ((const NetNotifyPacket *) payload)->text);
    if (type == NET_PACKET_CHAT) snprintf(g_reply, sizeof(g_reply), "%s", ((const NetChatPacket *) payload)->text);
    if (type == NET_PACKET_CHEAT) {
        const NetCheatPacket *c = payload;
        g_cheat_uid = uid;
        g_cheat_flag = c->flag;
        g_cheat_on = c->on;
    }
}

static int game_on(Game *game, const cJSON *map) {
    static const cJSON *maps[1];
    static int modes[1];
    maps[0] = map;
    modes[0] = 0;
    memset(game, 0, sizeof(*game));
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
    return game->ready;
}

static float len3(const vec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
static vec3 sub3(const vec3 a, const vec3 b) { return (vec3) {a.x - b.x, a.y - b.y, a.z - b.z}; }
static vec3 at3(const vec3 o, const vec3 d, const float t) { return (vec3) {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t}; }

// The solid object box (player collision's set, box collision) containing p, NULL none.
static const Object *solid_at(const Map *map, const vec3 p) {
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!bot_object_blocks_player(o) || o->complex_coll) continue;
        const vec3 s = object_box_size(o);
        if (p.x > o->position.x - s.x * 0.5f && p.x < o->position.x + s.x * 0.5f && p.y > o->position.y &&
            p.y < o->position.y + s.y && p.z > o->position.z - s.z * 0.5f && p.z < o->position.z + s.z * 0.5f) return o;
    }
    return NULL;
}

// The death chase cam's clamp before world_solid_fraction (menu_view.c, b404f22): boxes + terrain.
static float old_chase_fraction(const Game *game, const vec3 from, vec3 seg) {
    if (seg.x == 0.0f) seg.x = 1e-6f;
    if (seg.y == 0.0f) seg.y = 1e-6f;
    if (seg.z == 0.0f) seg.z = 1e-6f;
    float t = 1.0f;
    const Map *map = game->map;
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *o = map->objects[i];
        if (!bot_object_blocks_player(o) || o->complex_coll) continue;
        const float hit = line_in_rect(from, seg, o->position, object_box_size(o));
        if (hit > 0.0f && hit < t) t = hit;
    }
    if (map) {
        const float hit = world_terrain_fraction(game, from, seg);
        if (hit > 0.0f && hit < t) t = hit;
    }
    return t;
}

static const vec3 k_offset = {TP_OFFSET_X, TP_OFFSET_Y, TP_OFFSET_Z};

// ---------------------------------------------------------------- the collision pull-in

static void test_synthetic(void) {
    printf("synthetic walls...\n");
    static Game game;
    // a floor, a wall box 10 units behind the eye (+Z is behind at yaw 0), a triangle wall 10
    // units to the left (-X) as a baked "cpx" object would leave it
    cJSON *map = cJSON_Parse("{\"name\":\"tp\",\"spawns\":[[0,0,0]],\"objects\":["
                             "{\"p\":[0,-10,0],\"s\":[400,10,400]},{\"p\":[0,0,15],\"s\":[200,60,10]}],"
                             "\"tris\":[-10,-5,-100, -10,-5,100, -10,80,0]}");
    CHECK(game_on(&game, map), "tp map loaded");
    if (!game.ready) return;
    CHECK(game.map->tri_count == 1, "one map triangle (%zu)", game.map->tri_count);
    const vec3 eye = {0.0f, 9.5f, 0.0f};

    // world_solid_fraction: clear, a box, a triangle, the floor
    CHECK(world_solid_fraction(&game, eye, (vec3) {0, 0, -50}) == 1.0f, "a clear segment: 1");
    float t = world_solid_fraction(&game, eye, (vec3) {0, 0, 20});
    CHECK(fabsf(t - 0.5f) < 1e-3f, "the box 10 units behind: 0.5 of a 20 unit segment (%.4f)", t);
    t = world_solid_fraction(&game, eye, (vec3) {-20, 0, 0});
    CHECK(fabsf(t - 0.5f) < 1e-3f, "the triangle 10 units to the left: 0.5 (%.4f)", t);
    t = world_solid_fraction(&game, eye, (vec3) {0, -19.0f, -1});
    CHECK(fabsf(t - 0.5f) < 1e-3f, "the floor 9.5 below: 0.5 of 19 (%.4f)", t);
    CHECK(old_chase_fraction(&game, eye, (vec3) {-20, 0, 0}) == 1.0f, "the old death-cam clamp ignored triangles");

    // open space: the full v7 offset (5, 2, 14)
    vec3 spot;
    float keep = third_person_camera_spot(&game, eye, 0.0f, (float) M_PI, k_offset, &spot);
    CHECK(fabsf(keep - len3(k_offset)) < 1e-3f, "facing the wall: the camera keeps the full offset (%.2f)", keep);
    CHECK(fabsf(spot.z - (eye.z - TP_OFFSET_Z)) < 1e-3f && fabsf(spot.x - (eye.x - TP_OFFSET_X)) < 1e-3f && fabsf(spot.y - (eye.y + TP_OFFSET_Y)) < 1e-3f,
          "yaw pi: right is -X, back is -Z (%.2f, %.2f, %.2f)", spot.x, spot.y, spot.z);
    keep = third_person_camera_spot(&game, eye, 0.0f, 0.0f, (vec3) {0, 0, 0}, &spot);
    CHECK(keep == 0.0f && spot.x == eye.x && spot.y == eye.y && spot.z == eye.z, "a zero offset: the eye");

    // backed against the box (yaw 0 looks at -Z, the box is behind): pulled in, never in it
    keep = third_person_camera_spot(&game, eye, 0.0f, 0.0f, k_offset, &spot);
    CHECK(keep < len3(k_offset) - 1.0f, "the wall behind pulls the camera in (%.2f)", keep);
    CHECK(spot.z <= 10.0f - TP_WALL_GAP * 0.5f && !solid_at(game.map, spot), "the camera stays in front of the wall (z %.2f)", spot.z);
    CHECK(world_solid_fraction(&game, eye, sub3(spot, eye)) == 1.0f, "nothing solid between the eye and the camera");
    // pitched up and down too
    for (int p = -3; p <= 3; p++) {
        const float pitch = (float) p * 0.4f;
        keep = third_person_camera_spot(&game, eye, pitch, 0.0f, k_offset, &spot);
        CHECK(!solid_at(game.map, spot) && world_solid_fraction(&game, eye, sub3(spot, eye)) == 1.0f,
              "pitch %.1f: the camera (%.2f, %.2f, %.2f) is in open space", pitch, spot.x, spot.y, spot.z);
    }
    // backed against the triangle wall (looking at +X: back is -X)
    keep = third_person_camera_spot(&game, (vec3) {-6.0f, 9.5f, 0.0f}, 0.0f, (float) -M_PI / 2.0f, k_offset, &spot);
    CHECK(keep < 4.0f && spot.x > -10.0f + TP_WALL_GAP * 0.5f, "a map triangle behind pulls the camera in too (keep %.2f, x %.2f)", keep, spot.x);
    // squeezed: the wall right at the back
    keep = third_person_camera_spot(&game, (vec3) {0.0f, 9.5f, 9.6f}, 0.0f, 0.0f, k_offset, &spot);
    CHECK(keep < TP_MODEL_MIN_DIST, "pressed against the wall: inside the head range (%.2f), the own model is not drawn", keep);

    cJSON_Delete(map);
}

// ---------------------------------------------------------------- the pitch never yaws

// v10: RENDER.camera at (thrdPX, thrdPY, thrdPZ) inside CONTROLS.pchObjc (rotation.x = pitch)
// inside CONTROLS.object (rotation.y = yaw), looking along the look: the camera spot is
// eye + Ry(yaw) Rx(pitch) (5, 2, 14), as three.js composes the matrices.
static vec3 v10_rig_spot(const vec3 eye, const float pitch, const float yaw) {
    const float x = TP_OFFSET_X, y0 = TP_OFFSET_Y, z0 = TP_OFFSET_Z;
    const float y = y0 * cosf(pitch) - z0 * sinf(pitch), z = y0 * sinf(pitch) + z0 * cosf(pitch); // Rx(pitch)
    return (vec3) {eye.x + x * cosf(yaw) + z * sinf(yaw), eye.y + y, eye.z - x * sinf(yaw) + z * cosf(yaw)}; // Ry(yaw)
}

static void test_pitch(void) {
    printf("pitch: the camera orbits over / under the eye, never sideways...\n");
    const vec3 eye = {12.0f, 9.5f, -7.0f};
    int worst_rig = 0, worst_side = 0;
    float rig_err = 0.0f, side_err = 0.0f;
    for (int k = 0; k < 16; k++) {
        const float yaw = -(float) M_PI + (float) k * (float) M_PI / 8.0f;
        const vec3 right = {cosf(yaw), 0.0f, -sinf(yaw)};
        for (int p = -10; p <= 10; p++) {
            const float pitch = (float) p * 0.157f; // -1.57 .. 1.57 (the pitch clamp)
            vec3 spot;
            const float keep = third_person_camera_spot(NULL, eye, pitch, yaw, k_offset, &spot);
            const vec3 rig = v10_rig_spot(eye, pitch, yaw);
            const float e = len3(sub3(spot, rig));
            if (e > rig_err) { rig_err = e; worst_rig = k * 100 + p; }
            // the sideways part of the offset is the 5 to the right, at every pitch
            const vec3 d = sub3(spot, eye);
            const float side = fabsf(d.x * right.x + d.z * right.z - TP_OFFSET_X);
            if (side > side_err) { side_err = side; worst_side = k * 100 + p; }
            CHECK(fabsf(keep - len3(k_offset)) < 1e-3f, "no world: the full offset (%.3f)", keep);
        }
    }
    CHECK(rig_err < 1e-3f, "the spot is v10's rig Ry(yaw) Rx(pitch) (5, 2, 14) (worst %.5f at %d)", rig_err, worst_rig);
    CHECK(side_err < 1e-3f, "the camera stays 5 to the right of the eye at every pitch (worst %.5f at %d)", side_err, worst_side);
    // straight down the camera is above the head, straight up below it (the rig swings, never yaws)
    vec3 down, up;
    third_person_camera_spot(NULL, eye, -1.5f, 0.0f, k_offset, &down);
    third_person_camera_spot(NULL, eye, 1.5f, 0.0f, k_offset, &up);
    CHECK(down.y > eye.y + 13.0f && up.y < eye.y - 12.0f, "pitch -1.5: above (%.2f), +1.5: below (%.2f)", down.y - eye.y, up.y - eye.y);
}

// Every spawn of a real map: 16 yaws x 5 pitches in the open, then backed against the first wall
// behind in 8 directions; the camera never ends in a solid box or behind solid geometry.
static void test_map(const char *path) {
    printf("%s: every spawn, open and backed against walls...\n", path);
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("skip %s (gitignored map assets, as on CI)\n", path);
        return;
    }
    static Game game;
    CHECK(game_on(&game, map), "%s loaded", path);
    if (!game.ready) { cJSON_Delete(map); return; }
    const Map *m = game.map;

    // world_solid_fraction == the death cam's old clamp here (no "tris"): death_cam_test unchanged
    int same = 0, compared = 0;
    for (size_t s = 0; s < m->spawn_count; s++) {
        const vec3 feet = m->spawns[s]->position;
        const vec3 eye = {feet.x, feet.y + game_constants.player_height - game_constants.camera_height, feet.z};
        for (int k = 0; k < 16; k++) {
            const float a = (float) k * (float) M_PI / 8.0f;
            const vec3 seg = {60.0f * sinf(a), (float) (k % 5 - 2) * 8.0f, 60.0f * cosf(a)};
            compared++;
            same += m->tri_count || world_solid_fraction(&game, eye, seg) == old_chase_fraction(&game, eye, seg);
        }
    }
    CHECK(same == compared, "%s: world_solid_fraction == the old death-cam clamp on %d of %d segments", path, same, compared);

    int cases = 0, bad = 0, walls = 0, pulled = 0, printed = 0;
    for (size_t s = 0; s < m->spawn_count; s++) {
        const vec3 feet = m->spawns[s]->position;
        const vec3 eye0 = {feet.x, feet.y + game_constants.player_height - game_constants.camera_height, feet.z};
        if (solid_at(m, eye0)) continue;
        for (int k = 0; k < 16; k++) {
            const float yaw = (float) k * (float) M_PI / 8.0f;
            for (int p = -2; p <= 2; p++) {
                const float pitch = (float) p * 0.5f;
                vec3 spot;
                third_person_camera_spot(&game, eye0, pitch, yaw, k_offset, &spot);
                cases++;
                if (solid_at(m, spot) || world_solid_fraction(&game, eye0, sub3(spot, eye0)) < 1.0f) {
                    if (bad++ < 5) printf("  spawn %zu yaw %.2f pitch %.1f: camera in geometry\n", s, yaw, pitch);
                }
            }
        }
        // backed against a wall: walk backwards (horizontally) until the body touches something
        for (int k = 0; k < 8; k++) {
            const float yaw = (float) k * (float) M_PI / 4.0f;
            const vec3 back = {sinf(yaw), 0.0f, cosf(yaw)}; // -forward at pitch 0
            const float body = game_constants.player_scale + 0.5f;
            vec3 eye = eye0;
            int found = 0;
            for (int step = 0; step < 200; step++) {
                if (world_solid_fraction(&game, eye, at3((vec3) {0, 0, 0}, back, body)) < 1.0f) { found = 1; break; }
                eye = at3(eye, back, 0.5f);
            }
            if (!found) continue;
            walls++;
            for (int p = -2; p <= 2; p++) {
                const float pitch = (float) p * 0.4f;
                vec3 spot;
                const float keep = third_person_camera_spot(&game, eye, pitch, yaw, k_offset, &spot);
                cases++;
                pulled += keep < len3(k_offset) - 0.5f;
                if (solid_at(m, spot) || world_solid_fraction(&game, eye, sub3(spot, eye)) < 1.0f) {
                    if (bad++ < 5) printf("  spawn %zu backed yaw %.2f pitch %.1f: camera in geometry\n", s, yaw, pitch);
                } else if (p == 0 && keep > 1.0f && printed++ < 2) {
                    // poses for the headless e2e (client/tests/third_person_e2e.py): feet, yaw, pitch
                    printf("  backed pose: %.2f %.2f %.2f %.4f 0 (keep %.2f)\n", eye.x,
                           eye.y - (game_constants.player_height - game_constants.camera_height), eye.z, yaw, keep);
                }
            }
        }
    }
    CHECK(bad == 0, "%s: %d of %d camera spots ended in geometry", path, bad, cases);
    CHECK(walls >= (int) m->spawn_count, "%s: backed against %d walls (spawns %zu)", path, walls, m->spawn_count);
    CHECK(pulled > walls, "%s: the walls pulled the camera in %d times", path, pulled);
    printf("  %d spots, %d walls, %d pulled in\n", cases, walls, pulled);
    cJSON_Delete(map);
}

// ---------------------------------------------------------------- the server's permission

static Game *g_game;
#define PEERS 3
static ChatPeer g_peers[PEERS]; // uid 1 = dev, 2 = guest, 3 = guest (the custom host when premium_host)

static void host_log(void *user, const char *text) {
    (void) user;
    snprintf(g_reply, sizeof(g_reply), "%s", text);
}

static int peer_count(void *user) {
    (void) user;
    return PEERS;
}

static int peer_at(void *user, const int index, ChatPeer *out) {
    (void) user;
    if (index < 0 || index >= PEERS) return 0;
    *out = g_peers[index];
    out->player = game_find_player(g_game, out->id);
    return 1;
}

static void say(ChatCommandHost *host, const int sender, const char *line) {
    g_reply[0] = g_notify[0] = 0;
    g_cheat_uid = g_cheat_flag = g_cheat_on = -1;
    chat_command_handle(host, sender, line);
}

static int granted(const int uid) {
    return g_cheat_uid == uid && g_cheat_flag == NET_CHEAT_THIRD_PERSON && g_cheat_on == 1 && !g_notify[0];
}

static int refused(const int uid, const char *notify) {
    return g_cheat_uid == uid && g_cheat_flag == NET_CHEAT_THIRD_PERSON && g_cheat_on == 0 && !strcmp(g_notify, notify);
}

static void test_permission(void) {
    printf("permission...\n");
    cJSON *map = cJSON_Parse("{\"name\":\"perm\",\"spawns\":[[0,0,0],[50,0,0],[-50,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[400,10,400]}]}");
    static Game game;
    g_game = &game;
    CHECK(game_on(&game, map), "perm map loaded");
    if (!game.ready) return;
    for (int uid = 1; uid <= PEERS; uid++) {
        Player *player = player_init(&game);
        player->uid = uid;
        snprintf(player->name, sizeof(player->name), "p%d", uid);
        game_players_add(&game, player);
        player_spawn(player);
    }
    g_peers[0] = (ChatPeer) {.id = 1, .name = "p1", .address = "127.0.0.1", .dev = 1};
    g_peers[1] = (ChatPeer) {.id = 2, .name = "p2", .address = "203.0.113.2"};
    g_peers[2] = (ChatPeer) {.id = 3, .name = "p3", .address = "203.0.113.3"};
    static ChatCommandHost host;
    host.game = &game;
    host.peer_count = peer_count;
    host.peer_at = peer_at;
    host.log = host_log;

    // a public server (server_bin's defaults: perms on, gameserver.cheats "auth"; perms off alike)
    for (int perms = 1; perms >= 0; perms--) {
        host.perms = perms;
        host.cheats_open = 0;
        say(&host, 2, "/thirdperson on");
        CHECK(refused(2, "Third person is off on this server"), "public (perms %d): refused with a notify and an off (%s / cheat %d %d)",
              perms, g_notify, g_cheat_flag, g_cheat_on);
        CHECK(strstr(g_reply, "Not allowed here") != NULL, "public: the sender's reply says so (%s)", g_reply);
        CHECK(game.players[1]->active, "still playing");
    }
    host.perms = 1;
    const char *reason = NULL;
    CHECK(!chat_third_person_allowed(&host, 2, &reason) && reason && !strcmp(reason, "Third person is off on this server"),
          "chat_third_person_allowed agrees (%s)", reason ? reason : "-");
    say(&host, 1, "/thirdperson on");
    CHECK(granted(1), "public: a DEV peer is granted (%s)", g_notify);
    say(&host, 1, "/thirdperson off");
    CHECK(g_cheat_uid == 1 && g_cheat_flag == NET_CHEAT_THIRD_PERSON && g_cheat_on == 0, "thirdperson off: the grant is taken");
    say(&host, CHAT_CMD_CONSOLE, "/thirdperson on");
    CHECK(g_cheat_uid == -1 && !g_notify[0] && strstr(g_reply, "Enabled"), "the server console is never refused (%s)", g_reply);
    // a server that opened its cheats to everyone (perms off, cheats "all")
    host.perms = 0;
    host.cheats_open = 1;
    say(&host, 2, "/thirdperson on");
    CHECK(granted(2), "perms off + cheats all: granted (%s)", g_notify);
    host.perms = 1;
    host.cheats_open = 0;

    // ranked: refused, even for the host (devs excepted)
    g_peers[1].ranked = 1;
    say(&host, 2, "/thirdperson on");
    CHECK(refused(2, "No third person in ranked"), "ranked: refused (%s)", g_notify);
    g_peers[1].ranked = 0;
    game.config.comp = 1;
    say(&host, 2, "/thirdperson on");
    CHECK(refused(2, "No third person in ranked"), "a comp match: refused (%s)", g_notify);
    say(&host, 1, "/thirdperson on");
    CHECK(granted(1), "a comp match: a DEV still may");
    game.config.comp = 0;

    // a custom game: the host, and everyone once knTPCam is on
    game_config_host(&game, NULL, 0);
    CHECK(game_config_is_custom(&game), "hosted: a custom game");
    game.config.tp_cam = 0;
    say(&host, 2, "/thirdperson on");
    CHECK(refused(2, "The host has third person off"), "custom, knTPCam off: a guest is refused (%s)", g_notify);
    g_peers[2].premium_host = 1;
    say(&host, 3, "/thirdperson on");
    CHECK(granted(3), "custom: the host is granted (%s)", g_notify);
    game.config.tp_cam = 1;
    say(&host, 2, "/thirdperson on");
    CHECK(granted(2), "custom, knTPCam on: a guest is granted (%s)", g_notify);
    // knTPCam is a host option (Host Game > Other): off by default, merged from a request
    GameConfig cfg = g_default_game_config;
    CHECK(cfg.tp_cam == 0, "knTPCam defaults to off");
    cJSON *req = cJSON_Parse("{\"knTPCam\":true}");
    game_config_merge(&cfg, req, 1);
    CHECK(cfg.tp_cam == 1, "knTPCam from a host request (%d)", cfg.tp_cam);
    cJSON_Delete(req);
    game_config_free(&game);
    g_peers[2].premium_host = 0;

    // the offline sandbox: always
    game.is_sandbox = 1;
    say(&host, 2, "/thirdperson on");
    CHECK(granted(2), "the offline sandbox: granted (%s)", g_notify);
    game.is_sandbox = 0;
    cJSON_Delete(map);
}

int main(void) {
    test_synthetic();
    test_pitch();
    test_map("assets/maps/official/10_kanji.json");
    test_map("assets/maps/official/02_sandstorm.json");
    test_map("assets/maps/official/20_krunk_plaza.json"); // baked "cpx" triangles
    test_permission();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
