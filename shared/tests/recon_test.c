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

    // WP-C v7.1.4 netRate (client/src/net/recon.c client_recon_sample): 0 = an input every frame
    int net_rate;
    float input_accum;
    int fixed_frames;           // frames exactly 1 / fps long (no +-30%)
    unsigned int frames, sampled;
    float min_input_delta, input_time;

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

    // A fixed start, independent of the spawn pick and its RNG use: the scripted walks stay
    // on the map from these spawns (from others, e.g. Kanji's spawns[0] / [1], they walk off
    // an edge and the server kills the player at deathY).
    const int start = strstr(map, "kanji") ? 5 : strstr(map, "sandstorm") ? 4 : -1; // (-128 -144 -108), (241 33 -322)
    const Map *spawn_map = sim->server.map;
    if (spawn_map && start >= 0 && (size_t) start < spawn_map->spawn_count) {
        const Spawn *spawn = spawn_map->spawns[start];
        sim->s->position = sim->s->last_position = spawn->position;
        sim->s->direction.x = 0.0f;
        sim->s->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) spawn->direction);
    }

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
        NetReconPacket recon;
        if (packet.type != NET_PACKET_RECON || !net_packet_read_recon(&recon, &packet, sim->c)) continue;
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
    const float delta = (1.0f / fps) * (sim->fixed_frames ? 1.0f : 0.7f + 0.6f * r);
    sim->now += delta;
    while (sim->server_next_tick <= sim->now) {
        server_tick(sim);
        sim->server_next_tick += 1.0 / 30.0;
    }
    client_receive(sim);
    if (!sim->c->active) return;
    sim->frames++;
    if (!sim->net_rate) {
        client_frame(sim, *input, delta);
        return;
    }
    // netRate: an input frame only once half a period has passed, carrying the summed time;
    // the other frames tick the world only (main.c)
    const float input_delta = net_input_rate_take(&sim->input_accum, delta, sim->net_rate);
    if (input_delta < 0.0f) {
        game_tick(&sim->client, (float) sim->now, delta);
        return;
    }
    if (!sim->sampled || input_delta < sim->min_input_delta) sim->min_input_delta = input_delta;
    sim->sampled++;
    sim->input_time += input_delta;
    client_frame(sim, *input, input_delta);
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
    if (!sim_init(sim, "311_sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
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
    if (!sim_init(sim, "311_sandstorm.json", 4, latency, jitter)) { CHECK(0, "map"); free(sim); return -1.0f; } // Vince: Shotgun
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
    if (!sim_init(sim, "311_sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
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
    if (!sim_init(sim, "311_sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
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
    if (!sim_init(sim, "311_sandstorm.json", 0, latency, jitter)) { CHECK(0, "map"); free(sim); return; }
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
    if (!sim_init(sim, "311_sandstorm.json", 0, 50.0f, 10.0f)) { CHECK(0, "map"); free(sim); return; }
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

// ---------------------------------------------------------------- look rotation ("visual spinbot")

static float angle_gap(const float a, const float b) { // |a - b| on the circle
    return fabsf(normalize_angle(a - b));
}

static int finite_player(const Player *p) {
    return isfinite(p->position.x) && isfinite(p->position.y) && isfinite(p->position.z) && isfinite(p->velocity.x) &&
           isfinite(p->velocity.y) && isfinite(p->velocity.z) && isfinite(p->direction.x) && isfinite(p->direction.y);
}

// Wrap / clamp / round3 / lerp and the STATE / "l" round trips at large and broken angles.
static void case_rotation_math(void) {
    const float pi2 = (float) M_PI * 2.0f;
    const float yaws[] = {0.0f, 1.0f, -1.0f, 6.28f, -6.28f, 6.3f, -6.3f, 100.0f, -1000.5f, -123456.789f, 1e9f, -3.0e38f, 3.4028235e38f};
    for (size_t i = 0; i < sizeof(yaws) / sizeof(yaws[0]); i++) {
        const float w = player_wrap_yaw(yaws[i]);
        CHECK(isfinite(w) && w > -pi2 && w < pi2, "wrap(%g) = %g", yaws[i], w);
        if (fabsf(yaws[i]) < 1e4f) CHECK(angle_gap(w, yaws[i]) < 1e-3f, "wrap(%g) = %g changed the heading", yaws[i], w);
        if (yaws[i] > -pi2 && yaws[i] < pi2) CHECK(w == yaws[i], "wrap(%g) = %g: v3.1.1-range yaws must pass unchanged", yaws[i], w);
        const float r = player_round3(yaws[i]);
        CHECK(isfinite(r), "round3(%g) = %g", yaws[i], r);

        NetStatePacket state = {0};
        Player p = {0};
        p.direction = (vec2) {yaws[i], yaws[i]};
        net_packet_from_player(&state, &p);
        CHECK(state.direction[1] > -pi2 && state.direction[1] < pi2, "STATE yaw %g from %g", state.direction[1], yaws[i]);
        CHECK(fabsf(state.direction[0]) <= (float) M_PI / 2.0f, "STATE pitch %g from %g", state.direction[0], yaws[i]);
        NetReconPacket recon;
        net_packet_from_recon(&recon, &p, 0);
        CHECK(recon.yaw > -pi2 && recon.yaw < pi2, "l yaw %g from %g", recon.yaw, yaws[i]);
    }
    // STATE carries the step value, on-ground, grapple and ping (protocol 1.4.5): the v10 "k" record's fields
    {
        Player p = {0};
        p.step_val = 0.25f;
        p.on_ground = 1;
        p.grapple.active = 1;
        p.grapple.point = (vec3) {1.0f, 2.0f, 3.0f};
        p.ping = 42.0f;
        NetStatePacket st = {0};
        net_packet_from_player(&st, &p);
        CHECK(st.step_val == 0.25f && st.on_ground == 1 && st.grapple_active == 1 && st.grapple[0] == 1.0f && st.grapple[2] == 3.0f && st.ping == 42.0f,
              "STATE carries step, ground, grapple and ping");
        Player q = {0};
        net_packet_apply_player(&q, &st);
        CHECK(q.step_val == 0.25f && q.on_ground && q.grapple.active && q.grapple.point.y == 2.0f && q.ping == 42.0f,
              "the STATE fields apply to the player");

        // The STATE's two sizes: a 57-byte V1 (peers before 1.4.5) reads with its tail at rest, a 79-byte V2 whole,
        // anything else is malformed. A client that only took the 79-byte form dropped every STATE of a 1.4.4
        // server: the others stood at their spawn with frozen legs.
        CHECK(NET_STATE_PACKET_V1_SIZE == 57 && NET_STATE_PACKET_V2_SIZE == 79, "STATE sizes %zu / %zu", (size_t) NET_STATE_PACKET_V1_SIZE, (size_t) NET_STATE_PACKET_V2_SIZE);
        st.position[0] = 7.0f;
        NetStatePacket v1 = {.step_val = 9.0f, .on_ground = 0, .grapple_active = 1, .ping = 1.0f};
        CHECK(net_packet_read_state(&v1, &st, NET_STATE_PACKET_V1_SIZE) == NET_STATE_V1, "a 57-byte STATE reads as V1");
        CHECK(v1.position[0] == 7.0f && v1.step_val == 0.0f && v1.on_ground == 1 && !v1.grapple_active && v1.ping == 0.0f, "V1: the tail reads as rest, grounded");
        NetStatePacket v2 = {0};
        CHECK(net_packet_read_state(&v2, &st, NET_STATE_PACKET_V2_SIZE) == NET_STATE_V2 && v2.step_val == 0.25f && v2.grapple[2] == 3.0f, "a 79-byte STATE reads as V2");
        CHECK(!net_packet_read_state(&v2, &st, 58) && !net_packet_read_state(&v2, &st, 56) && !net_packet_read_state(&v2, &st, 0), "58 / 56 / 0 bytes are malformed");
        Player r = {.step_val = 0.5f, .on_ground = 0, .ping = 77.0f};
        r.grapple.active = 1;
        net_packet_apply_player_v1(&r, &v1);
        CHECK(r.position.x == 7.0f && r.step_val == 0.5f && !r.on_ground && r.grapple.active && r.ping == 77.0f, "the V1 apply keeps step, ground, grapple and ping");
        CHECK(net_peer_has_state_v2(NET_PROTOCOL_MINOR_EXT, NET_PROTOCOL_PATCH_STATE_V2) && net_peer_has_state_v2(NET_PROTOCOL_MINOR_EXT + 1, 0) &&
              !net_peer_has_state_v2(NET_PROTOCOL_MINOR_EXT, NET_PROTOCOL_PATCH_STATE_V2 - 1) && !net_peer_has_state_v2(NET_PROTOCOL_MINOR_V7, 9),
              "the 79-byte STATE goes to 1.4.5+ peers only");
    }
    const float bad[] = {NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < 3; i++) {
        CHECK(player_wrap_yaw(bad[i]) == 0.0f && player_clamp_pitch(bad[i]) == 0.0f, "non-finite look not zeroed");
        NetStatePacket state = {.direction = {bad[i], bad[i]}};
        Player p = {0};
        p.direction = (vec2) {1.0f, 1.0f};
        net_packet_apply_player(&p, &state);
        CHECK(isfinite(p.direction.x) && isfinite(p.direction.y), "STATE apply took a non-finite direction");
        CHECK(isfinite(player_lerp_angle(bad[i], 1.0f, 0.5f)) && isfinite(player_lerp_angle(1.0f, bad[i], 0.5f)), "lerp of non-finite");
    }
    CHECK(fabsf(player_clamp_pitch(10.0f) - (float) M_PI / 2.0f) < 1e-6f && fabsf(player_clamp_pitch(-10.0f) + (float) M_PI / 2.0f) < 1e-6f,
          "pitch clamp");

    // Math.lerpAngle parity inside the v3.1.1 range, and the short way between any two wraps.
    CHECK(fabsf(player_lerp_angle(0.1f, 6.2f, 0.5f) - fmodf(6.2f + (6.383185f - 6.2f) * 0.5f, pi2)) < 1e-4f, "lerp across 2pi");
    CHECK(fabsf(player_lerp_angle(1.0f, 0.5f, 0.5f) - 0.75f) < 1e-6f, "plain lerp");
    for (int k = -6; k <= 6; k++) {
        for (int j = 0; j < 64; j++) {
            const float from = -3.0f + 0.1f * (float) j, step = -0.3f; // a remote player turning right
            const float to = from + step;
            const float a = to + (float) k * pi2, b = from - (float) (k * 3) * pi2; // wrapped differently
            const float mid = player_lerp_angle(a, b, 0.5f);
            CHECK(angle_gap(mid, from + step * 0.5f) < 1e-3f, "lerp(%g, %g) = %g, want %g: long way round", a, b, mid,
                  from + step * 0.5f);
        }
    }
}

// The report: a player turning right for a long time. The old client let its yaw grow without
// bound (input.y_dir -= look forever); remote players interpolated between far-apart unwrapped
// yaws and spun. Here the inputs are the old, unbounded ones: the server must still store and
// send a wrapped yaw, prediction and the authority must agree, and an observer interpolating the
// STATE packets (client_apply_state + player_update) must turn smoothly the short way.
static void case_turn_right(void) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "311_sandstorm.json", 0, 80.0f, 20.0f)) { CHECK(0, "map"); free(sim); return; }

    Player *watched = player_init(&sim->client); // the turning player as another client sees it
    watched->uid = 2;
    game_players_add(&sim->client, watched);
    player_spawn(watched);
    watched->send_rate = 30.0f;

    const float rate = 7.5f; // rad/s to the right: a fast flick, sustained
    float yaw = sim->c->direction.y;
    const float yaw_start = yaw;
    float max_step = 0.0f, max_server_yaw = 0.0f, last_seen = 0.0f, seen_start = 0.0f;
    double turned = 0.0; // the observer's signed turn, frame by frame (a spin adds whole turns)
    int have_seen = 0, states = 0;
    const double start = sim->now, server_tick = 1.0 / 30.0;
    double next_state = sim->now;
    while (sim->now - start < 40.0 && sim->c->active) {
        const double before = sim->now;
        yaw -= rate / 60.0f; // ~ -300 rad by the end: never wrapped by this "client"
        const Input input = {.move_dir = -1, .x_dir = 0.3f, .y_dir = yaw}; // standing: no fall, no respawn
        sim_step(sim, &input, 60.0f);
        const float dt = (float) (sim->now - before);
        max_server_yaw = MAX(max_server_yaw, fabsf(sim->s->direction.y));

        if (sim->now >= next_state) { // "k": the server's STATE of this player to the observer
            next_state += server_tick;
            NetStatePacket state;
            net_packet_from_player(&state, sim->s);
            net_player_apply_state(watched, &state, NET_STATE_V2); // client_apply_state
            states++;
        }
        // the client's game_tick (in sim_step) ran player_update(watched) for this frame
        if (have_seen && states > 3) { // from the 4th STATE: the first ones turn from the spawn yaw
            const float step = normalize_angle(watched->direction.y - last_seen);
            max_step = MAX(max_step, fabsf(step));
            turned += step;
        } else if (states == 3) {
            seen_start = watched->direction.y;
        }
        last_seen = watched->direction.y;
        have_seen = states >= 3;
        (void) dt;
    }
    for (int i = 0; i < 30; i++) { // let the observer reach the last STATE
        game_tick(&sim->client, (float) (sim->now += 1.0 / 60.0), 1.0f / 60.0f);
        turned += normalize_angle(watched->direction.y - last_seen);
        last_seen = watched->direction.y;
    }
    sim_settle(sim, 60.0f);
    const float pi2 = (float) M_PI * 2.0f;
    // The observer starts from its first STATE (seen_start ~ the spawn yaw) and must end on
    // the sent heading having turned exactly as far as the sender, frame steps well below pi.
    const double want = (double) yaw - (double) yaw_start - normalize_angle(seen_start - yaw_start);
    printf("  turn right: sent yaw %.1f, server |yaw| <= %.3f, observer turned %.2f rad (sent %.2f), max %.3f rad/frame, %d states\n",
           yaw, max_server_yaw, turned, want, max_step, states);
    CHECK(yaw < -250.0f, "the test did not turn far enough (%f)", yaw);
    CHECK(max_server_yaw < pi2, "server yaw left (-2pi, 2pi): %f", max_server_yaw);
    CHECK(max_step < 1.0f, "observer yaw jumped %f rad in one frame (spin)", max_step);
    // Up to latency * rate (the inputs still in flight when the last STATE left) short; every
    // spin would add a whole 2pi.
    CHECK(fabs(turned - want) < 1.5, "observer turned %f rad, the player %f rad: it spun", turned, want);
    CHECK(angle_gap(sim->c->direction.y, sim->s->direction.y) < 1e-3f, "client yaw %f != server yaw %f", sim->c->direction.y,
          sim->s->direction.y);
    CHECK(angle_gap(sim->s->direction.y, yaw) < 1e-2f, "server heading %f != sent heading %f", sim->s->direction.y, yaw);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "turning: client/server diverged by %f", dist3(sim->c->position, sim->s->position));
    CHECK(finite_player(sim->s), "server player went non-finite");
    free(sim);
}

// A hostile client: huge, NaN and inf look values (single frames, as the server's
// player_accept_input sees them) must never reach the physics, the STATE or the "l".
// A remote player's walk cycle (player_mesh.c legs: sin(step_val)) is the server's step value, lerped
// between consecutive STATEs like the position (v10.js:169517 posBuffer 's'); it never re-derives it
// from the distance covered (which a 10 Hz STATE stream, snapped by every packet, turned into a stall)
// and the playerStep sound fires at each zero crossing (posStep). A V1 STATE (a server before 1.4.5)
// carries no step value: the cycle comes from the distance covered, as before.
static void case_remote_step(void) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "311_sandstorm.json", 0, 0.0f, 0.0f)) { CHECK(0, "map"); free(sim); return; }

    Player *watched = player_init(&sim->client);
    watched->uid = 2;
    game_players_add(&sim->client, watched);
    player_spawn(watched);
    watched->send_rate = 10.0f;
    watched->force_pos = 1; // SPAWN: the first STATE snaps (client_apply_spawn)
    const vec3 origin = watched->position;

    NetStatePacket state = {0};
    net_packet_from_player(&state, watched);
    state.uid = 2;
    state.active = 1;
    state.step_val = 0.0f;
    state.on_ground = 1;
    net_player_apply_state(watched, &state, NET_STATE_V2);
    CHECK(!watched->interpolate && watched->step_carried && watched->step_val == 0.0f, "the first STATE after a spawn snaps");

    // the server's player walks: stepVal 0 -> 1.2 while it moves 3 units, then -1.2 (its stepDir flipped)
    const float steps[] = {1.2f, -1.2f, 0.9f};
    const unsigned short events_before = watched->step_events;
    float min_mid = 10.0f, max_mid = -10.0f, prev = 0.0f;
    int monotone = 1;
    for (size_t i = 0; i < 3; i++) {
        state.position[0] = origin.x + 3.0f * (float) (i + 1);
        state.step_val = steps[i];
        net_player_apply_state(watched, &state, NET_STATE_V2);
        CHECK(watched->interpolate && watched->step_val == prev && watched->interp_step_end == steps[i], "STATE %zu: the step value is the target, not the value", i);
        // 100 ms (one send period at 10 Hz / interpolation 1.1 -> ~91% of the way) in 6 frames
        for (int f = 0; f < 6; f++) {
            const float before = watched->step_val;
            game_tick(&sim->client, (float) (sim->now += 1.0 / 60.0), 1.0f / 60.0f);
            if (f == 2) { min_mid = MIN(min_mid, watched->step_val); max_mid = MAX(max_mid, watched->step_val); }
            if ((steps[i] > prev && watched->step_val < before - 1e-6f) || (steps[i] < prev && watched->step_val > before + 1e-6f)) monotone = 0;
        }
        CHECK(fabsf(watched->step_val - steps[i]) < 0.12f * fabsf(steps[i] - prev) + 1e-3f, "STATE %zu: step %f did not reach %f from %f", i, watched->step_val, steps[i], prev);
        prev = watched->step_val;
    }
    CHECK(monotone, "the step value moved away from its target between states");
    CHECK(min_mid > -1.1f && max_mid < 1.1f, "mid-way values %f / %f: the step is lerped, not snapped", min_mid, max_mid);
    CHECK(watched->step_events == events_before + 3, "playerStep at each zero crossing: %u events, expected 3", watched->step_events - events_before);
    CHECK(watched->position.x > origin.x + 8.0f, "the position interpolated to %f (origin %f)", watched->position.x, origin.x);

    // a V1 STATE (no step value): the cycle from the distance covered, the carried value is forgotten
    NetStatePacket v1;
    net_packet_read_state(&v1, &state, NET_STATE_PACKET_V1_SIZE);
    v1.position[0] = origin.x + 20.0f;
    const float carried = watched->step_val;
    net_player_apply_state(watched, &v1, NET_STATE_V1);
    CHECK(!watched->step_carried && watched->on_ground, "V1: nothing carried, grounded");
    for (int f = 0; f < 6; f++) game_tick(&sim->client, (float) (sim->now += 1.0 / 60.0), 1.0f / 60.0f);
    CHECK(fabsf(watched->step_val - carried) > 0.1f, "V1: the step value %f still moves with the distance (was %f)", watched->step_val, carried);
    free(sim);
}

