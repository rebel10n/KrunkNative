// Web UDP transport, WebRTC backend: knet datagrams (net_wt.c owns the knet session) over one
// unordered, unreliable RTCDataChannel ({ordered: false, maxRetransmits: 0}: knet does its own
// acks and resends) to net-bridge/rtc, which relays every message 1:1 to the game server's
// UDP port (doc/transport.md). This is the preferred web transport: it needs no certificate
// on the media path and works through the Cloudflare-proxied signaling URL.
//
// Signaling: one WHIP-style POST of the SDP offer to the gateway (`?target=host:port`
// appended, as for the WebSocket bridge), the 201 body is the answer. No trickle ICE: the
// gateway is ICE-lite with a public host candidate in its answer, and the browser's own
// candidates are not needed (the gateway learns the browser's address from its checks).
//
// Opening has three budgets (measured: 0.3-0.4 s for the whole open from a nearby network,
// a few RTTs of ICE + DTLS + SCTP after the answer; the gateway logs its own timings):
//   RTC_SIGNAL_MS     the POST answered (Cloudflare -> tunnel -> gateway)
//   RTC_SILENT_MS     after the answer, at least one ICE check answered; none at all means UDP
//                     to the gateway's port is blocked on this network: fail fast
//   RTC_CONNECT_MS    after the answer, ICE + DTLS + SCTP + the channel open
// Every failure (and a session lost later) is printed to the browser console with the
// connection / ICE / DTLS / SCTP states, the candidate pairs, the timings and the event log,
// and reported to net_wt.c (kn_dg_on_fail) for the in-game console and the fallback.
//
// Gateway URL (first match wins): ?rtc=<url>, the kn-rtc <meta> knob (loader.js,
// deploy/pages/stage.py; "off" disables WebRTC), else rtc.json next to the page (`make rtc`).
// ?transport=rtc keeps only this backend; ?transport=ws|wt (or an explicit ?ws=) skips it.
//
// JS here is network glue only (AGENTS.md): it moves bytes and reports states. (EM_JS bodies
// become C string literals: no backslash escapes in them, hence [0-9] and fromCharCode(10).)
#ifdef __EMSCRIPTEN__
#include <client.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>

#define RTC_SIGNAL_MS 6000
#define RTC_SILENT_MS 3000
#define RTC_CONNECT_MS 6000

EM_JS(int, kn_rtc_wanted, (void), {
    try {
        const q = new URLSearchParams(location.search);
        const t = q.get('transport');
        if (t === 'ws' || t === 'wt' || (q.get('ws') && t !== 'rtc')) return 0; // an explicit ?ws= endpoint wants that WebSocket
        if (t !== 'rtc' && !q.get('rtc') && typeof Module !== 'undefined' && Module['krunkNativeRtcUrl'] === 'off') return 0; // kn-rtc meta
    } catch (e) {}
    return typeof RTCPeerConnection !== 'undefined' ? 1 : 0;
});

