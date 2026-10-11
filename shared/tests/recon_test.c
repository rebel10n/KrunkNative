// Client prediction + server reconciliation (Krunker v3.1.1 tmpInpts / "q" / "l"), end to end
// without sockets: an authority Game (the server) and a predicting client Game on the same map,
// joined by a simulated link with latency + jitter (ordered, like TCP). The client predicts
// every frame's input, sends them in "q" batches (NetInputBatchPacket through the real packet
// framing) and reconciles with every "l" (NetReconPacket) via net_reconcile.
//
// Measured: the correction each "l" applies (predicted position before the snap + replay vs
// after it). 0 = no rubber-banding. Server-driven changes (push, teleport, knockback, noclip
// flips, a client-only cheat move) must be corrected and then converge.
#include <math.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---------------------------------------------------------------- link

#define LINK_MAX 4096
typedef struct {
    double at;
    NetPacket packet;
} LinkItem;

typedef struct {
    LinkItem items[LINK_MAX];
    int head, count;
    double last_at;
    float delay, jitter;
    unsigned int rng;
} Link;

static float link_rand(Link *link) { // own LCG: pcg32 is the games' RNG
    link->rng = link->rng * 1103515245u + 12345u;
    return (float) ((link->rng >> 8) & 0xffff) / 65535.0f * 2.0f - 1.0f;
}

static void link_send(Link *link, const double now, const int type, const void *payload, const uint16_t length) {
    if (link->count >= LINK_MAX) return;
    double at = now + link->delay + link->jitter * link_rand(link);
    if (at < link->last_at) at = link->last_at; // ordered stream
    link->last_at = at;
    LinkItem *item = &link->items[(link->head + link->count++) % LINK_MAX];
    item->at = at;
    item->packet.type = (uint8_t) type;
    item->packet.length = length;
    memcpy(item->packet.payload, payload, length);
}

static int link_take(Link *link, const double now, NetPacket *out) {
    if (!link->count || link->items[link->head].at > now) return 0;
    *out = link->items[link->head].packet;
    link->head = (link->head + 1) % LINK_MAX;
    link->count--;
    return 1;
}

// ---------------------------------------------------------------- the two ends

typedef struct {
    Game server, client;
    Player *s, *c;      // the same player on both ends
    Link up, down;      // client -> server, server -> client
    double now;         // seconds
    double server_next_tick, next_recon_at;

    // client net state (client/src/net/recon.c in miniature)
    Input pending[NET_RECON_PENDING_MAX + 64];
    size_t pending_count;
    NetInputPacket batch[NET_INPUT_BATCH_MAX];
    int batch_count;
    float batch_time;
    int next_seq;

    // negative controls: snap without replaying (no prediction kept), replays without velObj
    int control_no_replay, control_no_vel_obj;

    // measurements
    unsigned int recons;
    float max_correction, last_correction;
    unsigned int frame_rng;
} Sim;

static const cJSON *load_map(const char *name) {
    const char *roots[] = {"./", "../", "../../"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%sassets/maps/%s", roots[i], name);
        size_t length;
        unsigned char *data;
        if (read_file(path, &data, &length)) continue;
        const cJSON *parsed = cJSON_Parse((char *) data);
        free(data);
        if (parsed) return parsed;
    }
    return NULL;
}