static void case_bad_rotation(void) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "311_sandstorm.json", 0, 0.0f, 0.0f)) { CHECK(0, "map"); free(sim); return; }
    const float values[] = {NAN, INFINITY, -INFINITY, 3.4028235e38f, -3.4028235e38f, 1e30f, -1e30f, 2.2e9f, -2.2e9f,
                            1e-45f, -0.0f, 123456.0f, 6.2831855f, -6.2831855f};
    const size_t n = sizeof(values) / sizeof(values[0]);
    int seq = 0;
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < n; j++) {
                Input input = {.seq = seq++, .move_dir = (int) ((i + j) % 9) - 1, .delta = 1.0f / 60.0f, .x_dir = values[i],
                               .y_dir = values[j], .shoot = (int) (j & 1), .jump = (int) (i & 1)};
                player_accept_input(sim->s, &input);
                NetInputPacket wire;
                net_packet_from_input(&wire, &input); // and the raw prediction path on the client
                Input back;
                net_packet_to_input(&back, &wire);
                if (sim->c->active) player_proc_input(sim->c, &back, 0, 0);
            }
            game_tick(&sim->server, (float) (sim->now += 1.0 / 30.0), 1.0f / 30.0f);
            if (!sim->s->active) player_spawn(sim->s);
            CHECK(finite_player(sim->s), "server player non-finite after look %g", values[i]);
            CHECK(fabsf(sim->s->direction.y) < (float) M_PI * 2.0f && fabsf(sim->s->direction.x) <= (float) M_PI / 2.0f,
                  "server look %g %g out of range", sim->s->direction.x, sim->s->direction.y);
            CHECK(!sim->c->active || finite_player(sim->c), "predicted player non-finite after look %g", values[i]);
        }
    }
    NetStatePacket state;
    net_packet_from_player(&state, sim->s);
    CHECK(isfinite(state.position[0]) && isfinite(state.direction[1]), "STATE non-finite");
    printf("  bad rotation: %d frames, server player finite at %.2f %.2f %.2f yaw %.3f\n", seq, sim->s->position.x,
           sim->s->position.y, sim->s->position.z, sim->s->direction.y);
    free(sim);
}

