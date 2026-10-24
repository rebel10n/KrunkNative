// Krunker v10.0.3 player poses and freezeOthers (shared/include/player_pose.h), against deob-10.0.3.js:
//   - KrunkScript player.pose: the server script's setter runs game.sanitizePose (:220978-221034) and
//     game.setPlayerPose broadcasts PLAYER_POSE (:221035-221042); the getter reads it back (:208225-208231);
//     a client script cannot set it; a non-object or an object without pose keys clears it;
//   - the NET_EXT_PLAYER_POSE body (NetPlayerPosePacket, "pz" sid, pose | 0) and endGame's clear (:221901);
//   - the arm targets of hold / holdL (:193375), split arms (:193337), isWeaponModelHidden (:195350);
//   - updateHeight's bodyY on the rendering client only (:193561-193566);
//   - game.applyFreezeOthers (:221547-221573): the script's own flags it writes (an unfreeze turns them all on, a
//     script or a respawn's reset may turn them on meanwhile), and its reset at startGame (:219811).
// The scripts run through ks_game_load on a real Game with the server's API table. Run from the repository
// root: ./<build>/player_pose_test
#include <ks_game.h>
#include <net_ext.h>
#include <player_pose.h>
#include <world.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(const float a, const float b) { return fabsf(a - b) < 1e-5f; }

// ---------------------------------------------------------------- host: script log lines, sent packets

static char g_log[16][256];
static int g_log_count;

static void host_log(void *user, const int is_error, const char *line) {
    (void) user;
    (void) is_error;
    if (g_log_count < 16) snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line ? line : "");
}

typedef struct {
    int type;
    int uid;
    uint8_t payload[128];
    uint16_t length;
} SentPacket;

static SentPacket g_sent[512];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= 512) return;
    SentPacket *p = &g_sent[g_sent_count++];
    p->type = type;
    p->uid = uid;
    p->length = length < sizeof(p->payload) ? length : (uint16_t) sizeof(p->payload);
    if (payload && p->length) memcpy(p->payload, payload, p->length);
}

static int count_sent(const int type, const int uid) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].type == type && g_sent[i].uid == uid;
    return n;
}

// The last NET_EXT_PLAYER_POSE sent (to everyone), decoded; 0 when none
static int last_pose_packet(NetPlayerPosePacket *out) {
    for (int i = g_sent_count - 1; i >= 0; i--) {
        if (g_sent[i].type != NET_PACKET_EXT) continue;
        uint16_t subtype, length;
        const uint8_t *body;
        if (!net_ext_unpack(g_sent[i].payload, g_sent[i].length, &subtype, &body, &length) || subtype != NET_EXT_PLAYER_POSE) continue;
        if (length != sizeof(*out) || g_sent[i].uid != NET_TARGET_ALL) return 0;
        memcpy(out, body, sizeof(*out));
        return 1;
    }
    return 0;
}

static int count_pose_packets(void) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) {
        uint16_t subtype, length;
        const uint8_t *body;
        n += g_sent[i].type == NET_PACKET_EXT && net_ext_unpack(g_sent[i].payload, g_sent[i].length, &subtype, &body, &length) &&
             subtype == NET_EXT_PLAYER_POSE;
    }
    return n;
}

// ---------------------------------------------------------------- game setup (as ks_game_v1003_test.c)

static cJSON *load_map(const char *path) {
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("Failed to load %s (run from the repository root)\n", path);
        exit(1);
    }
    return map;
}

static void game_on(Game *game, const cJSON *map, const int mode_id) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = mode_id;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
}

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = (vec3) {(float) uid * 10.0f, 0.0f, 0.0f};
    player->last_position = player->position;
    return player;
}

static KsApiImpl g_api[KS_API_COUNT];

static KsHost script_host(const KsSide side) {
    memset(g_api, 0, sizeof(g_api));
    ks_game_api_fill(g_api, side);
    KsHost host = {0};
    host.side = side;
    host.log = host_log;
    host.api = g_api;
    return host;
}