static float dist3(const vec3 a, const vec3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

static int sim_init(Sim *sim, const char *map, const int class_index, const float latency_ms, const float jitter_ms) {
    memset(sim, 0, sizeof(*sim));
    const cJSON *server_map = load_map(map);
    const cJSON *client_map = load_map(map);
    if (!server_map || !client_map) return 0;

    game_configure(&sim->server, NULL, &server_map, 1, NULL, 0, NULL, 0, NULL, 0);
    game_configure(&sim->client, NULL, &client_map, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&sim->server, 0, 0, 0);
    game_init(&sim->client, 0, 0, 0);
    sim->server.server = &sim->server; // the authority (game_is_authority)

    sim->s = player_init(&sim->server);
    sim->c = player_init(&sim->client);
    sim->s->uid = sim->c->uid = 1;
    sim->s->class_index = sim->c->class_index = class_index;
    game_players_add(&sim->server, sim->s);
    game_players_add(&sim->client, sim->c);
    player_spawn(sim->s);
    player_spawn(sim->c);
    sim->c->is_you = 1;

    // SPAWN: the client takes the server's spawn point
    sim->c->position = sim->c->last_position = sim->s->position;
    sim->c->direction = sim->s->direction;

    sim->up.delay = sim->down.delay = latency_ms / 1000.0f;
    sim->up.jitter = sim->down.jitter = jitter_ms / 1000.0f;
    sim->up.rng = 1;
    sim->down.rng = 7;
    sim->frame_rng = 3;
    return 1;
}

static void sim_free(Sim *sim) {
    // Process exit frees the games; only the per-player buffers matter between cases.
    (void) sim;
}

static void client_flush(Sim *sim) {
    if (!sim->batch_count) return;
    uint8_t payload[NET_MAX_PAYLOAD];
    payload[0] = (uint8_t) sim->batch_count;
    memcpy(payload + 1, sim->batch, sim->batch_count * sizeof(NetInputPacket));
    link_send(&sim->up, sim->now, NET_PACKET_INPUT_BATCH, payload, (uint16_t) (1 + sim->batch_count * sizeof(NetInputPacket)));
    sim->batch_count = 0;
}

// One client frame with `input` (seq assigned here): predict, record, batch.
static void client_frame(Sim *sim, Input input, const float delta) {
    input.delta = delta;
    input.seq = sim->next_seq++;
    player_queue_input(sim->c, &input);
    if (sim->pending_count < sizeof(sim->pending) / sizeof(sim->pending[0])) sim->pending[sim->pending_count++] = input;

    if (sim->batch_count >= NET_INPUT_BATCH_MAX) client_flush(sim);
    net_packet_from_input(&sim->batch[sim->batch_count++], &input);
    sim->batch_time += delta;
    if (sim->batch_time >= NET_INPUT_SEND_INTERVAL) {
        sim->batch_time -= NET_INPUT_SEND_INTERVAL;
        client_flush(sim);
    }

    game_tick(&sim->client, (float) sim->now, delta);
    net_recon_take_vel_obj(sim->c, sim->pending, sim->pending_count);
}

static void client_receive(Sim *sim) {
    NetPacket packet;
    while (link_take(&sim->down, sim->now, &packet)) {
        if (packet.type != NET_PACKET_RECON || packet.length != sizeof(NetReconPacket)) continue;
        NetReconPacket recon;
        memcpy(&recon, packet.payload, sizeof(recon));
        const vec3 before = sim->c->position;
        if (sim->control_no_vel_obj) {
            for (size_t i = 0; i < sim->pending_count; i++) sim->pending[i].has_vel_obj = 0;
        }
        if (sim->control_no_replay) sim->pending_count = 0;
        sim->pending_count = net_reconcile(sim->c, &recon, sim->pending, sim->pending_count, 0);
        sim->last_correction = dist3(before, sim->c->position);
        if (sim->last_correction > sim->max_correction) sim->max_correction = sim->last_correction;
        sim->recons++;
    }
}

static void server_tick(Sim *sim) {
    NetPacket packet;
    while (link_take(&sim->up, sim->now, &packet)) {
        NetInputBatchPacket batch;
        if (packet.type != NET_PACKET_INPUT_BATCH || !net_packet_read_input_batch(&batch, &packet)) continue;
        for (int i = 0; i < batch.count; i++) {
            Input input;
            net_packet_to_input(&input, &batch.frames[i]);
            if (sim->s->active) player_accept_input(sim->s, &input);
        }
    }
    game_tick(&sim->server, (float) sim->now, 1.0f / 30.0f);
    if (net_recon_due(sim->s, sim->now, &sim->next_recon_at)) {
        NetReconPacket recon;
        net_packet_from_recon(&recon, sim->s, sim->s->input_seq);
        link_send(&sim->down, sim->now, NET_PACKET_RECON, &recon, sizeof(recon));
    }
}

// Advances real time by one client frame (fps +-30%) with `input`, running the server ticks
// and deliveries that fall inside it.
static void sim_step(Sim *sim, const Input *input, const float fps) {
    sim->frame_rng = sim->frame_rng * 1664525u + 1013904223u;
    const float r = (float) ((sim->frame_rng >> 8) & 0xffff) / 65535.0f;
    const float delta = (1.0f / fps) * (0.7f + 0.6f * r);
    sim->now += delta;
    while (sim->server_next_tick <= sim->now) {
        server_tick(sim);
        sim->server_next_tick += 1.0 / 30.0;
    }
    client_receive(sim);
    if (sim->c->active) client_frame(sim, *input, delta);
}

// Idle frames until everything in flight was processed and acked (then client == server).
static void sim_settle(Sim *sim, const float fps) {
    const Input idle = {.move_dir = -1, .x_dir = sim->c->direction.x, .y_dir = sim->c->direction.y};
    const double until = sim->now + 2.5; // > 1 s "l" period + 2 * (latency + jitter)
    while (sim->now < until) sim_step(sim, &idle, fps);
}

// Walk / strafe / turn / jump / crouch-slide script at time t.
static Input script_input(const Sim *sim, const double t) {
    Input input = {0};
    const int phase = (int) (t / 0.8);
    static const int dirs[] = {2, 2, 1, 3, 0, 4, 2, 6, 5, 7, -1, 2};
    input.move_dir = dirs[phase % (int) (sizeof(dirs) / sizeof(dirs[0]))];
    input.y_dir = (float) fmod(t * 0.9, 2.0 * M_PI);
    input.x_dir = 0.2f * (float) sin(t);
    input.jump = fmod(t, 1.3) < 0.05;
    input.crouch = fmod(t, 3.1) > 2.6;
    (void) sim;
    return input;
}

// ---------------------------------------------------------------- cases

static void case_movement(const char *map, const float latency, const float jitter, const float fps) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, map, 0, latency, jitter)) { CHECK(0, "map %s", map); free(sim); return; }

    const double start = sim->now;
    while (sim->now - start < 12.0 && sim->c->active) {
        const Input input = script_input(sim, sim->now - start);
        sim_step(sim, &input, fps);
    }
    sim_settle(sim, fps);
    const float final = dist3(sim->c->position, sim->s->position);
    printf("  %-14s %3.0f ms +-%2.0f @%3.0f fps: %u recons, max correction %.6f, final %.6f, pending %zu\n", map, latency, jitter, fps,
           sim->recons, sim->max_correction, final, sim->pending_count);
    CHECK(sim->recons >= 10, "recons %u", sim->recons);
    CHECK(sim->max_correction < 1e-3f, "rubber-banding: max correction %f", sim->max_correction);
    CHECK(final < 1e-3f, "client/server diverged by %f", final);
    sim_free(sim);
    free(sim);
}