// ---------------------------------------------------------------- WP-C: netRate, held jump, v7 RECON

// v7.1.4 netRate sampling (index.js gameLoop): at 144 Hz the input rate follows the render rate up to
// ~288 fps (a frame of at least half a period samples), every input frame carries the time since the
// previous one, and prediction + reconciliation stay exact.
static void case_net_rate(const float fps, const int rate, const unsigned int want_per_second) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "311_sandstorm.json", 0, 50.0f, 15.0f)) { CHECK(0, "map"); free(sim); return; }
    sim->net_rate = rate;
    sim->fixed_frames = 1;
    const double start = sim->now;
    while (sim->now - start < 4.0 && sim->c->active) {
        const Input input = script_input(sim, sim->now - start);
        sim_step(sim, &input, fps);
    }
    const double elapsed = sim->now - start;
    const float per_second = (float) sim->sampled / (float) elapsed;
    const float period = 1.0f / (float) rate;
    printf("  netRate %3d Hz @%3.0f fps: %u of %u frames sampled (%.1f/s), min input %.2f ms\n", rate, fps, sim->sampled, sim->frames,
           per_second, sim->min_input_delta * 1000.0f);
    CHECK(fabsf(per_second - (float) want_per_second) <= 2.0f, "netRate %d @%.0f fps: %.1f inputs/s, want %u", rate, fps, per_second, want_per_second);
    CHECK(sim->min_input_delta >= period * 0.5f - 1e-4f, "an input frame shorter than half a period (%.3f ms)", sim->min_input_delta * 1000.0f);
    CHECK(fabsf(sim->input_time + sim->input_accum - (float) elapsed) < 1e-3f, "input time %.4f + %.4f pending != %.4f elapsed", sim->input_time,
          sim->input_accum, elapsed);
    sim->fixed_frames = 0; // +-30% frames for the rest
    walk(sim, 3.0, 2, fps);
    sim_settle(sim, fps);
    CHECK(sim->max_correction < 1e-3f, "netRate %d @%.0f fps: rubber-banding %f", rate, fps, sim->max_correction);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "netRate %d @%.0f fps: final %f", rate, fps, dist3(sim->c->position, sim->s->position));
    free(sim);
}

