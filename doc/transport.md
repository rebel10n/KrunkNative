# Game transport (UDP / WebRTC / WebTransport)

Game traffic runs over **knet**, a small UDP protocol in `shared/src/knet.c`
(`shared/include/knet.h`). It has no sockets: it is pure state machines, so the same code
runs in server_bin, the native client, the wasm client and `shared/tests/knet_test.c`.
The game protocol on top is unchanged. It still uses framed packets (1 byte type, 2 bytes
length, payload ≤ `NET_MAX_PAYLOAD`), and every `net_send_packet(fd, ...)` call site works
as before.

| Path | Transport | Code |
|---|---|---|
| native client ↔ server_bin | UDP (knet), TCP fallback | `client/src/net/net_udp.c`, `server/src/net_udp.c` |
| browser ↔ server_bin | WebRTC DataChannel → `kn-rtc` gateway → UDP (knet), then WebTransport → `kn-wt` relay → UDP, then WebSocket fallback | `client/src/net/net_rtc.c`, `net_wt.c`, `net-bridge/rtc/`, `net-bridge/wt/` |
| legacy | TCP / WebSocket via net-bridge `/native` | `client/src/net/net.c`, `net_web.c` |

server_bin listens on TCP **and** UDP on the same port number. `KRUNKNATIVE_NET_UDP=0`
turns UDP off (TCP only).

## Wire format

Byte 0 is the datagram type. It is never `0xFF`, because datagrams that start with
`FF FF FF FF` are server-info queries (`shared/include/server_info.h`), which the server
answers on the game socket.

- Handshake (stateless cookie, anti-spoofing and anti-amplification)
  - `CONNECT_REQUEST` C→S: 64 bytes, padded, with magic `KNU`, version and a random salt.
  - `CHALLENGE` S→C: 13 bytes, salt plus cookie = SipHash(key, ip, port, salt, 10 s epoch).
    It is smaller than the request, and the server keeps no state yet.
  - `CONNECT_RESPONSE` C→S: salt, cookie and an optional forwarded address. The address
    is trusted only from loopback, so only the WT relay can set it.
  - `ACCEPT` S→C (salt, token) or `DENY` (salt, reason).
- `DATA` (both directions): `[0x06][token4][seq2][ack2][ack_bits4][ack_delay2]`, then messages:
  - `0x10` reliable: `[offset4][len2][bytes]`, one byte-stream segment.
  - `0x20` unreliable: `[flags1][id2][len2][type1][payload]`, one whole game frame.
- `DISCONNECT` (both directions): `[0x07][token4]`.

The MTU is 1000 bytes, which fits a WebTransport datagram on any path QUIC accepts, and a
DataChannel message in one SCTP chunk.

## Channels

`net_packet_channel()` in `shared/src/net_channel.c` picks the channel for each packet type:

- **Reliable-ordered** (all types not listed below): spawns, kills, chat, RECON, JOIN,
  inventory, map stream. The game frames are concatenated into one byte stream, which is
  cut into segments and resent until acked. Any message size works, so the map stream
  fragments for free.
- **Unreliable, sequenced** (`STATE`): every frame queued in one flush shares a
  generation. A frame from a generation older than the newest one delivered is dropped.
- **Redundant** (`INPUT`, `INPUT_BATCH`, `PING`, `PONG`): each frame is repeated in the
  next 2 flushes and de-duplicated by id. One lost datagram costs nothing, and the server
  also dedupes inputs by seq (`player_accept_input`).

Acks: every DATA carries the newest seq received plus a 32-bit bitfield. Lost segments
are resent when the RTO expires (srtt + 4·rttvar, 50 ms to 2 s, with backoff) or when a
datagram 3 seqs newer has been acked. RTT samples subtract the peer's `ack_delay`, so
the knet RTT on loopback is around 1 ms. The game ping (`PING`/`PONG`, HUD) still uses
the server's own measurement. The server stamps PONG arrival when the datagram is read,
before it takes `g_server->lock`, as net-ping requires.