static void walk(Sim *sim, const double seconds, const int dir, const float fps) {
    const double until = sim->now + seconds;
    while (sim->now < until) {
        const Input input = {.move_dir = dir, .x_dir = sim->c->direction.x, .y_dir = sim->c->direction.y};
        sim_step(sim, &input, fps);
    }
}

// changePosition(recon) push and io_tp teleport on the server mid-walk.
static void case_server_moves(const float latency, const float jitter) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
    walk(sim, 1.5, 2, 60.0f);

    // push: a trigger moves the player (stopMomentum) while inputs are in flight
    const vec3 pushed = {sim->s->position.x + 30.0f, sim->s->position.y + 20.0f, sim->s->position.z};
    game_change_position(&sim->server, sim->s, pushed, 1, 1, 0, 0);
    sim->max_correction = 0.0f;
    const unsigned int before = sim->recons;
    walk(sim, 2.0 * latency / 1000.0 + 0.2, 2, 60.0f);
    const float push_correction = sim->max_correction;
    CHECK(sim->recons > before, "push: no immediate recon");
    CHECK(push_correction > 20.0f, "push: client was not corrected (%f)", push_correction);
    sim->max_correction = 0.0f;
    walk(sim, 2.5, 2, 60.0f);
    CHECK(sim->max_correction < 1e-3f, "push: rubber-banding after the correction (%f)", sim->max_correction);
    sim_settle(sim, 60.0f);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "push: final %f", dist3(sim->c->position, sim->s->position));
    printf("  push     %3.0f ms: correction %.3f, then max %.6f\n", latency, push_correction, sim->max_correction);

    // teleport (io_tp)
    const vec3 to = {sim->s->position.x - 40.0f, sim->s->position.y + 30.0f, sim->s->position.z + 10.0f};
    net_player_teleport(sim->s, to.x, to.y, to.z);
    sim->max_correction = 0.0f;
    walk(sim, 2.0 * latency / 1000.0 + 0.2, 6, 60.0f);
    const float tp_correction = sim->max_correction;
    CHECK(tp_correction > 20.0f, "teleport: client was not corrected (%f)", tp_correction);
    sim->max_correction = 0.0f;
    walk(sim, 2.5, 6, 60.0f);
    CHECK(sim->max_correction < 1e-3f, "teleport: rubber-banding after the correction (%f)", sim->max_correction);
    sim_settle(sim, 60.0f);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "teleport: final %f", dist3(sim->c->position, sim->s->position));
    printf("  teleport %3.0f ms: correction %.3f, then max %.6f\n", latency, tp_correction, sim->max_correction);
    free(sim);
}

