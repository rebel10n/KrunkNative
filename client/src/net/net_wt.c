// Web UDP transports: knet (shared/src/knet.c) over a browser datagram API, relayed 1:1 to
// the game server's UDP port (doc/transport.md). Two backends share the knet session below:
//   webrtc        unordered, unreliable DataChannel to net-bridge/rtc (net_rtc.c)
//   webtransport  WebTransport datagrams to net-bridge/wt (this file)
// net_web.c tries them in that order and falls back to its WebSocket path when a backend is
// not configured / not supported, or its session or knet handshake does not come up (a
// failed backend is skipped for the rest of the page's life, until a server switch).
// ?transport=ws (or an explicit ?ws=) skips both; ?transport=rtc / ?transport=wt keeps only
// that one (also with ?ws=, whose ?target= is reused; without one the gateway's default target).
//
// Everything runs on the main thread: the browser's datagram reader and a 10 ms interval
// timer (acks, resends, keepalive) call into C between frames.
#ifdef __EMSCRIPTEN__
#include <client.h>
#include <knet.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WT_FAKE_FD 1                 // client->net_socket while the session is up (like net_web.c)
#define WT_OPEN_TIMEOUT 6000.0       // ms for the browser session (ICE + DTLS / QUIC) to open
#define WT_HANDSHAKE_TIMEOUT 3000.0  // ms for the knet handshake once the session is open
#define WT_RETRY_MILLIS 2000

enum { WT_IDLE, WT_OPENING, WT_HANDSHAKE, WT_UP };

static const NetDgramBackend wt_backend;
// Preference order (net_web.c falls back to the WebSocket after the last one).
static const NetDgramBackend *const g_backends[] = {&net_rtc_backend, &wt_backend};
#define WT_BACKEND_COUNT ((int) (sizeof(g_backends) / sizeof(g_backends[0])))

static struct {
    int state;
    const NetDgramBackend *backend; // the session's (or the attempt's) transport
    int gen;                        // id of the current browser session (stale callbacks are ignored)
    int disabled[WT_BACKEND_COUNT]; // a failed attempt: skipped until the next server switch
    double opened_at;
    Client *client;
    uint32_t salt;
    double handshake_started, handshake_sent;
    uint8_t handshake_out[64];
    size_t handshake_length;
    long interval;
    int reconnect_pending;
    int closing;         // net_wt_close: the caller decides what comes next
    KnetConn conn;
    KnetSim sim;
} g_wt;

static double wt_now(void) { return emscripten_get_now() / 1000.0; }

// ------------------------------------------------------------------ browser side
EM_JS(int, kn_wt_wanted, (void), {
    try {
        const q = new URLSearchParams(location.search);
        const t = q.get('transport');
        if (t === 'ws' || t === 'rtc' || (q.get('ws') && t !== 'wt')) return 0; // an explicit ?ws= endpoint wants that WebSocket
        if (t !== 'wt' && typeof Module !== 'undefined' && Module['krunkNativeWtUrl'] === 'off') return 0; // kn-wt meta
    } catch (e) {}
    return typeof WebTransport !== 'undefined' ? 1 : 0;
});

EM_JS(void, kn_wt_open, (const char *target_c, int gen), {
    const target = UTF8ToString(target_c);
    (async () => {
        try {
            let url = null, hashes = [];
            try { url = new URLSearchParams(location.search).get('wt'); } catch (e) {}
            if (!url && Module['krunkNativeWtUrl'] && Module['krunkNativeWtUrl'] !== 'off') url = Module['krunkNativeWtUrl']; // kn-wt meta (loader.js)
            if (!url) {
                const r = await fetch('wt.json', {cache: 'no-store'});
                if (!r.ok) throw new Error('no WebTransport relay (wt.json ' + r.status + ')');
                const info = await r.json();
                url = info.url || ('https://' + location.hostname + ':' + info.port + (info.path || '/native'));
                if (info.certHash) {
                    hashes = [{algorithm: 'sha-256', value: Uint8Array.from(atob(info.certHash), c => c.charCodeAt(0))}];
                }
            }
            if (target) url += (url.indexOf('?') >= 0 ? '&' : '?') + 'target=' + encodeURIComponent(target);
            console.log('[wt] connecting to ' + url);
            const wt = new WebTransport(url, hashes.length ? {serverCertificateHashes: hashes} : {});
            Module.knWt = {wt: wt, writer: null};
            wt.closed.then(() => _kn_dg_on_close(gen), () => _kn_dg_on_close(gen));
            await wt.ready;
            if (Module.knWt === undefined || Module.knWt.wt !== wt) return; // closed meanwhile
            Module.knWt.writer = wt.datagrams.writable.getWriter();
            _kn_dg_on_open(gen);
            const reader = wt.datagrams.readable.getReader();
            for (;;) {
                const {value, done} = await reader.read();
                if (done) break;
                const p = _malloc(value.length);
                HEAPU8.set(value, p);
                _kn_dg_on_datagram(gen, p, value.length);
                _free(p);
            }
        } catch (e) {
            console.warn('[wt] ' + (e && e.message ? e.message : e));
            _kn_dg_on_close(gen);
        }
    })();
});

