// The web hot loader (doc/web-hot-loader.md): the first script in <head> of the game page, inlined
// there by platform/web/hot_loader.py when the web build is assembled (the native client never
// sees it: ui/index.html stays as it is, and the HTML engine ignores <script> anyway).
//
// Before the client (client.js / .wasm / .data) has even started downloading it
//   1. hints the browser at what comes next (preload of the client files, preconnect to the
//      matchmaker and the WebRTC gateway);
//   2. seeks: one GET /mm/hot-seek with the player's saved Quickplay preferences (the knSettings
//      keys the C client persists: seek_modes, seek_maps, defaultRegion; pingRegion7), the whole
//      find done by kn_brain;
//   3. opens the game transport to the answer's server: the WebRTC DataChannel to the gateway
//      (signaling, ICE, DTLS, SCTP: the slow part of a connect). The game server sees nothing
//      until the client's knet handshake, so no join deadline runs while the wasm downloads.
// window.__knHot holds the answer and the open channel. The client adopts both when it runs:
// servers_ui.c's startup seek takes the answer instead of seeking (net_hot.c), net_rtc.c takes
// the channel instead of opening one. Anything unusual (an offline game, a pinned server, no
// gateway, a refused seek) leaves __knHot without an answer or a channel, and the client does
// what it always did. Network glue only: no UI here (AGENTS.md).
(function () {
  var L = window.location;
  if (L.protocol !== 'http:' && L.protocol !== 'https:') return;
  var now = function () { return window.performance && performance.now ? performance.now() : Date.now(); };
  var q;
  try { q = new URLSearchParams(L.search); } catch (e) { return; }
  // not a seek into a Box Office lobby: offline / sandbox / test maps, a pinned server or transport,
  // a map to play, a demo, the editor (each of these is the client's own flow); ?nohot: off (measurements)
  var skip = ['nohot', 'offline', 'sandbox', 'test', 'map', 'testMap', 'play', 'ws', 'transport', 'demo', 'editor', 'viewer', 'host'];
  for (var i = 0; i < skip.length; i++) if (q.has(skip[i])) return;
  if (q.get('mode') === 'offline') return;
  var meta = function (n) {
    var t = document.querySelector('meta[name="' + n + '"]');
    return t ? t.getAttribute('content') || '' : '';
  };
  if (meta('kn-flavor')) return; // Krunker Strike seeks through its director
  var origin = function (u) { try { return new URL(u, L.href).origin; } catch (e) { return ''; } };
  var hint = function (rel, href, as) {
    if (!href) return;
    var l = document.createElement('link');
    l.rel = rel;
    l.href = href;
    if (as) l.as = as;
    if (as === 'fetch' || rel === 'preconnect') l.crossOrigin = 'anonymous';
    document.head.appendChild(l);
  };
  var H = window.__knHot = {t0: now(), times: {}, status: 0, body: '', game: q.get('game') || '', used: false, rtc: null};
  var mark = function (k) { H.times[k] = Math.round(now() - H.t0); };

  // 1. the client files (loader.js names them the same way: kn-build, kn-root)
  var build = meta('kn-build'), root = meta('kn-root');
  var file = function (ext) { return root + (build ? 'client.' + build + '.' + ext : 'client.' + ext); };
  hint('preload', file('js'), 'script');
  hint('preload', file('wasm'), 'fetch');
  hint('preload', file('data'), 'fetch');
  var brain = (meta('kn-brain') || L.origin).replace(/\/+$/, '');
  var rtcUrl = q.get('rtc') || meta('kn-rtc');
  if (origin(brain) !== L.origin) hint('preconnect', origin(brain));
  if (rtcUrl && rtcUrl !== 'off' && origin(rtcUrl) !== L.origin) hint('preconnect', origin(rtcUrl));

  // 2. the seek: the saved preferences, as the C client reads them (ui_settings.c knSettings,
  //    seek_prefs.c, servers_ui.c's region choice)
  var settings = {};
  var ls = function (k) { try { return window.localStorage.getItem(k) || ''; } catch (e) { return ''; } };
  try { settings = JSON.parse(ls('knSettings') || '{}') || {}; } catch (e) { settings = {}; }
  var list = function (v) { return Array.isArray(v) ? v.join(',') : typeof v === 'string' || typeof v === 'number' ? String(v) : ''; };
  var region = typeof settings.defaultRegion === 'string' ? settings.defaultRegion : '';
  var cached = ls('pingRegion7');
  if (!region && cached && cached === ls('knPingRegionPinged')) region = cached; // a ping race's winner only
  var p = {hostname: 'krunknative', proto: '@KN_PROTOCOL@', ruleset: 'v10', region: region,
           seek_modes: list(settings.seek_modes), seek_maps: list(settings.seek_maps), game: H.game,
           autoChangeGame: H.game ? 'false' : ''}; // a ?game= that is gone: refused (the client shows why), never another lobby
  var qs = [];
  for (var k in p) if (p[k]) qs.push(k + '=' + encodeURIComponent(p[k]));
  if (typeof fetch !== 'function') return;
  H.seek = fetch(brain + '/mm/hot-seek?' + qs.join('&'), {credentials: 'omit', cache: 'no-store'})
    .then(function (r) {
      return r.text().then(function (t) { H.status = r.status; H.body = t; mark('answer'); });
    })
    .catch(function (e) { H.status = -1; H.body = String(e && e.message || e); mark('answer'); })
    .then(function () {
      var a = null;
      try { a = H.status === 200 ? JSON.parse(H.body) : null; } catch (e) { a = null; }
      if (a && a.host && a.port) openRtc(a.host + ':' + a.port);
      return H;
    });

  // 3. the WebRTC DataChannel, as net_rtc.c opens it (one WHIP-style POST, no trickle ICE), held for
  //    the client; messages that arrive before it (none: the server waits for the knet handshake)
  //    are kept in order
  function openRtc(target) {
    // ("" = the dev gateway's rtc.json next to the page, as net_rtc.c)
    if (rtcUrl === 'off' || typeof RTCPeerConnection === 'undefined') return;
    var s = H.rtc = {target: target, pc: null, dc: null, open: false, failed: '', msgs: [], adopted: false, url: '', peer: '', remote: ''};
    var fail = function (why) {
      if (s.adopted || s.failed) return;
      s.failed = why;
      try { if (s.dc) s.dc.close(); } catch (e) {}
      try { if (s.pc) s.pc.close(); } catch (e) {}
      mark('rtcFailed');
    };
    (rtcUrl ? Promise.resolve(rtcUrl) : fetch('rtc.json', {cache: 'no-store'}).then(function (r) {
      if (!r.ok) throw new Error('no WebRTC gateway configured (rtc.json ' + r.status + ')');
      return r.json().then(function (j) { return j.url; });
    })).then(function (url) {
      url += (url.indexOf('?') >= 0 ? '&' : '?') + 'target=' + encodeURIComponent(target);
      s.url = url;
      var pc = s.pc = new RTCPeerConnection({iceServers: []});
      var dc = s.dc = pc.createDataChannel('knet', {ordered: false, maxRetransmits: 0});
      dc.binaryType = 'arraybuffer';
      dc.onopen = function () { s.open = true; mark('rtcOpen'); };
      dc.onmessage = function (e) { if (!s.adopted) s.msgs.push(e.data); };
      dc.onclose = function () { if (!s.adopted) fail('the data channel closed before the client took it'); };
      pc.onconnectionstatechange = function () { if (pc.connectionState === 'failed') fail('the peer connection failed'); };
      return pc.createOffer().then(function (o) { return pc.setLocalDescription(o); }).then(function () {
        return fetch(url, {method: 'POST', headers: {'Content-Type': 'application/sdp'}, body: pc.localDescription.sdp});
      }).then(function (res) {
        s.peer = res.headers.get('X-KN-RTC-Peer') || '';
        return res.text().then(function (body) {
          if (!res.ok) throw new Error('signaling: the gateway refused (HTTP ' + res.status + ')');
          mark('rtcAnswer');
          s.remote = body;
          return pc.setRemoteDescription({type: 'answer', sdp: body});
        });
      });
    }).catch(function (e) { fail(String(e && e.message || e)); });
    // a channel nobody took (the client failed to start, or seeked elsewhere) is not kept open
    setTimeout(function () { if (!s.adopted) fail('the client did not take the channel within 90 s'); }, 90000);
  }
})();