static void case_net_rate_math(void) {
    static const int rates[NET_RATE_COUNT] = {30, 60, 120, 144, 240, 360};
    for (int i = 0; i < NET_RATE_COUNT; i++) CHECK(net_rate_hz(i) == rates[i], "netRate option %d", i);
    CHECK(net_rate_hz(-1) == 144 && net_rate_hz(NET_RATE_COUNT) == 144, "netRate default 144 Hz");
    float accum = 0.0f;
    CHECK(net_input_rate_take(&accum, 0.0f, 144) < 0.0f && accum == 0.0f, "a zero frame samples nothing");
    CHECK(net_input_rate_take(&accum, 0.003f, 144) < 0.0f && fabsf(accum - 0.003f) < 1e-7f, "3 ms < half of 6.94 ms");
    CHECK(fabsf(net_input_rate_take(&accum, 0.0005f, 144) - 0.0035f) < 1e-7f && accum == 0.0f, "3.5 ms rounds up to one period");
    CHECK(fabsf(net_input_rate_take(&accum, 0.1f, 30) - 0.1f) < 1e-7f, "a long frame is one input frame");
    CHECK(net_input_rate_take(&accum, NAN, 30) < 0.0f && accum == 0.0f, "NaN frame time ignored");
}

// v7.1.4 jump input is held (J4): without autoJump one jump per press (tryJump waits for a release),
// with autoJump a jump on every landing while held; both predicted exactly.
static void case_held_jump(const int auto_jump) {
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "311_sandstorm.json", 0, 80.0f, 20.0f)) { CHECK(0, "map"); free(sim); return; }
    sim->server.config.auto_jump = sim->client.config.auto_jump = (unsigned char) auto_jump;
    sim->net_rate = 144;
    walk(sim, 1.0, -1, 120.0f);
    int jumps = 0, was_ground = sim->s->on_ground;
    const double until = sim->now + 3.0;
    while (sim->now < until) {
        const Input input = {.move_dir = -1, .jump = 1, .x_dir = sim->c->direction.x, .y_dir = sim->c->direction.y};
        sim_step(sim, &input, 120.0f);
        if (was_ground && !sim->c->on_ground && sim->c->velocity.y > 0.0f) jumps++;
        was_ground = sim->c->on_ground;
    }
    sim_settle(sim, 120.0f);
    printf("  held jump (autoJump %d): %d jumps in 3 s, max correction %.6f\n", auto_jump, jumps, sim->max_correction);
    CHECK(auto_jump ? jumps >= 3 : jumps == 1, "held jump, autoJump %d: %d jumps", auto_jump, jumps);
    CHECK(sim->max_correction < 1e-3f, "held jump: rubber-banding %f", sim->max_correction);
    CHECK(dist3(sim->c->position, sim->s->position) < 1e-3f, "held jump: final %f", dist3(sim->c->position, sim->s->position));
    free(sim);
}

