// Client prediction + server reconciliation (Krunker v3.1.1 index.js).
//
//   gameLoop: every frame's input b9 gets CONTROLS.getISN(), is pushed onto CONTROLS.tmpInpts
//     and run through procInputs at once (prediction); the frames of one window leave together
//     as one "q" packet. v10 (game.deob.js:221735-221830): the window sums the frame time while
//     our player is active (iiïïiïï += delta) and is sent from the first input frame that finds it
//     >= tickRate (1000 / networkData tf, 30 Hz by default), then restarts at 0.
//   reconcilePlayer ("l", 1 Hz + server-driven snaps): the player is snapped to the server's
//     state after input l[0], the acked inputs leave tmpInpts and the rest replay through
//     procInputs(input, Game, recon = true). No smoothing and no error threshold.
//   "start" (our spawn) -> CONTROLS.reset(): tmpInpts emptied, isn = 0.
//
// Offline (game.is_local) the client's Game is the authority: nothing is recorded or sent.
//
// Debug latency (off by default): KRUNKNATIVE_NET_DELAY_MS=<one-way ms> and
// KRUNKNATIVE_NET_JITTER_MS=<+- ms> hold every received packet and every sent input batch
// that long (order kept, like TCP). KRUNKNATIVE_RECON_LOG=1 prints each correction.
#include <client.h>
#include <game_config.h> // game_network_data: the "q" window (tickRate)
#include <ks_client.h> // ks_client_no_prediction
#include <krunker_net.h> // krunker_net_inputs_paused: an official server's "lock"
#include <ui_logic.h>  // ui_setting_number ("netRate")
#include <ui_mobile.h> // ui_mobile_active (netRate's mobile default)
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double at;
    NetPacket packet;
} DelayedPacket;

typedef struct {
    DelayedPacket *items;
    size_t count, capacity;
    double last_at;
} DelayQueue;

static struct {
    int loaded;
    float delay, jitter; // seconds
    int log;
    DelayQueue in, out;
} g_recon;

static void recon_load_env(void) {
    if (g_recon.loaded) return;
    g_recon.loaded = 1;
    const char *delay = getenv("KRUNKNATIVE_NET_DELAY_MS");
    const char *jitter = getenv("KRUNKNATIVE_NET_JITTER_MS");
    const char *log = getenv("KRUNKNATIVE_RECON_LOG");
    g_recon.delay = delay ? fmaxf(0.0f, (float) atof(delay)) / 1000.0f : 0.0f;
    g_recon.jitter = jitter ? fmaxf(0.0f, (float) atof(jitter)) / 1000.0f : 0.0f;
    g_recon.log = log && log[0] && log[0] != '0';
    if (g_recon.delay > 0.0f || g_recon.jitter > 0.0f) {
        printf("[recon] debug latency: %.0f ms one-way, +-%.0f ms jitter\n", g_recon.delay * 1000.0f, g_recon.jitter * 1000.0f);
    }
}

static int delay_enabled(void) {
    recon_load_env();
    return g_recon.delay > 0.0f || g_recon.jitter > 0.0f;
}

// One packet `delay +- jitter` from now, never before the previous one (an ordered stream).
static void delay_push(DelayQueue *queue, const NetPacket *packet) {
    if (queue->count == queue->capacity) {
        const size_t capacity = queue->capacity ? queue->capacity * 2 : 64;
        DelayedPacket *items = realloc(queue->items, capacity * sizeof(DelayedPacket));
        if (!items) return;
        queue->items = items;
        queue->capacity = capacity;
    }
    const float r = (float) rand() / (float) RAND_MAX * 2.0f - 1.0f;
    double at = glfwGetTime() + g_recon.delay + g_recon.jitter * r;
    if (at < queue->last_at) at = queue->last_at;
    queue->last_at = at;
    queue->items[queue->count].at = at;
    queue->items[queue->count].packet = *packet;
    queue->count++;
}

// Pops the due packets in order into `handle`.
static void delay_drain(DelayQueue *queue, Client *client, void (*handle)(Client*, const NetPacket*)) {
    const double now = glfwGetTime();
    size_t due = 0;
    while (due < queue->count && queue->items[due].at <= now) due++;
    if (!due) return;
    // Copied out first: a handler may push again (INIT -> spawn) or reset the queue.
    DelayedPacket *items = malloc(due * sizeof(DelayedPacket));
    if (!items) return;
    memcpy(items, queue->items, due * sizeof(DelayedPacket));
    memmove(queue->items, queue->items + due, (queue->count - due) * sizeof(DelayedPacket));
    queue->count -= due;
    for (size_t i = 0; i < due; i++) handle(client, &items[i].packet);
    free(items);
}

