# Rented servers (Krunker v10.0.7 "Rent Dedicated Server") on Box Office

Krunker lets a player rent a dedicated server by the month for KR. The server runs a community game
around the clock and is listed in the server browser with the blue **24/7** tag. This page is the spec:
first Krunker's behaviour, cited against `deob-10.0.7.js`, then how Box Office implements it.

## 1. Krunker v10.0.7

### Constants (`config`, deob-10.0.7.js:19647-19651)

| key | value | source |
| --- | --- | --- |
| `dediDurat` | `[30, 60, 90]` days | `iîíiïïì` :18707 |
| `dediPrices` | `[7500, 14000, 20000]` KR | `iïiîïïi` :18708 |
| `dediSlots` | `3` | :19649 (the client never reads it) |
| `dediTimer` | `600000` ms (10 min) | :19650 (the client never reads it; the window says a server can take up to 10 minutes to start) |
| `dedicatedOnlyRegions` | `us-wa, us-il, gb-lon, mx-cmx, as-seoul` | `iíïììïï` :16440. Matchmaking never pings these regions (:149813), so they host rentals only |

The rental regions are the 17 rows of `iíïííïî` (:18709-18777), each `{r: region, n: city, f: flag}`.
Index 3 (Miami) and index 15 (Cape Town) are `unlisted`. A server's `REG` field is an index into this
table:

| REG | city | REG | city | REG | city |
| --- | --- | --- | --- | --- | --- |
| 0 | New York | 6 | Mexico | 12 | Mumbai |
| 1 | Sillicon Valley [sic] | 7 | Sao Paolo [sic] | 13 | Frankfurt |
| 2 | Dallas | 8 | Manama | 14 | London |
| 3 | Miami (unlisted) | 9 | Tokyo | 15 | Cape Town (unlisted) |
| 4 | Seattle | 10 | Singapore | 16 | Sydney |
| 5 | Chicago | 11 | Seoul | | |

### The window

Rent Dedicated Server is `hostTypes[2]` (`{title, buyServ: true, noCustM: true}`, :16580-16584). It is
the Host Game window (`windows[7]`) with `buyServ` set. `gen` draws only the rental pages
(:219853-219938), with tab state in `windows[7].tabIndex` and `switchTab2(i)` (:219801-219805, which
resets the chosen game when it goes to tab 0):

- **Tabs** (`buyTabs`, :219806-219810): `Rent Server` and `My Servers`. On tabs 2 and 3 both are
  `bigMenTabDis` and a red `close` icon tab leads back to tab 0 (:219868-219873).
- **Tab 0, Rent Server**: one `serverCard` per listed region, with its flag, its city and
  "`N` Available", where N counts that region's servers with `TRM <= 0`. A click sets `servInd` to the
  region and opens tab 2 (:219874-219880).
- **Tab 1, My Servers**: one card for each server whose `OID` is the player's id and whose `TRM > 0`.
  Each card shows the game thumbnail (`/m<gid>/thumb.png`), the flag, `SVN_SID` and "Cloud Compute".
  A click opens tab 3 for that server. With no servers it reads "No available servers", and it reads
  "Loading..." until the list arrives (:219881-219899).
- **Tab 2, a region's servers**: one card per server in the region. A rented one (`TRM > 0`) is grey
  and does nothing on click; a free one opens tab 3 (:219900-219917).
- **Tab 3, the server** (:219918-219935): a header card, then the details: name `SVN_SID`, region,
  city, and type "Cloud Compute single-core". If `TRM > 0` it shows "Time remaining" formatted by
  `iííìïïï` (:23957, `Xy Xd Xh Xm`). Then come:
  - the duration select `#dediHostDur`, with options `30/60/90 Days`, and `onchange`
    `windows[7].dediCst(v)`, which sets `#dediCostI` to the formatted price (:219824-219826);
  - Select / Change Game, which runs `showGames(2)`;
  - the cost line `#dediCostI` KR;
  - the message line `#dediHostErr`;
  - `#rentServBtn`, which reads "Rent Server", or "Extend Rental" when the player owns the server and
    it has time left;
  - for the owner only, `#updDediServBtn` "Restart/Update", which runs `rentDediServer(ID, 1)`.

