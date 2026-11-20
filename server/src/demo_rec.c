// K.P.D demo recorder (server/include/server_demo.h, doc/v10/replays.md 2.1).
//
// Krunker v10.0.7's demo is the packet stream a spectator receives, with keyframes, cut around a K.P.D
// call (deob-10.0.7.js:201618-201646, the loader; the recorder itself is Krunker's server code). Here a
// hidden spectator connection (net_add_recorder) takes the place of that spectator: what the server sends
// it, through either send path, lands in the tick's stream record; a keyframe is its join burst again
// (net_send_demo_keyframe). The window keeps DEMO_BEFORE_S + DEMO_AFTER_S of history from a keyframe on;
// a call's demo is cut DEMO_AFTER_S after it (or when the suspect leaves / the server stops) and goes to
// kn_brain (kpd_demo_chunk / kpd_demo_done frames on the internal socket) and / or gameserver.demo_dir.
#include <server_demo.h>

#include <brain.h>
#include <cJSON.h>
#include <demo.h>
#include <editor_bus.h> // edbus_base64
#include <knet.h>       // NET_DEMO_FD_BASE, net_set_demo_send
#include <modes.h>
#include <server_config.h>
#include <server_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <server_accounts.h> // accounts_kpd_call_type
#include <time.h>

#define DEMO_FD (NET_DEMO_FD_BASE + 1)
#define DEMO_PENDING_MAX 8

typedef struct {
    double t; // seconds of tick_wall
    uint8_t kind;
    uint8_t *bytes;
    uint32_t length;
} RecRecord;

typedef struct {
    int used;
    double at, due;
    int suspect_id;      // connection id
    int32_t suspect_uid; // its player uid when called (-1 none)
    char suspect_name[SERVER_PLAYER_NAME_MAX];
    int suspect_account;
    int call_type;
    int64_t call_id;
    char token[16]; // the call token (Krunker's ct)
} RecCall;

static struct {
    int started;
    int recorder_id;
    int capture_keyframe; // net_send_demo_keyframe is running: the frames go to `keyframe`
    DemoBuf tick, keyframe;
    RecRecord *records;
    size_t count, capacity;
    double next_keyframe;
    int keyframe_due; // an INIT went by in the stream: a keyframe on the next tick
    RecCall calls[DEMO_PENDING_MAX];
    int written;
} g_rec = {.recorder_id = -1};

static double rec_now(void) {
    return g_server ? g_server->tick_wall : 0.0;
}

static int rec_enabled(void) {
    return g_gs_config.demos && (g_gs_config.demo_dir[0] || brain_enabled());
}

// ------------------------------------------------------------------ the recorder's transport

static int rec_capture(const uint8_t *frames, const size_t length) {
    DemoBuf *to = g_rec.capture_keyframe ? &g_rec.keyframe : &g_rec.tick;
    if (!g_rec.capture_keyframe) {
        // an INIT in the stream (a map change, a new round's reset): the next tick takes a keyframe
        for (size_t at = 0; at + NET_PACKET_HEADER_SIZE <= length;) {
            if (frames[at] == NET_PACKET_INIT) g_rec.keyframe_due = 1;
            at += NET_PACKET_HEADER_SIZE + (size_t) (frames[at + 1] << 8 | frames[at + 2]);
        }
    }
    return demo_buf_append(to, frames, length);
}

static int rec_send(const int fd, const uint8_t type, const void *payload, const uint16_t length) {
    if (fd != DEMO_FD || length > NET_MAX_PAYLOAD) return 0;
    uint8_t frame[NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD];
    frame[0] = type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) length;
    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);
    return rec_capture(frame, NET_PACKET_HEADER_SIZE + length);
}

static int rec_poll(ClientConnection *client) {
    (void) client;
    return 1; // it sends nothing
}

static void rec_send_frames(const int fd, const uint8_t *frames, const size_t length) {
    if (fd == DEMO_FD) rec_capture(frames, length);
}

static void rec_close(const int fd) {
    (void) fd;
}

static double rec_received_at(const int fd) {
    (void) fd;
    return 0.0;
}