static void send_now(Client *client, const NetPacket *packet) {
    client_net_send(client, (NetPacketType) packet->type, packet->payload, packet->length);
}

// Inbound debug latency: 1 = `packet` was taken (it comes back through
// client_recon_drain_delayed), 0 = handle it now.
int client_recon_delay_inbound(Client *client, const NetPacket *packet) {
    (void) client;
    if (!delay_enabled()) return 0;
    delay_push(&g_recon.in, packet);
    return 1;
}

void client_recon_drain_delayed(Client *client, void (*handle)(Client*, const NetPacket*)) {
    if (!delay_enabled()) return;
    delay_drain(&g_recon.in, client, handle);
    delay_drain(&g_recon.out, client, send_now);
}

// Krunker's "f" SYNC_FAILED (NET_EXT_KN_INPUTS_DROPPED from krunker_link.c): tmpInpts.length = 0.
// The server dropped them, so no "l" will ack them and none may be replayed.
void client_recon_drop_pending(Client *client) {
    client->net_pending_count = 0;
}

// CONTROLS.reset() on our (re)spawn and on a new session: no pending inputs, a fresh batch.
void client_recon_reset(Client *client) {
    client->net_input_accum = 0.0f; // index.js "start": inputFrameDelta = 0
    client->net_pending_count = 0;
    client->net_batch_count = 0;
    client->net_batch_time = 0.0f;
    client->net_next_input_seq = 0;
}

// v7.1.4 SETTINGS.tmp.netRate: Settings > General > Network Rate (select index 0-5, default 3 =
// 144 Hz, mobile 1 = 60 Hz). WP-D owns the row; until it exists the default applies.
static int recon_net_rate_hz(void) {
    double index = ui_mobile_active() ? 1 : NET_RATE_DEFAULT_INDEX;
    ui_setting_number("netRate", &index);
    return net_rate_hz((int) index);
}

// index.js gameLoop: inputFrameDelta += gameTimeDiff, then shouldSampleInput (net_input_rate_take).
// Online and offline alike (v7's sandbox player samples the same frames). Returns the delta (s) of
// the input frame to build this frame, or -1 when this frame samples none.
float client_recon_sample(Client *client, const float frame_delta) {
    // v10 gameLoop (game.deob.js:223145): `you && you.active && !window.locked`, else no input frame
    // at all: on an official server's "lock" (or while it has not given us our player back after an
    // init) no isn is used, nothing is predicted and neither the frame time nor the "q" window runs on.
    // KrunkNative's own server takes zero-input frames through a lock (mode_input_locked) instead.
    if (!client->game.is_local && krunker_net_inputs_paused()) return -1.0f;
    // v10 gameLoop: iiïïiïï += gameTimeDiff on every frame our player is active (main.c only asks
    // then), sampled or not; the "q" window closes on the next sampled frame past tickRate
    if (!client->game.is_local && isfinite(frame_delta) && frame_delta > 0.0f) client->net_batch_time += frame_delta;
    return net_input_rate_take(&client->net_input_accum, frame_delta, recon_net_rate_hz());
}

// tickRate: 1000 / tickFrequency (applyNetworking tf; our server's NETWORK_DATA tick_hz)
static float recon_window_seconds(const Client *client) {
    const int hz = game_network_data(&client->game)->tick_hz;
    return hz > 0 ? 1.0f / (float) hz : NET_INPUT_SEND_INTERVAL;
}

static void batch_flush(Client *client) {
    if (!client->net_batch_count) return;
    NetPacket packet = {.type = NET_PACKET_INPUT_BATCH};
    packet.payload[0] = (uint8_t) client->net_batch_count;
    memcpy(packet.payload + 1, client->net_batch, client->net_batch_count * sizeof(NetInputPacket));
    packet.length = (uint16_t) (1 + client->net_batch_count * sizeof(NetInputPacket));
    client->net_batch_count = 0;
    if (delay_enabled()) delay_push(&g_recon.out, &packet);
    else send_now(client, &packet);
}

// Whether this frame's input may join the "q" window: our player is alive and the round
// is not over (END seen while playing, until the next INIT; menu_view.c's `ended`).
int client_recon_sending(const Client *client) {
    if (client->game.is_local || !client->me || !client->me->active) return 0;
    return !client_round_ended(client);
}

