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
// Gateway URL (first match wins): ?rtc=<url>, the kn-rtc <meta> knob (loader.js,
// deploy/pages/stage.py; "off" disables WebRTC), else rtc.json next to the page (`make rtc`).
// ?transport=rtc keeps only this backend; ?transport=ws|wt (or an explicit ?ws=) skips it.
//
// JS here is network glue only (AGENTS.md): it moves bytes and reports open/close.
#ifdef __EMSCRIPTEN__
#include <client.h>
#include <emscripten.h>

EM_JS(int, kn_rtc_wanted, (void), {
    try {
        const q = new URLSearchParams(location.search);
        const t = q.get('transport');
        if (t === 'ws' || t === 'wt' || (q.get('ws') && t !== 'rtc')) return 0; // an explicit ?ws= endpoint wants that WebSocket
        if (t !== 'rtc' && !q.get('rtc') && typeof Module !== 'undefined' && Module['krunkNativeRtcUrl'] === 'off') return 0; // kn-rtc meta
    } catch (e) {}
    return typeof RTCPeerConnection !== 'undefined' ? 1 : 0;
});

EM_JS(void, kn_rtc_open, (const char *target_c, int gen), {
    const target = UTF8ToString(target_c);
    (async () => {
        let pc = null;
        const live = () => Module.knRtc !== undefined && Module.knRtc.pc === pc;
        try {
            let url = null;
            try { url = new URLSearchParams(location.search).get('rtc'); } catch (e) {}
            if (!url && Module['krunkNativeRtcUrl'] && Module['krunkNativeRtcUrl'] !== 'off') url = Module['krunkNativeRtcUrl']; // kn-rtc meta (loader.js)
            if (!url) {
                const r = await fetch('rtc.json', {cache: 'no-store'});
                if (!r.ok) throw new Error('no WebRTC gateway (rtc.json ' + r.status + ')');
                url = (await r.json()).url;
            }
            if (target) url += (url.indexOf('?') >= 0 ? '&' : '?') + 'target=' + encodeURIComponent(target);
            console.log('[rtc] connecting to ' + url);
            pc = new RTCPeerConnection({iceServers: []});
            const dc = pc.createDataChannel('knet', {ordered: false, maxRetransmits: 0});
            dc.binaryType = 'arraybuffer';
            Module.knRtc = {pc: pc, dc: dc};
            dc.onopen = () => { if (live()) _kn_dg_on_open(gen); };
            dc.onclose = () => { if (live()) _kn_dg_on_close(gen); };
            dc.onmessage = (e) => {
                if (!live() || !(e.data instanceof ArrayBuffer)) return;
                const n = e.data.byteLength;
                const p = _malloc(n);
                HEAPU8.set(new Uint8Array(e.data), p);
                _kn_dg_on_datagram(gen, p, n);
                _free(p);
            };
            pc.onconnectionstatechange = () => {
                if (live() && (pc.connectionState === 'failed' || pc.connectionState === 'closed')) _kn_dg_on_close(gen);
            };
            await pc.setLocalDescription(await pc.createOffer());
            const res = await fetch(url, {method: 'POST', headers: {'Content-Type': 'application/sdp'}, body: pc.localDescription.sdp});
            const body = await res.text();
            if (!res.ok) throw new Error('gateway refused (' + res.status + ' ' + body.trim() + ')');
            if (!live()) return; // closed meanwhile
            await pc.setRemoteDescription({type: 'answer', sdp: body});
        } catch (e) {
            console.warn('[rtc] ' + (e && e.message ? e.message : e));
            if (pc && live()) { Module.knRtc = undefined; try { pc.close(); } catch (e2) {} }
            _kn_dg_on_close(gen);
        }
    })();
});

// Unreliable channel: a datagram that does not fit the send buffer is dropped (knet resends).
EM_JS(void, kn_rtc_send, (const uint8_t *data, int length), {
    const s = Module.knRtc;
    if (!s || s.dc.readyState !== 'open' || s.dc.bufferedAmount > (1 << 20)) return;
    try { s.dc.send(HEAPU8.slice(data, data + length)); } catch (e) {}
});

EM_JS(void, kn_rtc_close, (void), {
    const s = Module.knRtc;
    Module.knRtc = undefined;
    if (s) {
        try { s.dc.close(); } catch (e) {}
        try { s.pc.close(); } catch (e) {}
    }
});

static int rtc_wanted(void) { return kn_rtc_wanted(); }
static void rtc_open(const char *target, const int gen) { kn_rtc_open(target, gen); }
static void rtc_send(const uint8_t *data, const int length) { kn_rtc_send(data, length); }
static void rtc_close(void) { kn_rtc_close(); }

const NetDgramBackend net_rtc_backend = {"webrtc", rtc_wanted, rtc_open, rtc_send, rtc_close};
#endif