The labels switch between "Rental Duration" / "Extend Time" and "Select Game" / "Change Game" on the
same owner test. `showGames(2)` (:204449) opens the Games window with `hostType` 2. Its pick
(`selectHostMap(name, preset, id)`, :225334-225348) sets the chosen game `{id, name}` and returns to the
window.

Krunker's own v10.0.7 Host chooser (window 42, :222239) has no card for host type 2. The window is still
in the bundle and works, but its only entry point is `openHostWindow(false, 2)`. v7.1.4's chooser had
that card: "Dedicated", with the background `/img/host-bg/2.png` (Krunker_v7.1.4/src/index.js:9402).

### Requests (game socket)

- `dsrvl` (C2S, no args, :219866): the list, requested the first time the window is generated.
- `rdsrv` (C2S, `rentDediServer(id, update)`, :204642-204668): `[id, {typ, mid, gnm}, update]`.
  - `typ` is the `#dediHostDur` value (0..2).
  - `mid` / `gnm` are the chosen game's id and name. Without a chosen game they fall back to the
    server's current `CONF.gid` / `CONF.mn`, and only while `TRM > 0`.
  - With neither, the client refuses with "Please select a game to host".
  - Before sending, the client hides both buttons and shows "Please Wait..." in white.
- `dsrvl` (S2C, `DEDI_BUY_RESPONSE`, :98672; handler `iiiîïíî` :225183 -> `windows[7].dediRes`,
  :219811-219823):
  - `[0, servers]` replaces `dediServers.data` and redraws the window;
  - `[1, message, keepHidden]` shows the message in white and shows the buttons again unless
    `keepHidden` is set.

A server row is `{ID, REG, TRM, OID, SVN, SID, CONF}`:
- `TRM`: milliseconds left; `<= 0` means free.
- `OID`: the owner's account id.
- `CONF`: a JSON string `{gid, mn, typ}`, meaning game id, game name, and the last duration index.

### In the game

- **Server browser.** A lobby with `ds` gets the blue `24/7` tag, gets no `2XP` tag, and keeps its own
  `h` name without the mode alias. The custom-game carousel tags it 24/7 as well (:218466-218476,
  :218507).
- **Rewards.** `Game.isDedicated` (:190676) makes `hasHost()` true (:190925). The `giveSkin` /
  `dropSkin` rewards are off on such a lobby unless it `mimicPubs` (:194509-194518), as on every hosted
  game.

## 2. Box Office

The client talks to `kn_brain` only (rule: no DB in the client). The frames above go over the brain's
social socket with Krunker's labels and argument order.

### Pool, regions, prices

- **Prices and durations** are Krunker's: 30 / 60 / 90 days for 7,500 / 14,000 / 20,000 KR, charged from
  `players.player_funds` with the ledger row `kr_transactions` (source `dedi`, ref
  `<server id>:<typ>`).