// END reached us while playing or spectating (the victory/defeat title, the endAnim slow
// motion, the results), until the next INIT. Offline too: the local game's END comes
// through the same events (game_events.c events_end / client_game_events_reset).
int client_round_ended(const Client *client) {
    return client->net_end_seconds >= 0 && client->net_end_result != INT32_MIN;
}

// tmpInpts.push(b9) + the "q" window: called with each input right after it was queued for
// prediction. Online only.
void client_recon_record(Client *client, const Input *input) {
    if (client->game.is_local) return;

    if (client->net_pending_count == client->net_pending_capacity) {
        const size_t capacity = client->net_pending_capacity ? client->net_pending_capacity * 2 : 256;
        Input *pending = realloc(client->net_pending_inputs, capacity * sizeof(Input));
        if (!pending) return;
        client->net_pending_inputs = pending;
        client->net_pending_capacity = capacity;
    }
    client->net_pending_inputs[client->net_pending_count++] = *input;

    // gameLoop only batches `!Game.singlePlayer && !endAnimationTimeLeft`, and the "q" send
    // needs `player && player.active`: nothing leaves while dead (death card, death cam,
    // spectating: no active player, so main.c builds no input at all) or on the end screen.
    // v3.1.1 resumes zero-delta frames once its endAnim reached 0, which its io_q drops in
    // the vote window like our server (end_timer); KrunkNative keeps quiet until the next
    // INIT. The frame still predicts locally and stays in the pending list (tmpInpts).
    if (!client_recon_sending(client)) {
        client->net_batch_count = 0; // the half-filled window is dropped, not sent late
        client->net_batch_time = 0.0f;
        return;
    }

    if (client->net_batch_count >= NET_INPUT_BATCH_MAX) batch_flush(client);
    net_packet_from_input(&client->net_batch[client->net_batch_count++], input);

    // iiïïiïï >= tickRate && (iiïïiïï = 0, send "q") (client_recon_sample sums the frame time)
    if (client->net_batch_time >= recon_window_seconds(client)) {
        client->net_batch_time = 0.0f;
        batch_flush(client);
    }
}

// Our KILL: the half-filled "q" window is never sent (the server killed us before this
// packet left it, and io_q ignores a dead player). The pending inputs stay until the
// respawn's client_recon_reset, like tmpInpts until CONTROLS.reset().
void client_recon_on_death(Client *client) {
    client->net_batch_count = 0;
    client->net_batch_time = 0.0f;
}

// After the frame's game tick ran the input: the shot's physPow knockback becomes that
// input's velObj (players.js shoot: if (!IO) a1.velObj), for later replays.
void client_recon_after_tick(Client *client) {
    if (client->me) net_recon_take_vel_obj(client->me, client->net_pending_inputs, client->net_pending_count);
}

// "l": reconcilePlayer(l, true).
void client_recon_apply(Client *client, const NetReconPacket *packet) {
    Player *me = client->me;
    if (client->game.is_local || !me || !me->active) return;

    const vec3 predicted = me->position;
    const int predicted_active = me->active;
    // KrunkScript DEFAULT.disablePrediction (Game.noPred): the snap applies, nothing is replayed
    if (ks_client_no_prediction()) client->net_pending_count = 0;
    const size_t before = client->net_pending_count;
    const int move_lock = client->game.move_lock || mode_input_locked(me);
    client->net_pending_count = net_reconcile(me, packet, client->net_pending_inputs, client->net_pending_count, move_lock);
    client->net_recon_count++;

    // what the snap + replay moved us (0 when the prediction ran the server's physics): the sync trace's corrections
    const float error = predicted_active && me->active ? sqrtf(powf(me->position.x - predicted.x, 2.0f) + powf(me->position.y - predicted.y, 2.0f) +
                                                               powf(me->position.z - predicted.z, 2.0f))
                                                       : 0.0f;
    if (error > client->net_recon_error_peak) client->net_recon_error_peak = error;
    if (error > NET_RECON_CORRECTION_EPS) client->net_recon_corrections++;

    if (g_recon.log) {
        printf("[recon] #%u ack %d replayed %zu/%zu error %.4f pos %.3f %.3f %.3f noclip %d\n", client->net_recon_count,
               (int) packet->ack_seq, client->net_pending_count, before, error, me->position.x, me->position.y, me->position.z,
               me->noclip);
        fflush(stdout);
    }
}