// Shared helpers on Module (installed once): the session's diagnostics.
EM_JS(void, kn_rtc_install, (void), {
    if (Module.knRtcDiag) return;
    // Tab closed / reloaded / navigated: the knet goodbye (net_wt.c kn_dg_bye) and the channel's
    // close go out now; a page that just vanishes looks like a lost network to the gateway
    // ("ICE consent lost" 15 s later) and leaves the player in the game server until KNET_TIMEOUT.
    addEventListener('pagehide', () => {
        const s = Module.knRtc;
        if (!s || !s.up || s.done) return;
        try { _kn_dg_bye(); } catch (e) {}
        try { if (s.dc) s.dc.close(); } catch (e) {}
    });
    const cand = (c) => c ? (c.candidateType || '?') + ' ' + (c.address || c.ip || '(hidden)') + ':' + c.port + '/' + (c.protocol || '?') : '-';
    // Candidate pairs and the selected path from getStats (resolves to a short text).
    Module.knRtcStats = async (s) => {
        if (!s.pc) return "";
        const st = await s.pc.getStats();
        const by = {};
        st.forEach((r) => { by[r.id] = r; });
        let sel = null, pairs = 0, req = 0, resp = 0, ok = 0, counted = false;
        st.forEach((r) => {
            if (r.type === 'transport' && r.selectedCandidatePairId) sel = by[r.selectedCandidatePairId];
            if (r.type === 'candidate-pair') {
                pairs++;
                if (r.requestsSent !== undefined) counted = true; // (Chrome, Safari; Firefox has no check counters)
                req += r.requestsSent || 0;
                resp += r.responsesReceived || 0;
                if (r.state === 'succeeded') ok++;
            }
        });
        s.checks = {pairs: pairs, req: req, resp: resp, ok: ok, counted: counted};
        let text = 'pairs ' + pairs + ' (' + ok + ' succeeded)' + (counted ? ', checks sent ' + req + ' answered ' + resp : "");
        if (sel) {
            s.path = cand(by[sel.localCandidateId]) + ' -> ' + cand(by[sel.remoteCandidateId]) +
                (sel.currentRoundTripTime !== undefined ? ', ICE rtt ' + Math.round(sel.currentRoundTripTime * 1000) + ' ms' : "");
            text += '; path ' + s.path;
        }
        return text;
    };
    Module.knRtcDiag = (s) => {
        const pc = s.pc;
        const sctp = pc && pc.sctp;
        const local = Object.keys(s.local).map((k) => k + ' x' + s.local[k]).join(', ') || 'none';
        return 'phase ' + s.phase + ' at ' + Math.round(performance.now() - s.t0) + ' ms' +
            (pc ? '; pc ' + pc.connectionState + ', ice ' + pc.iceConnectionState + ', gathering ' + pc.iceGatheringState +
                 ', dtls ' + (sctp && sctp.transport ? sctp.transport.state : '-') + ', sctp ' + (sctp ? sctp.state : '-') +
                 ', channel ' + (s.dc ? s.dc.readyState : '-') : "") +
            '; gateway ' + (s.remote || '-') + '; local candidates ' + local +
            (s.lastStats ? '; ' + s.lastStats : "") +
            '; timings ' + JSON.stringify(s.times) + '; events: ' + s.events.join(' | ');
    };
});