// Shotgun physPow knockback (players.js shoot velObj): the replay has to re-apply it.
static float knockback_run(const float latency, const float jitter, const int control) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "sandstorm.json", 4, latency, jitter)) { CHECK(0, "map"); free(sim); return -1.0f; } // Vince: Shotgun
    sim->control_no_vel_obj = control;
    CHECK(sim->c->weapon && sim->c->weapon->physical_power > 0.0f, "class 4 should hold a physPow weapon");
    walk(sim, 0.5, -1, 60.0f);

    int shots = 0;
    const double start = sim->now;
    while (sim->now - start < 6.0) {
        const double t = sim->now - start;
        Input input = {.move_dir = 2, .x_dir = -(float) M_PI / 2.0f + 0.05f, .y_dir = sim->c->direction.y};
        input.shoot = fmod(t, control ? 0.35 : 0.9) < 0.03; // the control fires often enough to overlap every "l"
        const unsigned int ammo = sim->c->ammo[sim->c->loadout_index];
        sim_step(sim, &input, 60.0f);
        if (sim->c->ammo[sim->c->loadout_index] < ammo) shots++;
    }
    sim_settle(sim, 60.0f);
    printf("  knockback %3.0f ms: %d shots, max correction %.6f, final %.6f\n", latency, shots, sim->max_correction,
           dist3(sim->c->position, sim->s->position));
    CHECK(shots >= 3, "shots %d", shots);
    const float max = sim->max_correction;
    if (!control) {
        CHECK(max < 1e-3f, "knockback: rubber-banding %f", max);
        CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "knockback: final %f", dist3(sim->c->position, sim->s->position));
    }
    free(sim);
    return max;
}

static void case_knockback(const float latency, const float jitter) {
    knockback_run(latency, jitter, 0);
    if (latency > 0.0f) { // control: replays without input.velObj lose the knockback
        const float lost = knockback_run(latency, jitter, 1);
        printf("  knockback control (no velObj) %3.0f ms: max correction %.3f\n", latency, lost);
        CHECK(lost > 0.5f, "knockback control should rubber-band (%f): the test does not see velObj", lost);
    }
}