static const ServerTransport g_rec_transport = {
    .name = "demo",
    .poll = rec_poll,
    .send_frames = rec_send_frames,
    .close = rec_close,
    .pong_received_at = rec_received_at,
    .input_received_at = rec_received_at,
};

// ------------------------------------------------------------------ the window

static int rec_push(const double t, const uint8_t kind, DemoBuf *bytes) {
    if (!bytes->length && kind == DEMO_REC_STREAM) return 1;
    if (g_rec.count == g_rec.capacity) {
        const size_t cap = g_rec.capacity ? g_rec.capacity * 2 : 1024;
        RecRecord *grown = realloc(g_rec.records, cap * sizeof(*grown));
        if (!grown) return 0;
        g_rec.records = grown;
        g_rec.capacity = cap;
    }
    uint8_t *copy = malloc(bytes->length ? bytes->length : 1);
    if (!copy) return 0;
    memcpy(copy, bytes->data, bytes->length);
    g_rec.records[g_rec.count++] = (RecRecord) {.t = t, .kind = kind, .bytes = copy, .length = (uint32_t) bytes->length};
    bytes->length = 0;
    return 1;
}

static void rec_take_keyframe(const double t) {
    if (g_rec.recorder_id < 0) return;
    rec_push(t, DEMO_REC_STREAM, &g_rec.tick); // what came before it stays before it
    g_rec.capture_keyframe = 1;
    g_rec.keyframe.length = 0;
    net_send_demo_keyframe(g_rec.recorder_id);
    g_rec.capture_keyframe = 0;
    if (g_rec.keyframe.length) rec_push(t, DEMO_REC_KEYFRAME, &g_rec.keyframe);
    g_rec.next_keyframe = t + DEMO_KEYFRAME_S;
    g_rec.keyframe_due = 0;
}

// Drops what no demo can need any more: everything before the last keyframe that is older than the
// oldest pending call's history (or than the full window when none is pending).
static void rec_trim(const double now) {
    double keep_from = now - (DEMO_BEFORE_S + DEMO_AFTER_S + DEMO_KEYFRAME_S);
    for (int i = 0; i < DEMO_PENDING_MAX; i++) {
        if (g_rec.calls[i].used && g_rec.calls[i].at - DEMO_BEFORE_S < keep_from) keep_from = g_rec.calls[i].at - DEMO_BEFORE_S;
    }
    size_t first = 0;
    for (size_t i = 0; i < g_rec.count && g_rec.records[i].t <= keep_from; i++) {
        if (g_rec.records[i].kind == DEMO_REC_KEYFRAME) first = i;
    }
    if (!first) return;
    for (size_t i = 0; i < first; i++) free(g_rec.records[i].bytes);
    memmove(g_rec.records, g_rec.records + first, (g_rec.count - first) * sizeof(*g_rec.records));
    g_rec.count -= first;
}

// ------------------------------------------------------------------ writing a demo

static void rec_token(char *out, const size_t size) {
    brain_random_hex(out, size, 6); // 12 hex characters
}

static const char *rec_map_name(void) {
    const Game *game = &g_server->game;
    const int index = game->current_map_index;
    const cJSON *map = index >= 0 && (size_t) index < game->map_count ? game->maps[index] : NULL;
    const cJSON *name = cJSON_GetObjectItem(map, "name");
    return cJSON_IsString(name) ? name->valuestring : "";
}