// Loads `src` as the `side` script of `game` and runs its start().
static void run_script(Game *game, const KsSide side, const char *src) {
    ks_game_unload(game);
    static KsHost host; // the game keeps a pointer to it: hooks run after this returns
    host = script_host(side);
    const int rc = ks_game_load(game, side, src, strlen(src), &host, NULL);
    CHECK(rc == 0, "the script compiles and loads (rc %d)", rc);
    g_log_count = 0;
    g_sent_count = 0;
    if (rc == 0) ks_game_start(game);
}

// ---------------------------------------------------------------- player.pose (script)

// Every key, out-of-range numbers, a string number, a NaN arm item, a short arm, an unknown key (dropped)
static const char *k_set_script =
    "public action start() {\n"
    "    obj[] all = GAME.PLAYERS.list();\n"
    "    obj p = all[0];\n"
    "    p.pose = {legs: 1.6, weapon: 0, bob: false, faceRide: \"yes\", hold: 0, holdL: true, legX: 9,\n"
    "              bodyY: \"-2.34567\", torsoX: -0.12345, armL: num[-1.2, 0 / 0, 7], armR: num[0.5], color: 3};\n"
    "    GAME.log(p.pose);\n"
    "}\n";

// GAME.log's JSON without its indentation
static void squeeze(const char *in, char *out, const size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++)
        if (*in != ' ' && *in != '\n') out[o++] = *in;
    out[o] = 0;
}

static void test_script_set(Game *game, Player *p) {
    printf("Running player.pose setter test...\n");
    run_script(game, KS_SIDE_SERVER, k_set_script);
    const PlayerPose *pose = &p->pose;
    CHECK(pose->has == PLAYER_POSE_KEYS, "every pose key is kept (has 0x%x)", pose->has);
    CHECK(pose->legs == 2, "legs: round(1.6) = 2 (%d)", pose->legs);
    CHECK(!pose->weapon && !pose->bob && pose->face_ride && !pose->hold && pose->hold_l, "the flags are v ? 1 : 0 (%d %d %d %d %d)",
          pose->weapon, pose->bob, pose->face_ride, pose->hold, pose->hold_l);
    CHECK(near(pose->leg_x, 3.2f), "legX 9 is clamped to 3.2 (%g)", pose->leg_x);
    CHECK(near(pose->body_y, -2.346f), "bodyY \"-2.34567\": parseFloat, rounded to 1/1000 (%g)", pose->body_y);
    CHECK(near(pose->torso_x, -0.123f), "torsoX -0.12345 rounds to -0.123 (%g)", pose->torso_x);
    CHECK(near(pose->arm_l[0], -1.2f) && pose->arm_l[1] == 0.0f && near(pose->arm_l[2], 3.2f), "armL: [-1.2, NaN -> 0, 7 -> 3.2] (%g %g %g)",
          pose->arm_l[0], pose->arm_l[1], pose->arm_l[2]);
    CHECK(near(pose->arm_r[0], 0.5f) && pose->arm_r[1] == 0.0f && pose->arm_r[2] == 0.0f, "armR [0.5]: the missing items are 0");
    char got[256] = "";
    if (g_log_count) squeeze(g_log[0], got, sizeof(got));
    CHECK(!strcmp(got, "{\"legs\":2,\"weapon\":0,\"bob\":0,\"faceRide\":1,\"hold\":0,\"holdL\":1,\"legX\":3.2,\"bodyY\":-2.346,"
                       "\"torsoX\":-0.123,\"armL\":[-1.2,0,3.2],\"armR\":[0.5,0,0]}"),
          "the getter reads the sanitized pose back, in sanitizePose's key order (%s)", got);
    NetPlayerPosePacket packet;
    CHECK(last_pose_packet(&packet), "setPlayerPose broadcasts PLAYER_POSE to everyone");
    CHECK(packet.uid == p->uid && packet.has == PLAYER_POSE_KEYS && packet.legs == 2 && packet.hold_l == 1 && near(packet.body_y, -2.346f) &&
          near(packet.arm_l[2], 3.2f), "the packet carries the sanitized pose (uid %d has 0x%x)", packet.uid, packet.has);
}