// control: snapping without the replay (what KrunkNative did before, minus ignoring "l") rubber-bands
static void case_control_no_replay(const float latency, const float jitter) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
    sim->control_no_replay = 1;
    const double start = sim->now;
    while (sim->now - start < 6.0 && sim->c->active) {
        const Input input = script_input(sim, sim->now - start);
        sim_step(sim, &input, 60.0f);
    }
    printf("  control (no replay) %3.0f ms: max correction %.3f\n", latency, sim->max_correction);
    CHECK(sim->max_correction > 1.0f, "no-replay control should rubber-band (%f)", sim->max_correction);
    free(sim);
}

// Noclip: the server turns it on (cheat-gate's NET_CHEAT path), the client learns it from the
// immediate "l"; flying is drift-free; turning it off mid-flight corrects once, then converges.
// A client that flips noclip on its own (no permission) is pulled back by the next "l".
static void case_noclip(const float latency, const float jitter) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
    walk(sim, 1.0, -1, 60.0f);

    // server-authorized noclip, flying up and forward
    sim->s->noclip = 1;
    const double until = sim->now + 2.0 * latency / 1000.0 + 0.2;
    while (sim->now < until) {
        const Input input = {.move_dir = 2, .x_dir = 0.6f, .y_dir = 0.0f};
        sim_step(sim, &input, 60.0f);
    }
    CHECK(sim->c->noclip, "noclip: the client never learned the flag");
    sim->max_correction = 0.0f;
    for (double t = sim->now + 3.0; sim->now < t;) {
        const Input input = {.move_dir = 2, .x_dir = 0.6f, .y_dir = 0.0f};
        sim_step(sim, &input, 60.0f);
    }
    const float flying = sim->max_correction;
    CHECK(flying < 1e-3f, "noclip: drift while flying %f", flying);
    CHECK(sim->s->position.y > 50.0f, "noclip: server did not fly (y %f)", sim->s->position.y);

    // off mid-flight: both fall
    sim->s->noclip = 0;
    const float height = sim->s->position.y;
    sim->max_correction = 0.0f;
    walk(sim, 2.0 * latency / 1000.0 + 0.2, 2, 60.0f);
    const float flip = sim->max_correction;
    CHECK(!sim->c->noclip, "noclip off: the client still flies");
    sim->max_correction = 0.0f;
    walk(sim, 1.0, 2, 60.0f);
    CHECK(sim->max_correction < 1e-3f, "noclip off: rubber-banding after the flip %f", sim->max_correction);
    CHECK(sim->c->position.y < height - 5.0f, "noclip off: the client is still floating (y %f from %f)", sim->c->position.y, height);
    sim_settle(sim, 60.0f);
    const float final = sim->s->active ? dist3(sim->c->position, sim->s->position) : 0.0f;
    CHECK(final < 1e-3f, "noclip off: final %f", final);
    printf("  noclip   %3.0f ms: flying max %.6f, flip correction %.3f, final %.6f (server %s)\n", latency, flying, flip, final,
           sim->s->active ? "alive" : "fell to death");
    free(sim);

    // client-only noclip (blocked cheat): the server keeps walking it, "l" pulls it back
    sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
    walk(sim, 1.5, -1, 60.0f);
    sim->c->noclip = 1;
    sim->max_correction = 0.0f;
    for (double t = sim->now + 1.2; sim->now < t;) {
        const Input input = {.move_dir = 2, .x_dir = 0.8f, .y_dir = 0.0f};
        sim_step(sim, &input, 60.0f);
    }
    const float pulled = sim->max_correction;
    CHECK(!sim->c->noclip, "cheat noclip: the client kept its own flag");
    CHECK(pulled > 5.0f, "cheat noclip: not corrected (%f)", pulled);
    sim_settle(sim, 60.0f);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "cheat noclip: final %f", dist3(sim->c->position, sim->s->position));
    printf("  cheat noclip %3.0f ms: pulled back %.3f\n", latency, pulled);

    // client-only position cheat (a speed / teleport hack): same
    sim->c->position.x += 60.0f;
    sim->max_correction = 0.0f;
    walk(sim, 1.5, -1, 60.0f);
    CHECK(sim->max_correction > 50.0f, "cheat move: not corrected (%f)", sim->max_correction);
    sim_settle(sim, 60.0f);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "cheat move: final %f", dist3(sim->c->position, sim->s->position));
    printf("  cheat move   %3.0f ms: pulled back %.3f\n", latency, sim->max_correction);
    free(sim);
}