// The demo bytes of [from, to] starting at the last keyframe at or before `from`, under DEMO_MAX_BYTES
// (later keyframes first when it is too long). meta_out: its meta (caller frees).
static int rec_build(const double from, const double to, cJSON *meta, DemoBuf *out) {
    size_t start = SIZE_MAX;
    for (size_t i = 0; i < g_rec.count && g_rec.records[i].t <= to; i++) {
        if (g_rec.records[i].kind != DEMO_REC_KEYFRAME) continue;
        if (g_rec.records[i].t <= from || start == SIZE_MAX) start = i;
    }
    if (start == SIZE_MAX) return 0;
    for (;;) {
        size_t bytes = 0, end = start;
        for (; end < g_rec.count && g_rec.records[end].t <= to; end++) bytes += DEMO_RECORD_HEADER_SIZE + g_rec.records[end].length;
        const double t0 = g_rec.records[start].t;
        const double duration = end > start ? g_rec.records[end - 1].t - t0 : 0.0;
        cJSON_DeleteItemFromObject(meta, "duration");
        cJSON_AddNumberToObject(meta, "duration", (double) (int64_t) (duration * 1000.0));
        char *text = cJSON_PrintUnformatted(meta);
        const size_t total = DEMO_HEADER_SIZE + (text ? strlen(text) : 0) + bytes;
        if (total > DEMO_MAX_BYTES) {
            free(text);
            size_t next = SIZE_MAX;
            for (size_t i = start + 1; i < end; i++) {
                if (g_rec.records[i].kind == DEMO_REC_KEYFRAME) {
                    next = i;
                    break;
                }
            }
            if (next == SIZE_MAX) return 0; // one keyframe span over the cap: nothing sensible to cut
            start = next;
            continue;
        }
        out->length = 0;
        int ok = demo_write_header(out, text ? text : "{}");
        free(text);
        for (size_t i = start; ok && i < end; i++) {
            const RecRecord *r = &g_rec.records[i];
            ok = demo_write_record(out, (uint32_t) ((r->t - t0) * 1000.0 + 0.5), r->kind, r->bytes, r->length);
        }
        return ok;
    }
}

static int rec_write_file(const char *token, const DemoBuf *demo, const cJSON *meta) {
    if (!g_gs_config.demo_dir[0]) return 0;
    char path[GS_STR_LARGE + 48];
    snprintf(path, sizeof(path), "%s/%s.kndemo", g_gs_config.demo_dir, token);
    FILE *file = fopen(path, "wb");
    if (!file) {
        server_log("[demo] cannot write %s", path);
        return 0;
    }
    const int ok = fwrite(demo->data, 1, demo->length, file) == demo->length;
    fclose(file);
    snprintf(path, sizeof(path), "%s/%s.json", g_gs_config.demo_dir, token);
    char *text = cJSON_PrintUnformatted(meta);
    file = text ? fopen(path, "wb") : NULL;
    if (file) {
        fputs(text, file);
        fclose(file);
    }
    free(text);
    return ok;
}

// kpd_demo_chunk {tok, off, data (base64)} ... then kpd_demo_done {tok, size, call, ct, suspect, suspect_name,
// typ, map, mode, dur}: reliable frames (resent after a reconnect until the brain answers).
static int rec_upload(const char *token, const DemoBuf *demo, const RecCall *call, const cJSON *meta) {
    if (!brain_enabled()) return 0;
    for (size_t off = 0; off < demo->length; off += DEMO_CHUNK_BYTES) {
        const size_t n = demo->length - off < DEMO_CHUNK_BYTES ? demo->length - off : DEMO_CHUNK_BYTES;
        char *b64 = edbus_base64(demo->data + off, n);
        if (!b64) return 0;
        cJSON *frame = cJSON_CreateObject();
        cJSON_AddStringToObject(frame, "type", "acct");
        cJSON_AddNumberToObject(frame, "rid", brain_next_rid());
        cJSON_AddStringToObject(frame, "op", "kpd_demo_chunk");
        cJSON *args = cJSON_AddObjectToObject(frame, "args");
        cJSON_AddStringToObject(args, "tok", token);
        cJSON_AddNumberToObject(args, "off", (double) off);
        cJSON_AddStringToObject(args, "data", b64);
        free(b64);
        if (brain_post(frame, 1) != 0) return 0;
    }
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "acct");
    cJSON_AddNumberToObject(frame, "rid", brain_next_rid());
    cJSON_AddStringToObject(frame, "op", "kpd_demo_done");
    cJSON *args = cJSON_AddObjectToObject(frame, "args");
    cJSON_AddStringToObject(args, "tok", token);
    cJSON_AddNumberToObject(args, "size", (double) demo->length);
    cJSON_AddNumberToObject(args, "call", (double) call->call_id);
    cJSON_AddStringToObject(args, "ct", call->token);
    cJSON_AddNumberToObject(args, "suspect", call->suspect_account);
    cJSON_AddStringToObject(args, "suspect_name", call->suspect_name);
    cJSON_AddNumberToObject(args, "typ", call->call_type);
    cJSON_AddItemToObject(args, "map", cJSON_Duplicate(cJSON_GetObjectItem(meta, "map"), 1));
    cJSON_AddItemToObject(args, "mode", cJSON_Duplicate(cJSON_GetObjectItem(meta, "mode"), 1));
    cJSON_AddItemToObject(args, "dur", cJSON_Duplicate(cJSON_GetObjectItem(meta, "duration"), 1));
    return brain_post(frame, 1) == 0;
}