- **The pool.** Krunker's fleet is its own. Box Office rents the slots in `brain.rentals.pool`, one entry
  per slot: `{"id", "host", "region", "name", "sid", "port", "public_host"}`. The brain keeps one
  `rented_servers` row per slot (an upsert at start, by id).
  - **Production:** three slots on kn-mnz (Ashburn, Virginia), listed as Krunker region 0 (New York, the
    nearest of Krunker's rental cities), named `Ashburn_1..3`, on ports 21024-21026.
  - **Why three:** `dediSlots` = 3 is the pool size per hosted region. Krunker's client never reads the
    value, so this is our reading, recorded here.
  - Regions with no Box Office slot show "0 Available", as a sold-out Krunker region does.
- **Per-account limit.** An account rents at most 3 servers at once. Box Office uses `dediSlots` for
  this too, so one player can't take the whole pool. This is a documented reading.

### `rdsrv` rules (services/brain/rentals.c)

Every refusal is `["dsrvl", 1, text, false]`. Every success is `["dsrvl", 1, text, true]` followed by
the fresh list `["dsrvl", 0, rows]`. The texts are Box Office's own.

| case | result |
| --- | --- |
| a guest | refused: sign in |
| unknown id, typ outside 0..2 | refused |
| free server, no game | refused: pick a game |
| free server, game given | **rent**: charge `dediPrices[typ]`, owner = player, expires = now + `dediDurat[typ]` days, `CONF = {gid, mn, cr, typ}`, generation + 1 |
| free server, 3 servers already rented | refused |
| own server (time left), no update flag | **extend**: charge, expires += days, game changed if a new one is given (a new game restarts it) |
| own server, update flag | **restart / update**: free of charge, the game may change, generation + 1 (the agent restarts it) |
| another player's server with time left | refused: already rented |
| not enough KR | refused, nothing written |
| the slot's host agent silent for 2 minutes | refused before charging: the server is offline |

- **Atomicity.** The checks and the charge share one transaction. The row is locked
  (`UPDATE ... SET id = id`), so two renters can't both take one slot.
- **Expiry.** A timer (`brain.rentals.check_ms`, default 30 s) frees every row whose `expires_s` has
  passed: owner and CONF are cleared and the generation goes up, so the agent stops the instance.
  Listing computes `TRM` from `expires_s`, so a lapsed server shows as free at once.
- **The startup guarantee (`dediTimer`).**
  - Box Office's reading of `dediTimer`: a server that never reports running within 10 minutes of a
    fresh rental is refunded in full, and the slot is freed.
  - The refund is a `dedi_refund` ledger row with the same ref. Krunker's window promises a server
    within 10 minutes of starting.
  - Extensions and updates never refund.

### Agent API (internal, bearer = `brain.internal_secret`)

- `GET /mm/internal/rentals?host=<host>` returns `{"now", "slots": [...]}`. Each slot is
  `{id, port, name, region, public_host, generation, run, map, creator, game_id}`. `run` is true while
  the row is rented.
- `POST /mm/internal/rentals/status`
  `{"host", "slots": [{"id", "generation", "state": "running"|"starting"|"stopped"|"failed"}]}` records
  `running_gen` and the agent's `agent_seen_s`. A rental whose generation is running counts as started
  for the refund rule.

### kn_rentd (server/src/rentd.c, rentd_core.c)

A small C11 supervisor on the game host, with no GL and no DB.

- **Polling.** It polls the agent API every `poll_s` (10 s).
- **Instances.** For every slot with `run` it keeps one `server_bin --headless --no-cli --cfg
  <state>/rent-<id>.cfg` running. The cfg is written from the slot: `hostname` = `name`, `port`,
  `region`, `node_id RENT<id>`, `maps` = the game (`published:<creator>/<name>` or a catalogue key),
  `dedicated 1`, the coordinator URL and secret.
- **Restarts.** A changed generation is a restart: SIGTERM, then SIGKILL after `stop_grace_s`. A crash
  restarts with backoff (1 s doubling to 60 s). A slot that is no longer `run` is stopped.
- **Resource caps.**
  - Each child gets `RLIMIT_AS` (`child_mem_mb`, default 1024), `RLIMIT_CORE` 0 and `nice` 5.
  - The systemd unit caps the whole group: `MemoryMax`, `CPUQuota`, `TasksMax`.
  - "Cloud Compute single-core" is one core per server.
- **Status.** It posts each slot's state after every pass.

server_bin's `dedicated 1` (server.cfg):
- adds `ds: true` and `h: <hostname>` to the lobby meta, so the browser shows the 24/7 row named
  `Ashburn_1`;
- turns off item drops and earned KR like any hosted game (Krunker's `isDedicated` reward rule);
- keeps the lobby public, never password-quick-joined, with Krunker's custom-game rules.

### Client (client/src/ui/rent_servers_ui.c)

- The window is host type 2's Host Game window, drawn in C with Krunker's element ids (`rentServBtn`,
  `updDediServBtn`, `dediHostDur`, `dediCostI`, `dediHostErr`) and the page's existing classes. The
  copy is Box Office's own.
- Handlers: `windows.7.switchTab2`, `windows.7.dediCst`, `rentDediServer`, and `showGames(2)` (the
  Games window, whose Host button picks the rental's game).
- **Entry point.** The Host chooser (window 42) gets v7.1.4's Dedicated card back. It is the only way
  into a window that 10.0.7 still ships. This difference from 10.0.7 is intentional and documented in
  the code.

### Deploy

- `deploy/ansible/roles/kn_rentd`: the binary from the release, `/etc/krunknative/rentd.cfg` (with the
  secret read like the game servers'), and `kn-rentd.service` with the caps.
- Ports 21024-21026 are added to kn-mnz's `kn_forwarded_ports`, divide's DNAT and `203.fw`, and kru's
  net-bridge / kn-rtc target range (`*_target_port_hi` 21026).