EM_JS(void, kn_rtc_open, (const char *target_c, int gen, int signal_ms, int silent_ms, int connect_ms), {
    const target = UTF8ToString(target_c);
    const t0 = performance.now();
    const at = () => Math.round(performance.now() - t0);
    const s = {pc: null, dc: null, gen: gen, t0: t0, phase: 'signaling', up: false, done: false, events: [], local: {}, times: {},
               timers: [], remote: "", path: "", lastStats: "", url: ""};
    Module.knRtc = s;
    const live = () => Module.knRtc === s;
    const ev = (m) => { s.events.push('+' + at() + 'ms ' + m); if (s.events.length > 40) s.events.splice(0, 1); };
    const note = (level, m) => { if (live()) _kn_dg_note(gen, level, stringToNewUTF8(m)); };
    // The end of this session (before or after it came up): diagnostics to the console, then
    // net_wt.c decides (fallback / reconnect). Stats are read before the connection is closed.
    const fail = async (reason) => {
        if (s.done) return;
        s.done = true;
        s.timers.forEach((t) => clearTimeout(t));
        ev('fail: ' + reason);
        try { s.lastStats = await Promise.race([Module.knRtcStats(s), new Promise((r) => setTimeout(() => r(s.lastStats), 400))]); } catch (e) {}
        console.warn('[rtc] ' + (s.up ? 'session lost' : 'could not open') + ': ' + reason + ' -- ' + Module.knRtcDiag(s));
        if (!live()) return; // the client closed it meanwhile
        Module.knRtc = undefined;
        try { if (s.dc) s.dc.close(); } catch (e) {}
        try { if (s.pc) s.pc.close(); } catch (e) {}
        _kn_dg_on_fail(gen, stringToNewUTF8(reason));
    };
    s.fail = fail;
    (async () => {
        try {
            let url = null;
            try { url = new URLSearchParams(location.search).get('rtc'); } catch (e) {}
            if (!url && Module['krunkNativeRtcUrl'] && Module['krunkNativeRtcUrl'] !== 'off') url = Module['krunkNativeRtcUrl']; // kn-rtc meta (loader.js)
            if (!url) {
                const r = await fetch('rtc.json', {cache: 'no-store'});
                if (!r.ok) throw new Error('no WebRTC gateway configured (rtc.json ' + r.status + ')');
                url = (await r.json()).url;
            }
            if (target) url += (url.indexOf('?') >= 0 ? '&' : '?') + 'target=' + encodeURIComponent(target);
            s.url = url;
            console.log('[rtc] connecting to ' + url);
            const pc = new RTCPeerConnection({iceServers: []});
            const dc = pc.createDataChannel('knet', {ordered: false, maxRetransmits: 0});
            s.pc = pc;
            s.dc = dc;
            dc.binaryType = 'arraybuffer';
            pc.onicecandidate = (e) => {
                if (!e.candidate) { ev('gathering complete'); return; }
                const c = e.candidate;
                const key = (c.type || '?') + '/' + (c.protocol || '?') + (/[.]local$/.test(c.address || "") ? ' (mdns)' : "");
                s.local[key] = (s.local[key] || 0) + 1;
            };
            pc.oniceconnectionstatechange = () => {
                const st = pc.iceConnectionState;
                ev('ice ' + st);
                if (st === 'connected' && s.times.ice === undefined) s.times.ice = at();
                if (!s.up || s.done) return;
                if (st === 'disconnected') {
                    console.warn('[rtc] ICE disconnected (no consent answer from the gateway for a few seconds); waiting for it to recover');
                    note(1, 'webrtc: ICE disconnected, waiting for it to recover');
                } else if (st === 'connected' || st === 'completed') {
                    note(0, 'webrtc: ICE recovered');
                } else if (st === 'failed') {
                    fail('ICE connection lost (the gateway stopped answering consent checks)');
                }
            };
            pc.onconnectionstatechange = () => {
                const st = pc.connectionState;
                ev('pc ' + st);
                if (st === 'connected' && s.times.dtls === undefined) s.times.dtls = at();
                if (st === 'failed') fail(s.up ? 'the peer connection failed' : 'the peer connection failed while opening');
            };
            dc.onopen = () => {
                if (!live() || s.done) return;
                s.up = true;
                s.phase = 'open';
                s.times.channel = at();
                s.timers.forEach((t) => clearTimeout(t));
                ev('channel open');
                const t = s.times;
                console.log('[rtc] open in ' + t.channel + ' ms (answer ' + t.answer + ' ms, ICE ' + t.ice + ' ms, DTLS ' + t.dtls + ' ms, channel ' + t.channel + ' ms after the start)');
                Module.knRtcStats(s).then((txt) => { s.lastStats = txt; if (s.path) console.log('[rtc] path ' + s.path); }).catch(() => {});
                _kn_dg_on_open(gen);
            };
            dc.onerror = (e) => { ev('channel error ' + (e && e.error ? e.error.message : "")); };
            dc.onclose = () => { ev('channel closed'); if (live()) fail(s.up ? 'the gateway closed the data channel' : 'the data channel closed while opening'); };
            dc.onmessage = (e) => {
                if (!live() || !(e.data instanceof ArrayBuffer)) return;
                const n = e.data.byteLength;
                const p = _malloc(n);
                HEAPU8.set(new Uint8Array(e.data), p);
                _kn_dg_on_datagram(gen, p, n);
                _free(p);
            };
            await pc.setLocalDescription(await pc.createOffer());
            const abort = new AbortController();
            s.timers.push(setTimeout(() => abort.abort(), signal_ms));
            let res, body;
            try {
                res = await fetch(url, {method: 'POST', headers: {'Content-Type': 'application/sdp'}, body: pc.localDescription.sdp, signal: abort.signal});
                body = await res.text();
            } catch (e) {
                throw new Error(abort.signal.aborted ? 'signaling: no answer from the gateway within ' + signal_ms + ' ms' : 'signaling: ' + (e && e.message ? e.message : e));
            }
            if (!res.ok) throw new Error('signaling: the gateway refused (HTTP ' + res.status + ' ' + body.trim() + ')');
            if (!live() || s.done) return; // closed meanwhile
            s.times.answer = at();
            s.phase = 'connecting';
            s.remote = (body.match(/a=candidate:[^ ]+ [0-9]+ ([^ ]+) [0-9]+ ([^ ]+) ([0-9]+) typ ([^ ]+)/g) || [])
                .map((l) => { const m = l.match(/a=candidate:[^ ]+ [0-9]+ ([^ ]+) [0-9]+ ([^ ]+) ([0-9]+) typ ([^ ]+)/); return m[4] + ' ' + m[2] + ':' + m[3] + '/' + m[1].toLowerCase(); })
                .filter((c, i, all) => all.indexOf(c) === i).join(', ');
            ev('answer (' + s.remote + ')');
            await pc.setRemoteDescription({type: 'answer', sdp: body});
            const sctp = pc.sctp;
            if (sctp) {
                sctp.onstatechange = () => ev('sctp ' + sctp.state);
                if (sctp.transport) sctp.transport.onstatechange = () => { ev('dtls ' + sctp.transport.state); if (sctp.transport.state === 'failed') fail('DTLS handshake failed'); };
            }
            // Watchdog: fail fast when no ICE check is ever answered (UDP blocked), else give
            // ICE + DTLS + SCTP their budget.
            const answered = performance.now();
            const watch = async () => {
                if (s.done || s.up || !live()) return;
                try { s.lastStats = await Module.knRtcStats(s); } catch (e) {}
                const since = performance.now() - answered;
                const c = s.checks || {resp: 0, req: 0, ok: 0, counted: false};
                // nothing answered (Chrome/Safari count the checks; Firefox: no pair succeeded)
                if (since >= silent_ms && c.ok === 0 && (!c.counted || c.resp === 0) && pc.iceConnectionState !== 'connected' &&
                    pc.iceConnectionState !== 'completed') {
                    fail('no ICE answer from the gateway (' + (s.remote || 'no candidate') + ') within ' + silent_ms + ' ms' +
                         (c.counted ? ' after ' + c.req + ' checks' : "") + ': UDP to it looks blocked on this network');
                    return;
                }
                if (since >= connect_ms) {
                    fail('not connected ' + connect_ms + ' ms after the answer (ice ' + pc.iceConnectionState + ', ' + (sctp && sctp.transport ? 'dtls ' + sctp.transport.state : 'no dtls') + ')');
                    return;
                }
                s.timers.push(setTimeout(watch, 500));
            };
            s.timers.push(setTimeout(watch, 500));
        } catch (e) {
            fail(e && e.message ? e.message : String(e));
        }
    })();
});