// The demo of call slot i (and of the other pending calls on the same suspect inside its window: one demo,
// several meta.calls), then the slots are free.
static void rec_finish(const int index) {
    RecCall *call = &g_rec.calls[index];
    if (!call->used) return;
    const double now = rec_now();
    rec_push(now, DEMO_REC_STREAM, &g_rec.tick);
    const double from = call->at - DEMO_BEFORE_S;
    const double to = now;

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "v", DEMO_VERSION);
    cJSON_AddNumberToObject(meta, "created", (double) time(NULL) * 1000.0);
    cJSON_AddStringToObject(meta, "map", rec_map_name());
    const GameModeConfig *mode = mode_config(game_mode_id(&g_server->game));
    cJSON_AddStringToObject(meta, "mode", mode && mode->name ? mode->name : "");
    cJSON_AddNumberToObject(meta, "typ", call->call_type);
    cJSON_AddStringToObject(meta, "server", g_gs_config.node_id);
    cJSON *calls = cJSON_AddArrayToObject(meta, "calls");
    cJSON *suspects = cJSON_AddArrayToObject(meta, "suspects");
    cJSON_AddItemToArray(suspects, cJSON_CreateNumber(call->suspect_uid));
    for (int i = 0; i < DEMO_PENDING_MAX; i++) {
        RecCall *c = &g_rec.calls[i];
        if (!c->used || c->suspect_id != call->suspect_id || c->at < from || c->at > to) continue;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "token", c->token);
        cJSON_AddNumberToObject(entry, "suspect", c->suspect_uid);
        cJSON_AddNumberToObject(entry, "call", (double) c->call_id);
        cJSON_AddItemToArray(calls, entry);
        if (i != index) c->used = 0;
    }

    DemoBuf demo = {0};
    char token[16];
    rec_token(token, sizeof(token));
    if (rec_build(from, to, meta, &demo)) {
        const int filed = rec_write_file(token, &demo, meta);
        const int sent = rec_upload(token, &demo, call, meta);
        g_rec.written++;
        server_log("[demo] %s: %zu bytes, %.0f s, suspect %s%s%s", token, demo.length,
                   cJSON_GetNumberValue(cJSON_GetObjectItem(meta, "duration")) / 1000.0, call->suspect_name,
                   filed ? ", written to demo_dir" : "", sent ? ", sent to the brain" : "");
    } else {
        server_log("[demo] no demo for the call on %s (nothing recorded)", call->suspect_name);
    }
    demo_buf_free(&demo);
    cJSON_Delete(meta);
    call->used = 0;
}

// ------------------------------------------------------------------ the hooks

static void rec_start(void) {
    if (g_rec.started || !rec_enabled() || !g_server->game.ready) return;
    net_set_demo_send(rec_send);
    g_rec.capture_keyframe = 1;
    g_rec.keyframe.length = 0;
    g_rec.recorder_id = net_add_recorder(&g_rec_transport, DEMO_FD);
    g_rec.capture_keyframe = 0;
    if (g_rec.recorder_id < 0) {
        net_set_demo_send(NULL);
        return;
    }
    g_rec.started = 1;
    const double now = rec_now();
    rec_push(now, DEMO_REC_KEYFRAME, &g_rec.keyframe);
    g_rec.next_keyframe = now + DEMO_KEYFRAME_S;
    g_rec.keyframe_due = 0;
    server_log("[demo] K.P.D demo recorder on (connection %d)%s", g_rec.recorder_id, g_gs_config.demo_dir[0] ? ", demo_dir set" : "");
}