Other limits: 10 s without a datagram is a timeout, a keepalive goes out after 0.5 s of
silence, at most 192 KB of reliable data is in flight, and a peer that leaves more than
8 MB unacked is dropped.

## Web transports

The wasm client runs the same knet session (`client/src/net/net_wt.c`) over a browser
datagram API, one backend at a time, in this order (`net_web_connect`):

1. **WebRTC** (`net_rtc.c`, gateway `net-bridge/rtc`, `kn-rtc`): one RTCDataChannel
   `knet` with `{ordered: false, maxRetransmits: 0}`, so every message is one datagram, lost or
   not, and knet does the acks and resends. Signaling is one WHIP-style request: the page
   POSTs its SDP offer (`Content-Type: application/sdp`) to `<gateway>?target=host:port` and
   the `201` body is the answer. There is no trickle ICE. The gateway is ICE-lite and its
   answer already carries its only candidate: a host candidate on one fixed UDP port (pion ICE
   UDP mux, every session shares it). The browser needs no candidates of its own in the offer,
   because its connectivity checks reach that port and the gateway learns its address as a
   peer-reflexive candidate. After ICE and DTLS, the gateway dials the target's UDP port and
   relays DataChannel messages 1:1, both ways. It holds no game state.
2. **WebTransport** (`net_wt.c`, relay `net-bridge/wt`, `kn-wt`): datagrams relayed 1:1 the
   same way. It needs a certificate for the relay and a direct UDP path.
3. **WebSocket** (`net_web.c`, net-bridge `/native`): the TCP byte stream, as before.

A backend that fails before the knet handshake completes is skipped for 20 s, then 40 s, 80 s
and so on up to 5 minutes. The count of consecutive failures resets once a session comes up.
After that pause the backend is tried again on the next connect: a reconnect, a server switch
or the page's retry link. A session that was up and drops is reopened over the same transport
first, after 2 s. WebRTC opening has three budgets (`net_rtc.c`):

- 6 s for the signaling POST.
- 3 s after the answer for at least one ICE check to be answered. If none is, UDP to the
  gateway is blocked on that network, and the client falls back without waiting for the rest.
  Chrome and Safari count the checks; for Firefox the test is that no candidate pair succeeded.
- 6 s after the answer for ICE + DTLS + SCTP + the channel.

Measured opens take 0.3-0.4 s in total from a nearby network, and 80-300 ms on loopback.
WebTransport keeps the 14 s outer guard of `net_wt.c`.

Diagnostics:

- Every attempt, failure reason and fallback is a transport note (`net_notes.c`). It is
  printed to the browser console (and stdout natively) and appears in the in-game console as
  `[net] ...`. The native UDP to TCP fallback is noted too.
- Every WebRTC failure, and a session lost later, also prints one browser-console line
  `[rtc] could not open: <reason> -- ...` or `[rtc] session lost: ...`. It carries the
  connection, ICE, DTLS, SCTP and channel states, the gateway candidates from the answer, the
  local candidate types, the candidate pairs and checks, the timings and the event log.
- A successful open prints `[rtc] open in N ms (answer, ICE, DTLS, channel)` and the selected
  path.
- The console command `netinfo` shows the transport, the server, the knet counters, the
  WebRTC states and path, and each backend's failures and pause.
- The menu ping has a tooltip, `Game transport: WebRTC|WebTransport|WebSocket|UDP|TCP`.
- The gateway logs each session with the browser family and version, the open timings
  (answer, ICE, DTLS, channel), and a specific close reason. The close reason says who ended
  the knet session (server or browser), or that ICE consent was lost (tab closed, sleep or
  network change). For a failed open it gives the ICE state and the number of checks that
  arrived: 0 means the browser's UDP never reached the port.

