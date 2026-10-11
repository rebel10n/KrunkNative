# Game transport (UDP / WebTransport)

Game traffic runs over **knet**, a small UDP protocol in `shared/src/knet.c`
(`shared/include/knet.h`). It has no sockets: it is pure state machines, so the same code
runs in server_bin, the native client, the wasm client and `shared/tests/knet_test.c`.
The game protocol on top is unchanged. It still uses framed packets (1 byte type, 2 bytes
length, payload ≤ `NET_MAX_PAYLOAD`), and every `net_send_packet(fd, ...)` call site works
as before.

| Path | Transport | Code |
|---|---|---|
| native client ↔ server_bin | UDP (knet), TCP fallback | `client/src/net/net_udp.c`, `server/src/net_udp.c` |
| browser ↔ server_bin | WebTransport datagrams → `kn-wt` relay → UDP (knet), WebSocket fallback | `client/src/net/net_wt.c`, `net-bridge/wt/` |
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

The MTU is 1000 bytes, which fits a WebTransport datagram on any path QUIC accepts.

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

## Running

- Native: there is nothing to configure. `KRUNKNATIVE_NET_TRANSPORT=udp|tcp` on the
  client forces one transport. The default tries UDP, and falls back to TCP when there is
  no answer within 1.5 s.
- Web: `make wt` runs the relay on UDP `:8443` (`WT_ADDR=`) and writes
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
- Loss simulation (on the receive side, off by default) uses `KRUNKNATIVE_NET_DROP`,
  `_DUP` and `_REORDER` (percent), and works in server_bin and client_bin.