// Unreliable channel: a datagram that does not fit the send buffer is dropped (knet resends).
EM_JS(void, kn_rtc_send, (const uint8_t *data, int length), {
    const s = Module.knRtc;
    if (!s || !s.dc || s.dc.readyState !== 'open' || s.dc.bufferedAmount > (1 << 20)) return;
    try { s.dc.send(HEAPU8.slice(data, data + length)); } catch (e) {}
});

// net_wt.c ends the session (knet timeout, server switch, quit, its open guard): one console
// line when it was still opening, so a give-up is never silent.
EM_JS(void, kn_rtc_close, (void), {
    const s = Module.knRtc;
    Module.knRtc = undefined;
    if (!s) return;
    if (!s.done && !s.up) console.warn('[rtc] attempt abandoned by the client -- ' + Module.knRtcDiag(s));
    s.done = true;
    (s.timers || []).forEach((t) => clearTimeout(t));
    // The channel closes first and the connection a moment later: closing both at once dropped the
    // knet goodbye net_wt.c just sent and the SCTP stream reset, so the gateway saw only silence
    // ("ICE consent lost") and the game server kept the player until its knet timeout.
    const open = s.dc && s.dc.readyState === 'open';
    try { if (s.dc) s.dc.close(); } catch (e) {}
    if (open) setTimeout(() => { try { if (s.pc) s.pc.close(); } catch (e) {} }, 300);
    else { try { if (s.pc) s.pc.close(); } catch (e) {} }
});

// netinfo: the session's states, timings, path and recent events ("" without one).
EM_JS(char *, kn_rtc_describe, (void), {
    const s = Module.knRtc;
    if (!s) return stringToNewUTF8("");
    return stringToNewUTF8('webrtc: ' + Module.knRtcDiag(s) + String.fromCharCode(10));
});

static int rtc_wanted(void) { return kn_rtc_wanted(); }
static void rtc_open(const char *target, const int gen) {
    kn_rtc_install();
    kn_rtc_open(target, gen, RTC_SIGNAL_MS, RTC_SILENT_MS, RTC_CONNECT_MS);
}
static void rtc_send(const uint8_t *data, const int length) { kn_rtc_send(data, length); }
static void rtc_close(void) { kn_rtc_close(); }
static void rtc_describe(char *out, const size_t size) {
    char *text = kn_rtc_describe();
    snprintf(out, size, "%s", text ? text : "");
    free(text);
}

const NetDgramBackend net_rtc_backend = {"webrtc", rtc_wanted, rtc_open, rtc_send, rtc_close, rtc_describe};
#endif