EM_JS(void, kn_wt_send, (const uint8_t *data, int length), {
    const w = Module.knWt && Module.knWt.writer;
    if (w) w.write(HEAPU8.slice(data, data + length)).catch(() => {});
});

// The page's ?ws= parameter ("" when absent), malloc'd.
EM_JS(char *, kn_page_ws_param, (void), {
    let ws = '';
    try { ws = new URLSearchParams(location.search).get('ws') || ''; } catch (e) {}
    return stringToNewUTF8(ws);
});

EM_JS(void, kn_wt_close, (void), {
    const s = Module.knWt;
    Module.knWt = undefined;
    if (s) { try { s.wt.close(); } catch (e) {} }
});

static int wt_wanted(void) { return kn_wt_wanted(); }
static void wt_open(const char *target, const int gen) { kn_wt_open(target, gen); }
static void wt_send(const uint8_t *data, const int length) { kn_wt_send(data, length); }
static void wt_close(void) { kn_wt_close(); }
static const NetDgramBackend wt_backend = {"webtransport", wt_wanted, wt_open, wt_send, wt_close};

// ------------------------------------------------------------------ knet glue
static void wt_emit(void *user, const uint8_t *data, const size_t length) {
    (void) user;
    if (g_wt.backend) g_wt.backend->send(data, (int) length);
}

static const char *wt_name(void) { return g_wt.backend ? g_wt.backend->name : "datagram transport"; }

static void wt_backend_close(void) {
    if (g_wt.backend) g_wt.backend->close();
}

static int wt_backend_index(const NetDgramBackend *backend) {
    for (int i = 0; i < WT_BACKEND_COUNT; i++) {
        if (g_backends[i] == backend) return i;
    }
    return -1;
}

static void wt_deliver(void *user, const uint8_t type, const uint8_t *payload, const uint16_t length) {
    Client *client = user;
    if (length > NET_MAX_PAYLOAD) return;
    NetPacket packet;
    packet.type = type;
    packet.length = length;
    if (length) memcpy(packet.payload, payload, length);
    client_net_log_packet("<=", packet.type, packet.payload, packet.length);
    if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) { // "pi" -> "po" at once
        client_net_log_packet("=>", NET_PACKET_PONG, packet.payload, packet.length);
        knet_conn_send(&g_wt.conn, NET_PACKET_PONG, packet.payload, packet.length, KNET_SEND_REDUNDANT);
        knet_conn_flush(&g_wt.conn, wt_now(), wt_emit, NULL);
        return;
    }
    client_net_queue_packet(client, &packet);
}

static void wt_reconnect_later(void *user) {
    Client *client = user;
    g_wt.reconnect_pending = 0;
    if (client->net_should_quit || client->offline || client->net_hold_retry) return;
    net_web_connect(client);
}

static void wt_stop_timer(void) {
    if (g_wt.interval) emscripten_clear_interval(g_wt.interval);
    g_wt.interval = 0;
}

// The session (or the attempt) is over. Before UP: the next backend, else the WebSocket path.
static void wt_finish(const char *why) {
    Client *client = g_wt.client;
    const int was_up = g_wt.state == WT_UP;
    const int was_attempt = g_wt.state != WT_IDLE && !was_up;
    if (g_wt.state == WT_IDLE) return;
    wt_stop_timer();
    if (was_up) {
        uint8_t bye[8];
        wt_emit(NULL, bye, knet_build_disconnect(bye, g_wt.conn.token));
        printf("%s session ended (%s; sent %llu / received %llu datagrams, %llu resent segments, rtt %d ms)\n", wt_name(), why,
               (unsigned long long) g_wt.conn.stats.datagrams_sent, (unsigned long long) g_wt.conn.stats.datagrams_received,
               (unsigned long long) g_wt.conn.stats.segments_resent, knet_conn_rtt_ms(&g_wt.conn));
        knet_conn_free(&g_wt.conn);
    }
    g_wt.state = WT_IDLE;
    wt_backend_close();
    if (!client || g_wt.closing) {
        if (was_up && client) {
            pthread_mutex_lock(&client->net_lock);
            client->net_socket = -1;
            pthread_mutex_unlock(&client->net_lock);
            on_client_socket_disconnect(client);
        }
        return;
    }

    if (was_up) {
        pthread_mutex_lock(&client->net_lock);
        client->net_socket = -1;
        pthread_mutex_unlock(&client->net_lock);
        on_client_socket_disconnect(client);
        if (!g_wt.reconnect_pending && !client->net_should_quit && !client->offline && !client->net_hold_retry) {
            g_wt.reconnect_pending = 1;
            emscripten_async_call(wt_reconnect_later, client, WT_RETRY_MILLIS);
        }
    } else if (was_attempt) {
        printf("%s unavailable (%s), trying the next transport\n", wt_name(), why);
        const int index = wt_backend_index(g_wt.backend);
        if (index >= 0) g_wt.disabled[index] = 1;
        if (!client->net_should_quit && !client->offline) net_web_connect(client); // next backend, else the WebSocket
    }
}

