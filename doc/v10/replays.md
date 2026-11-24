# K.P.D demos: recording, the replay viewer and replay voting

Krunker v10.0.7's replays belong to the K.P.D (Krunker Police Department): a K.P.D call on a suspect
leaves a server demo of that lobby, officers open it from the Demos window (or a `?demo=` link), watch
it in the in-game replay viewer and give a verdict ("hacker?" yes / no) that the case's votes count.
This document is the reference for Box Office's port. Everything said about Krunker cites
`deob-10.0.7.js:<line>`; where Krunker's behaviour lives on its servers (not in `game.js`) the Box
Office choice is named as such.

## 1. What Krunker does

### 1.1 The demo file

- A demo is a msgpack array `["demo", {v, meta, frames}]` (load, :201618-201627). `frames` is a list
  of `[t, packet]` pairs: `t` milliseconds from the demo's start, `packet` the msgpack of one socket
  message the server sent (type then arguments, :201628-201636). So a demo is a recording of the
  server's packet stream as a spectating client receives it, not a state dump.
- Keyframes are frames of the pseudo type `_kf` whose data is `{init, roster}`: the `init` packet's
  arguments and the roster (the `0` player-list packet) at that moment (:201681, :201793-201801). Playback
  starts at the first keyframe (`start`, :201846-201872) and every seek restarts from the last keyframe at
  or before the target (:201926-201950).
- Format versions: `v` < 3 roster rows were shorter; `load` pads them to the current player stride
  (:201638-201646). The stride is a client constant (`0x35` = 53 fields, :229709).
- `meta` carries `calls` (`[{token, suspectSid}]`), `suspects` (sids) and `suspect` (a name); the
  suspect is the call matching the opened call token, else the first of `suspects`, else the roster
  row whose name matches (`resolveSuspectSid`, :201666-201690).

### 1.2 Playback

- The player replaces the socket: `watchDemo` closes the live socket and marks `window.isReplay`
  (:229716-229722), then every frame is handed to the normal packet handlers (`apply`, :201803-201843).
  Control packets (error, dc, pi, pir, nwT, lkt, load, ready, cust, frs, end; :201566-201578) are
  never applied; `init` goes through `injectInit`, which waits for the world (round id and map) before
  playback continues (:201845-201862, 20 s at most). Damage packets (`4`) are only shown while the
  spectated player is the one that took them (`damageFilter`, :229710-229712).
- A 33 ms timer advances the playhead by elapsed time x speed and applies every frame up to it
  (`tick`, :201874-201905). Speeds cycle 0.5x, 1x, 2x, 4x (:229783-229787).
- Seek (`seek` / `_applySeek`, :201918-201963): clamp to the suspect segments, find the last keyframe,
  re-init only if its map differs, remove every player, re-add the keyframe's roster, then apply only
  the state-type frames between the keyframe and the target (`0`, `2`, `h`, `ts`, `7`, `t`, `lv`, `tm`,
  `pwup`, `ufl`, `uct`, `gte`, `bnk`; :201579-201593) plus the last three kill packets (`k`).
- Suspect segments (`computeSuspectSegments`, :201693-201737): the spans where the suspect is in the
  roster and alive (a roster or spawn frame with the suspect starts one, its death `3` pauses it, its
  leave `2` ends it); spans closer than 10 s merge. Playback skips the gaps (`tick` jumps to the next
  segment, :201881-201899) and the timeline paints the segments yellow on a grey track
  (:229747-229763).
- Identities are hidden at load (`hideIdentities`, :201752-201782): every roster name becomes
  `Player_<sid>`, account fields (level, clan, account id, badges) are cleared, chat lines take the
  `Player_<sid>` of their sender.

### 1.3 The viewer UI

- `#replayControls` (page markup, shown as flex once playback starts, :229744): play / pause, the
  timeline slider (drag pauses its updates, release seeks, :229775-229782), `mm:ss / mm:ss`, the speed
  button, "SUSPECT: Player_<sid>" (follows the suspect, :229769, :229788-229790), the hitbox toggle
  (box outlines around every other player, refreshed every 50 ms, yellow for the suspect, :229791-229803,
  :229651-229680), the resolve area and exit (reload, :229804-229806).
- While playing, the camera is put back on the suspect every 1.5 s if it drifted (:229810-229812).
- The resolve area (:229603-229660): a case still open shows its counts and a RESOLVE button, which
  turns into "HACKER?" YES / NO / cancel; a vote posts `kpd.vote(tok, ct, verdict)` and the answer's
  `yes`, `no`, `myv`, `banned`, `resolved` update the cached case; a closed case shows its status only;
  a failed vote shows the error for 2.5 s. Voting needs the demo opened from the list (a call token).