static const char *k_clear_script =
    "public action start() {\n"
    "    obj[] all = GAME.PLAYERS.list();\n"
    "    obj p = all[0];\n"
    "    p.pose = {bodyY: -3};\n"
    "    p.pose = {nothing: 1};\n"
    "    GAME.log(toStr p.pose);\n"
    "    p.pose = {legX: \"abc\", armL: 5};\n"
    "    p.pose = {bodyY: -3};\n"
    "    p.pose = 4;\n"
    "    GAME.log(toStr p.pose);\n"
    "}\n";

static void test_script_clear(Game *game, Player *p) {
    printf("Running player.pose clear test...\n");
    run_script(game, KS_SIDE_SERVER, k_clear_script);
    CHECK(!p->pose.has, "a non-object clears the pose");
    CHECK(g_log_count == 2 && !strcmp(g_log[0], "null") && !strcmp(g_log[1], "null"), "an object without pose keys reads back null (%s / %s)",
          g_log_count > 0 ? g_log[0] : "-", g_log_count > 1 ? g_log[1] : "-");
    CHECK(count_pose_packets() == 5, "every assignment broadcasts (%d)", count_pose_packets());
    NetPlayerPosePacket packet;
    CHECK(last_pose_packet(&packet) && packet.uid == p->uid && packet.has == 0, "the clear is PLAYER_POSE sid, 0");
}

static const char *k_hold_script =
    "public action start() {\n"
    "    obj[] all = GAME.PLAYERS.list();\n"
    "    obj p = all[0];\n"
    "    p.pose = {hold: 1, legs: 0, bodyY: -4};\n"
    "}\n";

static void test_script_client_side(Game *game, Player *p) {
    printf("Running player.pose client-side test...\n");
    memset(&p->pose, 0, sizeof(p->pose));
    run_script(game, KS_SIDE_CLIENT, k_hold_script);
    CHECK(!p->pose.has && count_pose_packets() == 0, "a client script cannot set the pose (has 0x%x, %d packets)", p->pose.has, count_pose_packets());
    run_script(game, KS_SIDE_SERVER, k_hold_script);
    CHECK(p->pose.has == (PLAYER_POSE_HOLD | PLAYER_POSE_LEGS | PLAYER_POSE_BODY_Y) && p->pose.hold && p->pose.legs == 0,
          "the server script's hold pose (has 0x%x)", p->pose.has);
}

// ---------------------------------------------------------------- the pose rules the client applies

static void test_rules(void) {
    printf("Running pose rules test...\n");
    PlayerPose pose = {0};
    float t[3];
    pose.has = PLAYER_POSE_HOLD;
    pose.hold = 1;
    player_pose_arm_target(&pose, 0, t);
    CHECK(near(t[0], -0.25f) && t[1] == 0.0f && t[2] == 0.0f, "hold: the left arm to [-0.25, 0, 0]");
    player_pose_arm_target(&pose, 1, t);
    CHECK(near(t[0], -0.25f), "hold: the right arm to [-0.25, 0, 0]");
    CHECK(player_pose_hides_weapon(&pose) && !player_pose_splits_arms(&pose), "hold hides the weapon model and keeps the arms merged");

    memset(&pose, 0, sizeof(pose));
    pose.has = PLAYER_POSE_HOLD_L;
    pose.hold_l = 1;
    player_pose_arm_target(&pose, 0, t);
    CHECK(near(t[0], -1.2f) && t[1] == 0.0f && near(t[2], -0.6f), "holdL: the left arm to [-1.2, 0, -0.6]");
    player_pose_arm_target(&pose, 1, t);
    CHECK(t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f, "holdL: the right arm stays");
    CHECK(!player_pose_hides_weapon(&pose) && player_pose_splits_arms(&pose), "holdL keeps the weapon and splits the arms");

    pose.has |= PLAYER_POSE_ARM_L;
    pose.arm_l[0] = 0.4f;
    player_pose_arm_target(&pose, 0, t);
    CHECK(near(t[0], 0.4f), "a given armL wins over holdL's");

    memset(&pose, 0, sizeof(pose));
    pose.has = PLAYER_POSE_WEAPON;
    pose.weapon = 1;
    CHECK(!player_pose_hides_weapon(&pose), "weapon 1 keeps the weapon model");
    pose.weapon = 0;
    CHECK(player_pose_hides_weapon(&pose), "weapon 0 hides the weapon model");
    pose.has = PLAYER_POSE_ARM_R;
    CHECK(player_pose_splits_arms(&pose), "armR splits the arms");

    float v;
    CHECK(player_pose_clamp(0.0005, 3.2f, &v) && near(v, 0.001f), "Math.round(0.5) = 1: 0.0005 -> 0.001 (%g)", v);
    CHECK(player_pose_clamp(-0.0005, 3.2f, &v) && v == 0.0f, "Math.round(-0.5) = -0: -0.0005 -> 0 (%g)", v);
    CHECK(!player_pose_clamp(NAN, 3.2f, &v) && !player_pose_clamp(INFINITY, 3.2f, &v), "a non-finite value is dropped");
}