static void wt_tick(void *user) {
    (void) user;
    const double now = wt_now();
    if (g_wt.state == WT_OPENING) {
        if ((now - g_wt.opened_at) * 1000.0 > WT_OPEN_TIMEOUT) wt_finish("the session did not open in time");
        return;
    }
    if (g_wt.state == WT_HANDSHAKE) {
        if ((now - g_wt.handshake_started) * 1000.0 > WT_HANDSHAKE_TIMEOUT) {
            wt_finish("no knet handshake answer through the gateway");
            return;
        }
        if (now - g_wt.handshake_sent >= KNET_HANDSHAKE_RESEND) {
            wt_emit(NULL, g_wt.handshake_out, g_wt.handshake_length);
            g_wt.handshake_sent = now;
        }
        return;
    }
    if (g_wt.state != WT_UP) return;
    if (knet_conn_timed_out(&g_wt.conn, now)) {
        wt_finish("timed out");
        return;
    }
    if (g_wt.conn.failed) {
        wt_finish("transport overflow");
        return;
    }
    knet_conn_flush(&g_wt.conn, now, wt_emit, NULL);
}

static void wt_tick_interval(void *user) { wt_tick(user); }

// The browser session is open (DataChannel open / WebTransport ready): start the knet handshake.
EMSCRIPTEN_KEEPALIVE void kn_dg_on_open(const int gen) {
    if (gen != g_wt.gen || g_wt.state != WT_OPENING) return;
    g_wt.state = WT_HANDSHAKE;
    g_wt.salt = (uint32_t) (emscripten_random() * 4294967295.0) | 1u;
    g_wt.handshake_length = knet_build_connect_request(g_wt.handshake_out, g_wt.salt);
    g_wt.handshake_started = wt_now();
    g_wt.handshake_sent = -1.0;
    wt_tick(NULL);
}

static void wt_handshake_datagram(const uint8_t *data, const size_t length) {
    uint8_t out[64];
    const size_t answer = knet_client_answer_challenge(data, length, g_wt.salt, out);
    if (answer) {
        memcpy(g_wt.handshake_out, out, answer);
        g_wt.handshake_length = answer;
        wt_emit(NULL, out, answer);
        g_wt.handshake_sent = wt_now();
        return;
    }
    uint32_t token = 0;
    char reason[128] = "";
    const int accepted = knet_client_read_accept(data, length, g_wt.salt, &token, reason, sizeof(reason));
    if (accepted < 0) {
        char message[160];
        snprintf(message, sizeof(message), "Server refused the connection: %s", reason[0] ? reason : "denied");
        on_client_socket_failed(g_wt.client, message);
        g_wt.state = WT_IDLE; // a refusal is final for this attempt: no WebSocket retry
        wt_stop_timer();
        wt_backend_close();
        return;
    }
    if (accepted == 0) return;

    Client *client = g_wt.client;
    knet_conn_init(&g_wt.conn, token, wt_now());
    knet_sim_from_env(&g_wt.sim, ((uint64_t) token << 32) | g_wt.salt);
    g_wt.state = WT_UP;
    pthread_mutex_lock(&client->net_lock);
    client->net_socket = WT_FAKE_FD;
    pthread_mutex_unlock(&client->net_lock);
    printf("%s session established (token %08x)\n", wt_name(), token);
    on_client_socket_connect(client);
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR, 0}; // the minor gates the v7 packets (KrunkScript 200-203)
    client_net_send_packet(WT_FAKE_FD, NET_PACKET_HELLO, &hello, sizeof(hello));
    client_net_send_join(client, WT_FAKE_FD); // brain ticket / session / guest name (net_join.c)
}

static void wt_receive(void *user, const uint8_t *data, const size_t length) {
    (void) user;
    int closed = 0;
    knet_conn_receive(&g_wt.conn, data, length, wt_now(), wt_deliver, g_wt.client, &closed);
    if (closed) wt_finish("server closed the session");
}