void demo_rec_tick(void) {
    if (!g_rec.started) {
        rec_start();
        return;
    }
    const double now = rec_now();
    rec_push(now, DEMO_REC_STREAM, &g_rec.tick);
    if (g_rec.keyframe_due || now >= g_rec.next_keyframe) rec_take_keyframe(now);
    for (int i = 0; i < DEMO_PENDING_MAX; i++) {
        if (g_rec.calls[i].used && now >= g_rec.calls[i].due) rec_finish(i);
    }
    rec_trim(now);
}

static void rec_note(const int conn_id, const int32_t uid, const char *name, const int account, const int call_type, const double after) {
    if (!g_rec.started) return;
    int slot = -1;
    for (int i = 0; i < DEMO_PENDING_MAX && slot < 0; i++) {
        if (!g_rec.calls[i].used) slot = i;
    }
    if (slot < 0) { // full: the oldest goes out now
        slot = 0;
        for (int i = 1; i < DEMO_PENDING_MAX; i++) {
            if (g_rec.calls[i].at < g_rec.calls[slot].at) slot = i;
        }
        rec_finish(slot);
    }
    RecCall *c = &g_rec.calls[slot];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->at = rec_now();
    c->due = c->at + (after > 0.0 ? after : 0.0);
    c->suspect_id = conn_id;
    c->suspect_uid = uid;
    c->suspect_account = account;
    c->call_type = call_type;
    snprintf(c->suspect_name, sizeof(c->suspect_name), "%s", name ? name : "");
    rec_token(c->token, sizeof(c->token));
}

void demo_rec_note_call(const ClientConnection *caller, const ClientConnection *suspect, const int call_type) {
    (void) caller;
    if (!suspect) return;
    rec_note(suspect->id, suspect->player ? suspect->player->uid : -1, suspect->name, suspect->acct.account_id, call_type, DEMO_AFTER_S);
}

int demo_rec_cut(const char *name, const double after_s) {
    if (!g_rec.started || !name || !name[0]) return 0;
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *c = &g_server->clients[i];
        if (c->recorder || strcasecmp(c->name, name) != 0) continue;
        rec_note(c->id, c->player ? c->player->uid : -1, c->name, c->acct.account_id, accounts_kpd_call_type(c), after_s);
        return 1;
    }
    for (size_t i = 0; i < g_server->game.player_count; i++) { // a bot: a player without a connection
        const Player *p = g_server->game.players[i];
        if (p && !strcasecmp(p->name, name)) {
            rec_note(-1 - p->uid, p->uid, p->name, 0, accounts_kpd_call_type(NULL), after_s);
            return 1;
        }
    }
    return 0;
}

void demo_rec_call_recorded(const int suspect_id, const int64_t call_id) {
    for (int i = 0; i < DEMO_PENDING_MAX; i++) {
        if (g_rec.calls[i].used && g_rec.calls[i].suspect_id == suspect_id && !g_rec.calls[i].call_id) g_rec.calls[i].call_id = call_id;
    }
}

void demo_rec_on_leave(const ClientConnection *client) {
    if (!g_rec.started || !client || client->recorder) return;
    for (int i = 0; i < DEMO_PENDING_MAX; i++) {
        if (g_rec.calls[i].used && g_rec.calls[i].suspect_id == client->id) rec_finish(i);
    }
}

void demo_rec_stop(void) {
    if (g_rec.started) {
        for (int i = 0; i < DEMO_PENDING_MAX; i++) rec_finish(i);
        for (size_t i = 0; i < g_server->client_count; i++) {
            if (g_server->clients[i].recorder) {
                net_remove_client(&g_server->clients[i]);
                break;
            }
        }
    }
    net_set_demo_send(NULL);
    for (size_t i = 0; i < g_rec.count; i++) free(g_rec.records[i].bytes);
    free(g_rec.records);
    demo_buf_free(&g_rec.tick);
    demo_buf_free(&g_rec.keyframe);
    const int written = g_rec.written;
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.recorder_id = -1;
    g_rec.written = written;
}

int demo_rec_recorder_id(void) {
    return g_rec.recorder_id;
}

int demo_rec_written(void) {
    return g_rec.written;
}