// RECON V2 carries wallJumpC / canSlideT / the exact slide ms (v7.1.4 io_syncPlayer [12] / [24]); a
// V1 RECON (a 1.3.0 server, net-bridge) keeps the client's own values. Input reset / interact ride
// in the input flags (map-logic WP-B's input[13] / [14]).
static void case_v7_recon_packets(void) {
    Player s = {0}, c = {0};
    s.position = (vec3) {1.0f, 2.0f, 3.0f};
    s.wall_jump_count = 3;
    s.can_slide_timer = 0.07f;
    s.slide_ms = 312.3456789;
    s.slide_timer = (float) (s.slide_ms / 1000.0);
    NetReconPacket out;
    net_packet_from_recon(&out, &s, 42);
    NetPacket packet = {.type = NET_PACKET_RECON, .length = (uint16_t) NET_RECON_PACKET_V2_SIZE};
    memcpy(packet.payload, &out, sizeof(out));
    NetReconPacket in;
    CHECK(net_packet_read_recon(&in, &packet, NULL), "RECON V2 read");
    net_packet_apply_recon(&c, &in);
    CHECK(c.wall_jump_count == 3 && c.can_slide_timer == 0.07f && c.slide_ms == s.slide_ms && c.slide_timer == s.slide_timer,
          "RECON V2: wallJumpC %d canSlideT %f slide %.7f ms", c.wall_jump_count, (double) c.can_slide_timer, c.slide_ms);
    // a 1.3.0 server: 58 bytes, the client's own locks stay
    Player keep = {0};
    keep.wall_jump_count = 1;
    keep.can_slide_timer = 0.02f;
    packet.length = (uint16_t) NET_RECON_PACKET_V1_SIZE;
    CHECK(net_packet_read_recon(&in, &packet, &keep), "RECON V1 read");
    net_packet_apply_recon(&keep, &in);
    CHECK(keep.wall_jump_count == 1 && keep.can_slide_timer == 0.02f && keep.slide_timer == s.slide_timer, "RECON V1 keeps the client's locks");
    packet.length = (uint16_t) NET_RECON_PACKET_V1_SIZE + 3;
    CHECK(!net_packet_read_recon(&in, &packet, NULL), "RECON with a cut v7 tail accepted");
    packet.length = (uint16_t) NET_RECON_PACKET_V2_SIZE + 8;
    CHECK(net_packet_read_recon(&in, &packet, NULL) && in.wall_jump_count == 3, "RECON longer than V2 reads its prefix");
    packet.length = 20;
    CHECK(!net_packet_read_recon(&in, &packet, NULL), "short RECON accepted");

    for (int reset = 0; reset <= 2; reset++) {
        for (int interact = 0; interact <= 1; interact++) {
            const Input input = {.seq = 9, .move_dir = 3, .delta = 0.007f, .jump = 1, .reset = (unsigned char) reset, .interact_held = (unsigned char) interact};
            NetInputPacket wire;
            net_packet_from_input(&wire, &input);
            Input back;
            net_packet_to_input(&back, &wire);
            CHECK(back.reset == reset && back.interact_held == interact && back.jump && back.seq == 9, "input reset %d interact %d", reset, interact);
        }
    }

    // a client that predicted wall jumps the server did not see is reset by the next "l"
    Sim *sim = calloc(1, sizeof(Sim));
    if (!sim_init(sim, "311_sandstorm.json", 0, 50.0f, 15.0f)) { CHECK(0, "map"); free(sim); return; }
    walk(sim, 1.0, 2, 60.0f);
    sim->c->wall_jump_count = 99;
    sim->c->can_slide_timer = 0.09f;
    const unsigned int recons = sim->recons;
    walk(sim, 1.5, 2, 60.0f);
    CHECK(sim->recons > recons && sim->c->wall_jump_count == sim->s->wall_jump_count, "the next \"l\" resets wallJumpC (%d, server %d)",
          sim->c->wall_jump_count, sim->s->wall_jump_count);
    free(sim);
}