// gap #8: a respawn restarts the client counter at 0; the server accepts the new life at once.
static void case_respawn(void) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "sandstorm.json", 0, 50.0f, 10.0f)) { CHECK(0, "map"); free(sim); return; }
    walk(sim, 3.0, 2, 60.0f);
    CHECK(sim->s->input_seq > 100, "first life input_seq %d", sim->s->input_seq);

    player_kill(sim->s, NULL, NULL, 1);
    sim_settle(sim, 60.0f);
    player_spawn(sim->s);
    CHECK(sim->s->input_seq == -1, "spawn resets input_seq (%d)", sim->s->input_seq);
    // client SPAWN: same point, counter and pending reset (client_recon_reset)
    player_spawn(sim->c);
    sim->c->position = sim->c->last_position = sim->s->position;
    sim->c->direction = sim->s->direction;
    sim->next_seq = 0;
    sim->pending_count = 0;
    sim->batch_count = 0;
    walk(sim, 1.0, 2, 60.0f);
    CHECK(sim->s->input_seq >= 20, "second life inputs rejected (input_seq %d)", sim->s->input_seq);
    sim_settle(sim, 60.0f);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "respawn: final %f", dist3(sim->c->position, sim->s->position));
    printf("  respawn: second life input_seq %d\n", sim->s->input_seq);
    free(sim);
}

static void case_packets(void) {
    // "q" framing round trip
    uint8_t wire[NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD];
    NetInputBatchPacket batch = {.count = NET_INPUT_BATCH_MAX};
    for (int i = 0; i < NET_INPUT_BATCH_MAX; i++) batch.frames[i] = (NetInputPacket) {.seq = 100 + i, .move_dir = i % 8, .delta = 0.016f, .scroll = -1};
    const uint16_t length = (uint16_t) (1 + batch.count * sizeof(NetInputPacket));
    wire[0] = NET_PACKET_INPUT_BATCH;
    wire[1] = (uint8_t) (length >> 8);
    wire[2] = (uint8_t) length;
    memcpy(wire + 3, &batch, length);
    NetPacket packet = {.type = wire[0], .length = length};
    memcpy(packet.payload, wire + 3, length);
    NetInputBatchPacket read;
    CHECK(net_packet_read_input_batch(&read, &packet) && read.count == NET_INPUT_BATCH_MAX && read.frames[19].seq == 119 &&
          read.frames[3].scroll == -1, "batch round trip");
    packet.length--;
    CHECK(!net_packet_read_input_batch(&read, &packet), "short batch accepted");
    CHECK(length <= NET_MAX_PAYLOAD, "batch %u bytes", length);
}

int main(void) {
    printf("Running reconciliation tests...\n");
    case_packets();

    const float latencies[] = {0.0f, 50.0f, 150.0f};
    for (size_t i = 0; i < sizeof(latencies) / sizeof(latencies[0]); i++) {
        const float jitter = latencies[i] > 0.0f ? latencies[i] * 0.3f : 0.0f;
        case_movement("sandstorm.json", latencies[i], jitter, 60.0f);
        case_movement("kanji.json", latencies[i], jitter, 144.0f);
        case_movement("sandstorm.json", latencies[i], jitter, 30.0f);
    }
    for (size_t i = 0; i < sizeof(latencies) / sizeof(latencies[0]); i++) {
        const float jitter = latencies[i] * 0.3f;
        case_server_moves(latencies[i], jitter);
        case_knockback(latencies[i], jitter);
        case_noclip(latencies[i], jitter);
    }
    case_respawn();
    case_control_no_replay(50.0f, 15.0f);
    case_control_no_replay(150.0f, 45.0f);

    if (g_failures) {
        printf("Reconciliation tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
