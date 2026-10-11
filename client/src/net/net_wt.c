// Web UDP transport: knet (shared/src/knet.c) over WebTransport datagrams, relayed 1:1 to
// the game server's UDP port by net-bridge/wt (doc/transport.md). net_web.c tries this
// first and falls back to its WebSocket path when the browser has no WebTransport, no
// relay is configured (wt.json next to the page, or ?wt=<url>), or the session / knet
// handshake does not come up. ?transport=ws (or an explicit ?ws=) skips it; ?transport=wt
// keeps it on with ?ws= (then the relay's default target is used).
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
#define WT_HANDSHAKE_TIMEOUT 3000.0  // ms for the knet handshake once WebTransport is ready
#define WT_RETRY_MILLIS 2000

enum { WT_IDLE, WT_OPENING, WT_HANDSHAKE, WT_UP };

static struct {
    int state;
    int disabled;        // a failed attempt: WebSocket for the rest of this page's life
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
        if (t === 'ws' || (q.get('ws') && t !== 'wt')) return 0; // an explicit ?ws= endpoint wants that WebSocket
        if (t !== 'wt' && typeof Module !== 'undefined' && Module['krunkNativeWtUrl'] === 'off') return 0; // kn-wt meta
    } catch (e) {}
    return typeof WebTransport !== 'undefined' ? 1 : 0;
});

EM_JS(void, kn_wt_open, (const char *target_c), {
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
            wt.closed.then(() => _kn_wt_on_close(), () => _kn_wt_on_close());
            await wt.ready;
            if (Module.knWt === undefined || Module.knWt.wt !== wt) return; // closed meanwhile
            Module.knWt.writer = wt.datagrams.writable.getWriter();
            _kn_wt_on_open();
            const reader = wt.datagrams.readable.getReader();
            for (;;) {
                const {value, done} = await reader.read();
                if (done) break;
                const p = _malloc(value.length);
                HEAPU8.set(value, p);
                _kn_wt_on_datagram(p, value.length);
                _free(p);
            }
        } catch (e) {
            console.warn('[wt] ' + (e && e.message ? e.message : e));
            _kn_wt_on_close();
        }
    })();
});

EM_JS(void, kn_wt_send, (const uint8_t *data, int length), {
    const w = Module.knWt && Module.knWt.writer;
    if (w) w.write(HEAPU8.slice(data, data + length)).catch(() => {});
});

EM_JS(void, kn_wt_close, (void), {
    const s = Module.knWt;
    Module.knWt = undefined;
    if (s) { try { s.wt.close(); } catch (e) {} }
});

// ------------------------------------------------------------------ knet glue
static void wt_emit(void *user, const uint8_t *data, const size_t length) {
    (void) user;
    kn_wt_send(data, (int) length);
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

// The session (or the attempt) is over. Before UP: fall back to the WebSocket path.
static void wt_finish(const char *why) {
    Client *client = g_wt.client;
    const int was_up = g_wt.state == WT_UP;
    const int was_attempt = g_wt.state != WT_IDLE && !was_up;
    if (g_wt.state == WT_IDLE) return;
    wt_stop_timer();
    if (was_up) {
        uint8_t bye[8];
        wt_emit(NULL, bye, knet_build_disconnect(bye, g_wt.conn.token));
        printf("webtransport session ended (%s; sent %llu / received %llu datagrams, %llu resent segments, rtt %d ms)\n", why,
               (unsigned long long) g_wt.conn.stats.datagrams_sent, (unsigned long long) g_wt.conn.stats.datagrams_received,
               (unsigned long long) g_wt.conn.stats.segments_resent, knet_conn_rtt_ms(&g_wt.conn));
        knet_conn_free(&g_wt.conn);
    }
    g_wt.state = WT_IDLE;
    kn_wt_close();
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
        printf("webtransport unavailable (%s), using the WebSocket transport\n", why);
        g_wt.disabled = 1;
        if (!client->net_should_quit && !client->offline) net_web_connect(client);
    }
}

static void wt_tick(void *user) {
    (void) user;
    const double now = wt_now();
    if (g_wt.state == WT_HANDSHAKE) {
        if ((now - g_wt.handshake_started) * 1000.0 > WT_HANDSHAKE_TIMEOUT) {
            wt_finish("no knet handshake answer through the relay");
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

EMSCRIPTEN_KEEPALIVE void kn_wt_on_open(void) {
    if (g_wt.state != WT_OPENING) return;
    g_wt.state = WT_HANDSHAKE;
    g_wt.salt = (uint32_t) (emscripten_random() * 4294967295.0) | 1u;
    g_wt.handshake_length = knet_build_connect_request(g_wt.handshake_out, g_wt.salt);
    g_wt.handshake_started = wt_now();
    g_wt.handshake_sent = -1.0;
    g_wt.interval = emscripten_set_interval(wt_tick_interval, 10, NULL);
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
        kn_wt_close();
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
    printf("webtransport session established (token %08x)\n", token);
    on_client_socket_connect(client);
    const NetHelloPacket hello = {1, 0, 0};
    client_net_send_packet(WT_FAKE_FD, NET_PACKET_HELLO, &hello, sizeof(hello));
    client_net_send_join(client, WT_FAKE_FD); // brain ticket / session / guest name (net_join.c)
}

static void wt_receive(void *user, const uint8_t *data, const size_t length) {
    (void) user;
    int closed = 0;
    knet_conn_receive(&g_wt.conn, data, length, wt_now(), wt_deliver, g_wt.client, &closed);
    if (closed) wt_finish("server closed the session");
}

EMSCRIPTEN_KEEPALIVE void kn_wt_on_datagram(const uint8_t *data, const int length) {
    if (length <= 0) return;
    if (g_wt.state == WT_HANDSHAKE) {
        wt_handshake_datagram(data, (size_t) length);
        return;
    }
    if (g_wt.state != WT_UP) return;
    knet_sim_filter(&g_wt.sim, data, (size_t) length, wt_receive, NULL);
    if (g_wt.state == WT_UP) knet_conn_flush(&g_wt.conn, wt_now(), wt_emit, NULL); // acks ride on whatever is due
}

EMSCRIPTEN_KEEPALIVE void kn_wt_on_close(void) {
    wt_finish(g_wt.state == WT_OPENING ? "could not open a WebTransport session" : "relay closed the session");
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

// 1 = a WebTransport session is being opened (or is up); 0 = use the WebSocket.
int net_wt_connect(Client *client) {
    if (g_wt.state != WT_IDLE) return 1;
    if (g_wt.disabled || !kn_wt_wanted()) return 0;
    g_wt.client = client;
    g_wt.state = WT_OPENING;
    char target[128];
    wt_target_from(client->net_ws_url, target, sizeof(target));
    kn_wt_open(target);
    return 1;
}

// Debug / tests (tools/kref/wt-e2e.js): the session's knet counters as JSON.
EMSCRIPTEN_KEEPALIVE const char *kn_wt_stats(void) {
    static char json[256];
    const KnetStats *st = &g_wt.conn.stats;
    snprintf(json, sizeof(json), "{\"up\":%d,\"rtt\":%d,\"sent\":%llu,\"received\":%llu,\"resent\":%llu,\"dups\":%llu,\"stale\":%llu}",
             g_wt.state == WT_UP, g_wt.state == WT_UP ? knet_conn_rtt_ms(&g_wt.conn) : -1, (unsigned long long) st->datagrams_sent,
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
        g_wt.disabled = 0; // the next server may have a relay
        net_web_connect(client);
    }
}
#endif