static void test_packet(Player *p) {
    printf("Running NET_EXT_PLAYER_POSE packet test...\n");
    CHECK(sizeof(NetPlayerPosePacket) == 48, "the body is 48 bytes");
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(NetPlayerPosePacket)];
    memset(&p->pose, 0, sizeof(p->pose));
    p->pose.has = PLAYER_POSE_ARM_R | PLAYER_POSE_TORSO_X;
    p->pose.arm_r[1] = -0.75f;
    p->pose.torso_x = 0.5f;
    const uint16_t n = player_pose_pack(p, buf, sizeof(buf));
    CHECK(n == sizeof(buf), "the envelope is header + body (%u)", (unsigned) n);
    uint16_t subtype, length;
    const uint8_t *body;
    CHECK(net_ext_unpack(buf, n, &subtype, &body, &length) && subtype == NET_EXT_PLAYER_POSE && length == sizeof(NetPlayerPosePacket),
          "it unpacks as NET_EXT_PLAYER_POSE 0x0200");
    NetPlayerPosePacket packet;
    memcpy(&packet, body, sizeof(packet));
    CHECK(packet.uid == p->uid && packet.has == (PLAYER_POSE_ARM_R | PLAYER_POSE_TORSO_X) && near(packet.arm_r[1], -0.75f) && near(packet.torso_x, 0.5f),
          "its fields");
    memset(&p->pose, 0, sizeof(p->pose));
    CHECK(player_pose_pack(p, buf, sizeof(buf)) == sizeof(buf) && !memcmp(buf + NET_EXT_HEADER_SIZE + 4, "\0\0", 2), "no pose: has 0 (pose 0)");
    CHECK(player_pose_pack(p, buf, NET_EXT_HEADER_SIZE + 4) == 0, "a short buffer packs nothing");
}

static void test_end_game(Game *game, Player *a, Player *b) {
    printf("Running endGame clear test...\n");
    PlayerPose pose = {.has = PLAYER_POSE_LEGS, .legs = 2};
    CHECK(game_set_player_pose(game, a, &pose), "the authority sets a pose");
    memset(&b->pose, 0, sizeof(b->pose));
    g_sent_count = 0;
    game_end_round(game);
    CHECK(!a->pose.has, "endGame clears the pose");
    CHECK(count_pose_packets() == 1, "one PLAYER_POSE 0, only for the posed player (%d)", count_pose_packets());
    NetPlayerPosePacket packet;
    CHECK(last_pose_packet(&packet) && packet.uid == a->uid && packet.has == 0, "pose 0 for it");

    // a non-authority (a client's mirror) does not set poses
    game->is_local = 0;
    CHECK(!game_set_player_pose(game, a, &pose) && !a->pose.has, "a client's game_set_player_pose does nothing");
    game->is_local = 1;
}