EMSCRIPTEN_KEEPALIVE void kn_dg_on_datagram(const int gen, const uint8_t *data, const int length) {
    if (gen != g_wt.gen || length <= 0) return;
    if (g_wt.state == WT_HANDSHAKE) {
        wt_handshake_datagram(data, (size_t) length);
        return;
    }
    if (g_wt.state != WT_UP) return;
    knet_sim_filter(&g_wt.sim, data, (size_t) length, wt_receive, NULL);
    if (g_wt.state == WT_UP) knet_conn_flush(&g_wt.conn, wt_now(), wt_emit, NULL); // acks ride on whatever is due
}

EMSCRIPTEN_KEEPALIVE void kn_dg_on_close(const int gen) {
    if (gen != g_wt.gen) return;
    wt_finish(g_wt.state == WT_OPENING ? "could not open a session" : "the gateway closed the session");
}

// ------------------------------------------------------------------ net_web.c hooks
// The brain's /native?target=host:port pick, reused as the relay's target.
static void wt_target_from(const char *ws_url, char *out, const size_t size) {
    out[0] = 0;
    const char *t = ws_url ? strstr(ws_url, "target=") : NULL;
    if (!t) return;
    t += 7;
    size_t n = strcspn(t, "&#");
    if (n >= size) n = size - 1;
    memcpy(out, t, n);
    out[n] = 0;
    for (char *p = out; *p; p++) { // %3A -> ':' (the only escape a host:port needs)
        if (p[0] == '%' && p[1] == '3' && (p[2] == 'A' || p[2] == 'a')) {
            *p = ':';
            memmove(p + 1, p + 3, strlen(p + 3) + 1);
        }
    }
}

// 1 = a datagram session (WebRTC, else WebTransport) is being opened or is up; 0 = use the
// WebSocket.
int net_wt_connect(Client *client) {
    if (g_wt.state != WT_IDLE) return 1;
    const NetDgramBackend *backend = NULL;
    for (int i = 0; i < WT_BACKEND_COUNT && !backend; i++) {
        if (!g_wt.disabled[i] && g_backends[i]->wanted()) backend = g_backends[i];
    }
    if (!backend) return 0;
    g_wt.client = client;
    g_wt.backend = backend;
    g_wt.gen++;
    g_wt.state = WT_OPENING;
    g_wt.opened_at = wt_now();
    if (!g_wt.interval) g_wt.interval = emscripten_set_interval(wt_tick_interval, 10, NULL); // open timeout, then knet
    char target[128];
    wt_target_from(client->net_ws_url, target, sizeof(target));
    if (!target[0]) { // a pinned ?ws=...?target=host:port (no matchmaker URL): the same server
        char *page_ws = kn_page_ws_param();
        wt_target_from(page_ws, target, sizeof(target));
        free(page_ws);
    }
    printf("trying the %s transport\n", backend->name);
    backend->open(target, g_wt.gen);
    return 1;
}

// Debug / tests (tools/kref/wt-e2e.js, rtc-e2e.sh): the session's knet counters as JSON.
EMSCRIPTEN_KEEPALIVE const char *kn_wt_stats(void) {
    static char json[320];
    const KnetStats *st = &g_wt.conn.stats;
    snprintf(json, sizeof(json),
             "{\"transport\":\"%s\",\"up\":%d,\"rtt\":%d,\"sent\":%llu,\"received\":%llu,\"resent\":%llu,\"dups\":%llu,\"stale\":%llu}",
             g_wt.state == WT_UP ? wt_name() : "", g_wt.state == WT_UP, g_wt.state == WT_UP ? knet_conn_rtt_ms(&g_wt.conn) : -1,
             (unsigned long long) st->datagrams_sent,
             (unsigned long long) st->datagrams_received, (unsigned long long) st->segments_resent,
             (unsigned long long) st->duplicates_received, (unsigned long long) st->unreliable_dropped);
    return json;
}

int net_wt_active(void) { return g_wt.state == WT_UP; }
int net_wt_busy(void) { return g_wt.state != WT_IDLE; }

int net_wt_send(const NetPacketType type, const void *payload, const uint16_t length) {
    if (g_wt.state != WT_UP || length > NET_MAX_PAYLOAD) return 0;
    client_net_log_packet("=>", type, payload, length);
    const int ok = knet_conn_send(&g_wt.conn, (uint8_t) type, payload, length, net_packet_channel((uint8_t) type));
    if (ok) knet_conn_flush(&g_wt.conn, wt_now(), wt_emit, NULL);
    return ok;
}

// Close for good (quit) or to switch servers (reconnect = 1: open the next one at once).
void net_wt_close(const int reconnect) {
    if (g_wt.state == WT_IDLE) return;
    Client *client = g_wt.client;
    g_wt.closing = 1; // no WebSocket fallback / delayed retry from wt_finish
    wt_finish("closed by the client");
    g_wt.closing = 0;
    if (reconnect && client) {
        memset(g_wt.disabled, 0, sizeof(g_wt.disabled)); // the next server may be reachable over them
        net_web_connect(client);
    }
}
#endif