Not to be confused with the game transport: the matchmaker and social WebSocket
(`wss://coordinator.krunk.io/social/ws`) is always open next to it. A server kick also ends
the session; for example the v3.1.1 idle kick, "Kicked for inactivity", after 60 s without
moving, counted from the connect (or the last map change) in the menu; hosts and DEV peers
are exempt. Both browser and gateway log it as the server
ending the session.

The HUD and menu ping is the server's PING/PONG measurement over the active transport, so
over WebRTC it is the RTT through the gateway. The knet RTT of the session is in `netinfo`,
in `_kn_wt_stats()` (JSON, with the transport name) and in the note when the session ends.

Both gateways fill the knet `CONNECT_RESPONSE` forwarded-address field with the browser's
address. For kn-rtc this is the ICE peer's real UDP source. server_bin trusts that field only
from loopback. On a gateway that serves remote game servers, such as kn-rtc and net-bridge
on kru, the edges see the gateway's address.

**NAT, STUN and TURN.** The gateway has a public address, so neither is needed. The browser
sends its checks from behind any NAT, including symmetric NAT and CGNAT, and the gateway
answers the source address it saw (ICE-lite: it never sends checks to the browser's own
candidates). Only networks that block outbound UDP to that port fail, such as some corporate
or school firewalls. They fall back to the next transport after about 3 s (no ICE check
answered). A gateway behind a 1:1 NAT advertises its public address with `-public-ip`. A
gateway behind a port forward must forward the same port number.

Clients on kru's own network (egress 5.188.124.57, hairpinned by the Proxmox host to .56)
work over the public candidate: ICE consent holds and the conntrack entry stays fresh (UDP
stream timeout 120 s). They need no LAN candidate.

Browsers: Chrome and Chromium (desktop, headless), and Firefox 157, are tested against the
ICE-lite gateway with its single host candidate (opens in 0.1-0.4 s). Safari/WebKit is not
tested here. Its libwebrtc stack talks to ICE-lite servers the same way, and on a failure the
console line gives the states, not a silent fallback.

TURN over TLS/443 for UDP-blocked networks is not built. A TURN-over-TCP/TLS leg is TCP on the
lossy first hop, much like the WebSocket fallback through Cloudflare, so it would mainly help
networks that block only some UDP ports. The gateway's "0 checks received" log lines measure
how many users that is. If it is worth it, Cloudflare's TURN service ($0.05/GB to the client,
1,000 GB a month free, shared with their SFU) fits better than coturn on kru, whose port 443
belongs to the host nginx. The design: kn-rtc mints short-lived credentials (Cloudflare
`generate-ice-servers`), and the client retries WebRTC once with `iceTransportPolicy: "relay"`
after a "no ICE answer" failure, before the WebSocket.