// ---------------------------------------------------------------- bodyY and the local eye

static void test_body_y(Player *p) {
    printf("Running bodyY height test...\n");
    memset(&p->pose, 0, sizeof(p->pose));
    p->crouch_val = 0.0f;
    player_update_height(p);
    const float standing = p->height;
    p->pose.has = PLAYER_POSE_BODY_Y;
    p->pose.body_y = -4.0f;
    p->render_you = 0;
    player_update_height(p);
    CHECK(near(p->height, standing), "an authority's player keeps the plain height (%g)", p->height);
    p->render_you = 1;
    player_update_height(p);
    CHECK(near(p->height, standing - 4.0f), "the rendering client's own eye drops with bodyY -4 (%g -> %g)", standing, p->height);
    p->crouch_val = 1.0f;
    player_update_height(p);
    CHECK(near(p->height, standing - game_constants.crouch_distance - 4.0f), "with the crouch (%g)", p->height);
    p->render_you = 0;
    p->crouch_val = 0.0f;
    memset(&p->pose, 0, sizeof(p->pose));
    player_update_height(p);
}

// ---------------------------------------------------------------- applyFreezeOthers

// The script's own flags: onChatMessage(msg, id) sets them on that player and logs what it reads back
static const char *k_flags_script =
    "public action onChatMessage(str msg, str playerID) {\n"
    "    obj p = GAME.PLAYERS.findByID(playerID);\n"
    "    if (msg == \"stop\") { p.defaultMovement = false; }\n"
    "    if (msg == \"go\") { p.defaultMovement = true; }\n"
    "    GAME.log(toStr p.defaultMovement, toStr p.defaultVelocity, toStr p.disableShooting, toStr p.disableMelee);\n"
    "}\n";