int main(void) {
    printf("Running reconciliation tests...\n");
    case_packets();

    const float latencies[] = {0.0f, 50.0f, 150.0f};
    for (size_t i = 0; i < sizeof(latencies) / sizeof(latencies[0]); i++) {
        const float jitter = latencies[i] > 0.0f ? latencies[i] * 0.3f : 0.0f;
        case_movement("311_sandstorm.json", latencies[i], jitter, 60.0f);
        case_movement("311_kanji.json", latencies[i], jitter, 144.0f);
        case_movement("311_sandstorm.json", latencies[i], jitter, 30.0f);
    }
    for (size_t i = 0; i < sizeof(latencies) / sizeof(latencies[0]); i++) {
        const float jitter = latencies[i] * 0.3f;
        case_server_moves(latencies[i], jitter);
        case_knockback(latencies[i], jitter);
        case_noclip(latencies[i], jitter);
    }
    case_respawn();
    case_net_rate_math();
    case_v7_recon_packets();
    case_net_rate(30.0f, 144, 30);   // a sample every frame
    case_net_rate(60.0f, 144, 60);
    case_net_rate(144.0f, 144, 144);
    case_net_rate(240.0f, 144, 240); // 4.17 ms >= 3.47 ms: still every frame (v7 roundToClos)
    case_net_rate(360.0f, 144, 180); // every other frame
    case_net_rate(144.0f, 30, 48);   // 30 Hz: every third 6.94 ms frame (20.8 ms >= 16.7 ms)
    case_held_jump(0);
    case_held_jump(1);
    case_control_no_replay(50.0f, 15.0f);
    case_control_no_replay(150.0f, 45.0f);
    // Last: these games draw from the shared spawn RNG, and the cases above expect its sequence.
    case_rotation_math();
    case_turn_right();
    case_remote_step();
    case_bad_rotation();

    if (g_failures) {
        printf("Reconciliation tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