- KPD Vision (the officer's red suspect name and ESP boxes, :162371-162432, :216370) also works in a
  replay (`isReplay` stands in for `kpdData`).

### 1.4 The API (`gapi.kpd`, :27443-27482)

| call | request | used by |
|---|---|---|
| `listDemos({off, q})` | `GET /kpd/demos?off=&q=` | Demos window (policeRes 4, :223195-223224) |
| `demoInfo(tok, ct)` | `GET /kpd/demos/<tok>?ct=` | `openDemoLink` popup (:229818-229833) |
| `downloadDemo(tok)` | `GET /kpd/demos/<tok>/download` (raw bytes) | `watchDemo` (:229694-229702) |
| `vote(tok, ct, verdict)` | `POST /kpd/demos/<tok>/vote {ct, verdict}` | the resolve area |
| `reviewers()` | `GET /kpd/reviewers` | Reviewers window (:223225-223250) |

`copyDemoLink` copies `<origin>/?demo=<tok>&sus=<ct>` (:229835-229845); the page opens it at load
(:230167).

## 2. Box Office

### 2.1 Recording (server_bin, `server/src/demo_rec.c`)

Krunker's recorder runs on its game servers; Box Office's runs in server_bin:

- **A hidden spectator connection.** The recorder is a `ClientConnection` with its own transport
  (`demo`, fds from `NET_DEMO_FD_BASE`), joined as a spectator (no `Player`), so it receives exactly
  the packet stream a spectating client receives, byte for byte, through both send paths (batched
  `send_frames` and `net_send_packet`). It is never counted or shown: not in the max-players check,
  the server's player counts, the player list or chat peers, no join / leave lines, no pings, no idle
  kick.
- **Keyframes** every `DEMO_KEYFRAME_S` (15 s) and after every INIT: the join burst a fresh spectator
  would get at that moment (`net_send_demo_keyframe`: MAP_INFO, INIT and what follows it, the spectate
  ack, every player's SPAWN / loadout / info, leaders, round state), captured separately from the
  stream. The recorder keeps its id, so the keyframe and the stream after it agree.
- **A rolling window**: the stream since the oldest keyframe needed for `DEMO_BEFORE_S` (120 s) of
  history. A K.P.D call marks `{token = the call's ct, suspect uid}`; `DEMO_AFTER_S` (30 s) later (or
  when the suspect leaves, the map ends or the server stops) the window from the call's keyframe is
  written out as one demo. Calls on the same suspect inside one window share the demo
  (`meta.calls`). Box Office choices: Krunker's lengths are server-side.
- Size: at most `DEMO_MAX_BYTES` (3.5 MiB, under the client's 4 MiB response cap); a longer window
  starts at a later keyframe.
- Upload: the demo goes to kn_brain over the game server's own authenticated link as
  `kpd_demo_chunk` frames (base64 slices of 192 KiB) and a `kpd_demo_done` with the meta (the call
  id, suspect, map, mode, typ, duration). The brain writes `uploads.dir/kpd-demos/<tok>.kndemo` and
  the `kpd_demos` row (migration 0346). Without a brain, `--demo-dir <dir>` (or `gameserver.demo_dir`)
  writes the file there (tests, local servers).

### 2.2 The file (`shared/include/demo.h`)

```
"KNDEMO\0" u8 version (1) | u32 meta length | meta JSON (UTF-8)
records: u32 t_ms | u8 kind (0 stream, 1 keyframe) | u32 length | bytes
```

Little-endian. `bytes` are the framed packets exactly as the TCP protocol carries them (type, u16
length big-endian, payload). meta: `{v, created (epoch ms), map, mode, typ, server, duration,
calls: [{token, suspect}], suspects: [uid]}`. `demo_suspect_segments()` is Krunker's segment rule on
SPAWN (present and alive), KILL of the suspect (dead) and LEAVE (gone), with the 10 s merge.
`demo_anonymize()` is `hideIdentities`: every SPAWN name, chat sender and PLAYER_INFO becomes
`Player_<uid>` with no level, clan or badges.

### 2.3 Storage and API (kn_brain, `services/brain/v10_mods.c`)

The existing routes keep Krunker's shapes; `/download` is new:

- `GET /kpd/demos/:tok/download`: the file, for an officer; 404 for anyone else (as `demoInfo`).
- `GET /kpd/demos/:tok?ct=` and `POST /kpd/demos/:tok/vote {ct, verdict}` as before; a vote's
  counts close the case at the threshold the brain already applies.

### 2.4 Playback (client, `client/src/net/demo_play.c`)

The demo player is a transport like the offline sandbox (`client/src/net/offline.c`): it feeds the
recorded frames into `client_net_queue_packet` at the playhead, so the client's own handlers build
the game exactly as when it spectated live. Sends go nowhere. Playback starts at the first keyframe;
a seek on the same map clears the players and applies the keyframe without its INIT / MAP_INFO, then
the frames up to the target with transient packets (shots, projectiles, sounds, effects, kills but
the last three) left out; another map re-inits. 33 ms steps, speeds 0.5 / 1 / 2 / 4, the suspect
segments and the 1.5 s re-follow as Krunker. `client->replay` makes `client_net_send` a no-op; an INIT of
the demo (the first keyframe's, a seek to another map's, a round change in the stream) holds the
playhead until its map is built (`game.ready`, no held INIT, no map download), 20 s at most. Box Office
choices: the step is per frame instead of a 33 ms timer; a seek's fast-forward applies the STATEs of the
last second before the target only (the earlier ones are overwritten anyway); control packets (pings,
errors, redirects, RECON / POSITION of a player the recorder does not have) are never applied.
`client_bin --demo <file>[:<call token>]` plays a local file (no server); `KRUNKNATIVE_REPLAY_LOG=1`
prints the playhead every second.

### 2.5 UI (`client/src/ui/hud_replay.c`)

`#replayControls` of the page (ids as Krunker's), driven from C: play / pause, slider, time, speed,
suspect follow, hitboxes, the resolve area and exit (`location.reload()`; a native re-exec). `watchDemo`
downloads and plays; `openDemoLink` (a `?demo=<tok>&sus=<ct>` link, `client_bin --demo-link
<tok>[:<ct>]`) shows the case popup first. The viewer uses v10's own text: RESOLVE -> HACKER? YES / NO /
cancel, SUSPECT, Downloading Demo / Loading Replay, KPD Demo Review and the API error strings. Votes go to
`POST /kpd/demos/<tok>/vote` with the account's session (`?token=`) when signed in; the API, not a local
sign-in check, decides the answer. Demos and Reviewers use `GET /kpd/demos?off&q` and `/kpd/reviewers`
(the official account's gapi when an official game is active), including the grouped response shape.
While a replay plays, `hud_votekick_kpd_sus()` is the replay's suspect (isReplay stands in for kpdData: the
spectator cards, KPD Vision), and the live case's verdict keys / panel are off. The hitbox outlines
(`client/src/replay_hitbox.c`) are LineMaterial boxes after the world pass, rebuilt every frame.

## 3. Tests

- `demo_test` (shared): format round-trip, determinism, refusals, keyframe lookup, segments, anonymize.
- `server/tests/kpd_demo_e2e.py BUILD [MAP] [KEEP]`: server_bin records (recorder invisible, keyframes,
  the window, a leaving suspect); `services/brain/tests/kpd_test.py` (upload) and
  `v10_api_mods_test.py` (download, list, vote) on the brain.
- `tools/replay/capture.sh`: records a demo on Sandstorm and Kanji, plays each headless with
  `--demo` and screenshots the replay.
- uidiff `kpd/kpd-replay-bar`, `kpd/kpd-replay-vote`, `kpd/kpd-replay-case` (state.replayBar /
  replayCase, KN baselines); the Demos list is `kpd/kpd-officer-demos`.
- `client/tests/kpd_replay_vote_e2e.py BUILD`: a live kn_brain plus a recorded Sandstorm demo
  uploaded as a K.P.D case. A headless officer client signs in, opens it with `watchDemo`, leaves its
  game server for the replay, frees the pointer with Escape and clicks RESOLVE and YES on the bar. The
  test checks that the vote landed (`kpd_demo_votes`, the case counts, the Demos list), that the bar
  shows 1Y 0N, and that the speed, pause and reload-on-exit controls work.
- `tools/replay/rt0_identity.sh BASE_BIN NEW_BIN`: the rtMode 0 check of AGENTS.md. It takes
  pinned-camera captures on Sandstorm and Kanji, with the UI and world-only, at a base build and at
  the new one. It runs each build `RUNS` times (default 6) to mask the pixels that differ between
  runs of the same build (Kanji's billboard video, menu animations). It fails on any other differing
  pixel.

Known limits: a custom or published map the client lacks cannot be fetched during a replay (there is
no server to answer MAP_REQUEST); server-line chat text is not anonymised; the window lengths
(120 s before a call, 30 s after) are Box Office's.