static void test_freeze_others(Game *game, Player *a, Player *b, Player *c) {
    printf("Running applyFreezeOthers test...\n");
    a->frozen = b->frozen = c->frozen = 0;
    run_script(game, KS_SIDE_SERVER, k_flags_script);
    b->velocity = (vec3) {3.0f, 1.0f, 2.0f};
    g_sent_count = 0;
    const int frozen = game_apply_freeze_others(game, 1, a->uid);
    CHECK(frozen == 2 && game->freeze_others, "two players frozen, freezeOthers set (%d)", frozen);
    CHECK(!a->frozen && b->frozen && c->frozen, "everyone but the caller (%d %d %d)", a->frozen, b->frozen, c->frozen);
    CHECK(!ks_player_allows(b, KS_PLAYER_DEFAULT_MOVEMENT) && !ks_player_allows(b, KS_PLAYER_DEFAULT_VELOCITY) && !ks_player_allows(b, KS_PLAYER_CAN_SHOOT) &&
          !ks_player_allows(b, KS_PLAYER_CAN_MELEE) && ks_player_allows(a, KS_PLAYER_CAN_SHOOT), "a frozen player has no default movement nor shooting");
    CHECK(b->velocity.x == 0.0f && b->velocity.y == 0.0f && b->velocity.z == 0.0f, "changePosition stops it where it stands");
    CHECK(count_sent(NET_PACKET_CHEAT, b->uid) == 1 && count_sent(NET_PACKET_CHEAT, c->uid) == 1 && count_sent(NET_PACKET_CHEAT, a->uid) == 0,
          "each frozen player gets \"frz\"");
    CHECK(count_sent(NET_PACKET_POSITION, b->uid) == 1 && count_sent(NET_PACKET_CHANGE_POSITION, NET_TARGET_ALL) == 2,
          "recon to the player and \"chgp\" to everyone (%d, %d)", count_sent(NET_PACKET_POSITION, b->uid),
          count_sent(NET_PACKET_CHANGE_POSITION, NET_TARGET_ALL));

    // v10 writes the script's own flags (:221561-221564): the script reads them back
    g_log_count = 0;
    ks_game_on_chat_message(game, b, "look");
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "false false true true"), "the script reads the freeze in its flags (%s)", g_log_count ? g_log[0] : "-");
    // and may turn them on again: defaultMovement = true moves a frozen player in v10
    ks_game_on_chat_message(game, b, "go");
    CHECK(ks_player_allows(b, KS_PLAYER_DEFAULT_MOVEMENT) && !ks_player_allows(b, KS_PLAYER_CAN_SHOOT), "a script's defaultMovement = true wins over the freeze");
    // a respawn's reset turns the movement on (player.reset :190737-190739); KN's script state reset also turns
    // shooting / melee on (ks_ref_reset_player; doc/v10/player-pose.md)
    ks_game_on_chat_message(game, b, "stop");
    ks_game_on_player_spawn(game, b);
    CHECK(ks_player_allows(b, KS_PLAYER_DEFAULT_MOVEMENT) && ks_player_allows(b, KS_PLAYER_DEFAULT_VELOCITY), "a respawn resets the movement flags");
    player_apply_freeze(b, 1); // frozen again for what follows

    // the unfrozen get every flag back, whatever the script had set (v10 defaultMovement = !freeze)
    ks_game_on_chat_message(game, c, "stop");
    CHECK(!ks_player_allows(c, KS_PLAYER_DEFAULT_MOVEMENT), "the script stopped c itself");
    b->frozen = 0; // only frozen players are let go: b stays as it is
    g_sent_count = 0;
    const int thawed = game_apply_freeze_others(game, 0, -1);
    CHECK(thawed == 1 && !c->frozen && !game->freeze_others, "unfreeze lets the frozen one go (%d)", thawed);
    CHECK(ks_player_allows(c, KS_PLAYER_DEFAULT_MOVEMENT) && ks_player_allows(c, KS_PLAYER_DEFAULT_VELOCITY) && ks_player_allows(c, KS_PLAYER_CAN_SHOOT) &&
          ks_player_allows(c, KS_PLAYER_CAN_MELEE), "unfreezing turns all four flags on, the script's own defaultMovement = false too");
    CHECK(!ks_player_allows(b, KS_PLAYER_CAN_SHOOT), "a player whose freeze marker is gone is not touched");
    CHECK(count_sent(NET_PACKET_CHEAT, c->uid) == 1 && count_sent(NET_PACKET_CHANGE_POSITION, NET_TARGET_ALL) == 0, "\"frz\" false, no move");

    // a client's "frz" writes the same (player_apply_freeze: game_events.c)
    player_apply_freeze(b, 1);
    CHECK(b->frozen && !ks_player_allows(b, KS_PLAYER_DEFAULT_MOVEMENT) && !ks_player_allows(b, KS_PLAYER_CAN_MELEE), "\"frz\" true");
    player_apply_freeze(b, 0);
    CHECK(!b->frozen && ks_player_allows(b, KS_PLAYER_DEFAULT_MOVEMENT) && ks_player_allows(b, KS_PLAYER_CAN_SHOOT), "\"frz\" false");

    game_apply_freeze_others(game, 1, -1);
    CHECK(a->frozen && b->frozen && c->frozen, "no exception: everyone frozen");
    game_start_round(game);
    CHECK(!game->freeze_others, "startGame: freezeOthers = false");
    game_apply_freeze_others(game, 0, -1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *map = load_map("shared/tests/maps/ramp_fix.json");
    static Game game;
    game_on(&game, map, MODE_ID_FFA);
    CHECK(game.ready && game.mode, "the test game is ready with a mode");
    Player *a = add_player(&game, 2), *b = add_player(&game, 3), *c = add_player(&game, 4);

    test_script_set(&game, a);
    test_script_clear(&game, a);
    test_script_client_side(&game, a);
    test_rules();
    test_packet(a);
    test_body_y(a);
    test_freeze_others(&game, a, b, c);
    test_end_game(&game, a, b);

    ks_game_unload(&game);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