**Phase 2** (not built): one kn-rtc per edge, next to its game servers. The DTLS path is then
browser → edge directly (one hop less than via kru), and the forwarded address comes from
loopback, so edge bans and logs see the browser's real IP. Each edge opens its ICE port in the
`edge_firewall` drop-in (`60-krunknative.nft`, `udp` list, outside abulxt's ports), and serves
signaling under its own name, for example `https://<edge>.krunk.io/rtc` through Cloudflare. The
matchmaker would return a per-server `rtcUrl` next to the target, and net_rtc.c would use it
instead of `kn-rtc`.

## Running

- Native: there is nothing to configure. `KRUNKNATIVE_NET_TRANSPORT=udp|tcp` on the
  client forces one transport. The default tries UDP, and falls back to TCP when there is
  no answer within 1.5 s.
- Web, WebRTC: `make rtc` runs the gateway, with signaling on `http://127.0.0.1:8085/rtc`
  (`RTC_HTTP=`) and ICE on UDP `:21100` (`RTC_ICE=`; a wildcard address binds every
  interface, a specific IP binds only that one). It writes `dist/web/rtc.json`
  (`{"url"}`), which the page fetches when its `kn-rtc` knob is empty. The deployed page sets
  `<meta name="kn-rtc">` (`ui/js/loader.js`, filled by `deploy/pages/stage.py` from
  `KN_RTC_URL`, default `https://play.krunk.io/rtc`: role `kn_rtc` on kru); `off` disables
  WebRTC. Page parameters:
  `?rtc=<url>`, `?transport=rtc|wt|ws` (one transport only). Targets are allowlisted exactly
  like net-bridge (`KRUNK_NATIVE_TARGETS`, `KRUNK_NATIVE_ALLOW_LOOPBACK`). Limits:
  `KRUNK_NATIVE_MAX_PER_IP`, `_MAX_SESSIONS`, `_RTC_IDLE_TIMEOUT_MS` (30 s with no datagram
  from the server), `_CONNECT_TIMEOUT_MS` (10 s for ICE, DTLS and the channel),
  `_CLIENT_IP_HEADER` (`cf-connecting-ip` behind cloudflared). CORS: `-origins` /
  `KRUNK_NATIVE_RTC_ORIGINS`. Deployment: `deploy/ansible` role `kn_rtc`.
- Web, WebTransport: `make wt` runs the relay on UDP `:8443` (`WT_ADDR=`) and writes
  `dist/web/wt.json` (`{"url","certHash"}`). The page fetches that file and uses
  `serverCertificateHashes` for the self-signed dev certificate. That certificate is
  valid for 13 days and is regenerated at every start. On edge hosts, pass
  `WT_CERT=`/`WT_KEY=` (a real certificate) and `-public-url`, and serve a `wt.json`
  with no `certHash`, or set the page's `<meta name="kn-wt">` knob (`ui/js/loader.js`,
  filled by `deploy/pages/stage.py`) to the relay URL; `kn-wt` = `off` disables WebTransport
  (static hosts such as Cloudflare Pages have no `wt.json`). Targets are allowlisted exactly like net-bridge
  (`KRUNK_NATIVE_TARGETS`). Page parameters: `?wt=<url>`, `?transport=ws|wt`.

## Testing

- `knet_test` (part of `make test`): handshake and cookie checks, timeouts, and sessions
  with drop, duplication, reordering and latency. It checks that every reliable byte
  arrives in order, that no stale STATE and no duplicate INPUT are delivered, and that
  RTT is plausible.
- `server/tests/udp_e2e.sh [BUILD]`: server_bin plus two headless clients fighting on
  the arena. `KN_UDP_LOSS="drop dup reorder"` sets the loss percentages,
  `KN_TEST_MAP=` streams a custom map, `KN_UDP_TRANSPORT=tcp` runs the same test over
  TCP. `make test-net` runs the clean and lossy variants.
- `tools/kref/wt-e2e.sh BUILD WEB_DIST`: server, relay, net-bridge and headless
  Chromium. It passes when the page connected over WebTransport.
- `tools/kref/rtc-e2e.sh BUILD WEB_DIST [seconds]`: server, kn-rtc, net-bridge and headless
  Chromium with `?transport=rtc`. It clicks to play, walks and shoots. It passes when the
  session came up over WebRTC and the page got in game, and it reports knet RTT
  (min/median/max), the HUD ping and the gateway's per-direction counters. `KN_RTC_DROP=15`
  makes the gateway drop that percentage in each direction. `KN_UDP_LOSS="0 5 5"` adds the
  server-side simulation, and `KN_TEST_MAP=` streams a custom map. Measured on loopback:
  knet RTT median 2 ms. With 15% drop each way plus 5% dup and 5% reorder, a 146 KB custom map
  streamed and the player got in game (RTT median 2 ms, max 6 ms). Against the deployed
  gateway (browser -> kru -> angeles-11 edge), knet RTT median was 86 ms and the HUD ping 85 ms.
  Over the WebSocket bridge on the same path, the HUD ping was about 100 ms.
- Loss simulation (on the receive side, off by default) uses `KRUNKNATIVE_NET_DROP`,
  `_DUP` and `_REORDER` (percent), and works in server_bin and client_bin.
