# Regions (Krunker v10.0.3): the table, /mm/ping-list and the seek's routing

The brain's `GET /mm/ping-list` (and `/ping-list`) answers `{region: "host[:port]"}`. The
official v10 client only pings region ids of its own list, so its keys must be v10 ids.
KrunkNative's region codes (`SV`, `LA`, `CH`, `VA`, from the inventory's
`kn_gameserver_region`, `deploy/ansible/inventory/hosts.yml`) are not among them.

krunk.io has fewer regions than Krunker (two: Silicon Valley and New York, plus `local` on a
dev host). Three rules follow, each with its section below:

1. **The pickers list every Krunker region** (v10's own dropdown), not only the ones we host.
2. **The default region is the lowest ping**, the way v10's `getRegion` picks it.
3. **A seek in a region we do not host is routed to the nearest hosted node**, never refused.

## The table: `shared/src/krunker_regions.c`

One table for every reader (`shared/include/krunker_regions.h`): the client's pickers, labels
and ping race (`client/src/ui/region_names.c`, `servers_ui.c`, `krunker_lobbies.c`,
`client/src/net/region_ping.c`), the brain's `/mm/ping-list` keys and seek routing
(`services/brain/registry.c`). `shared/tests/krunker_regions_test.c` pins it against the bundle.

Source: the 10.0.3 bundle (`~/jails/kru/src/Krunker_v10.0.3/game.js`, decoded copy
`~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`; read only). v10.0.2 (`~/jails/kru/src/Krunker_v10.0.3/deob/v10.0.2/game.js`) has the same
values.

| what | deob-10.0.3.js | v10.js (10.0.2) |
| --- | --- | --- |
| `availableRegions` (production) | 19306 | 16443 (`_7292`) |
| `sss` pushed on the beta host only; `["local"]` on a dev host | 19307-19311 | 16443 |
| `dedicatedOnlyRegions` | 19313 | 16444 (`_7294`) |
| region -> continent (1 oc, 2 as, 3 eu, 4 na, 5 sa) | 19315-19344 | 16446-16469 (`_7296`, `_7297`) |
| display names | 21566-21596 | 18672-18701 (`_7517`) |
| the Default Region dropdown lists `availableRegions` with those names | - | 203283-203300 |
| `getRegion` (the ping race) | - | 147469-147507 |

The table, in order (the dropdown's order first; `KR_*` flags of `krunker_regions.h`):

| # | id | name | continent | city (lat, lon) | flags |
| --- | --- | --- | --- | --- | --- |
| 1 | `de-fra` | Frankfurt | eu | Frankfurt 50.11, 8.68 | available |
| 2 | `us-ca-sv` | Silicon Valley | na | Santa Clara 37.35, -121.95 | available |
| 3 | `au-syd` | Sydney | oc | Sydney -33.87, 151.21 | available |
| 4 | `jb-hnd` | Tokyo | as | Tokyo 35.68, 139.69 | available |
| 5 | `sgp` | Singapore | as | Singapore 1.35, 103.82 | available |
| 6 | `us-nj` | New York | na | New York 40.71, -74.01 | available |
| 7 | `as-mb` | Mumbai | as | Mumbai 19.08, 72.88 | available |
| 8 | `us-tx` | Dallas | na | Dallas 32.78, -96.80 | available |
| 9 | `brz` | Brazil | sa | Sao Paulo -23.55, -46.63 | available |
| 10 | `me-bhn` | Middle East | as | Manama 26.23, 50.59 | available |
| 11 | `sss` | EU Super Secret Servers | eu | (Frankfurt) | beta only |
| 12 | `us-wa` | Seattle | - | Seattle 47.61, -122.33 | dedicated only |
| 13 | `us-il` | Chicago | - | Chicago 41.88, -87.63 | dedicated only |
| 14 | `gb-lon` | London | eu | London 51.51, -0.13 | dedicated only |
| 15 | `mx-cmx` | Mexico | sa | Mexico City 19.43, -99.13 | dedicated only |
| 16 | `as-seoul` | South Korea | as | Seoul 37.57, 126.98 | dedicated only |
| 17 | `us-ca-la` | Los Angeles | - | Los Angeles 34.05, -118.24 | name only |
| 18 | `us-ga` | Atlanta | - | Atlanta 33.75, -84.39 | name only |
| 19 | `nl-ams` | Amsterdam | - | Amsterdam 52.37, 4.90 | name only |
| 20 | `fr-par` | Paris | - | Paris 48.86, 2.35 | name only |
| 21 | `us-fl` | Miami | na | Miami 25.76, -80.19 | name only |
| 22 | `blr` | India | as | Bangalore 12.97, 77.59 | name only |
| 23 | `af-ct` | South Africa | eu (v10's value) | Cape Town -33.92, 18.42 | name only |
| 24 | `eu-stm` | Sweden | eu | Stockholm 59.33, 18.07 | name only |
| 25 | `tor` | Toronto | na | Toronto 43.65, -79.38 | name only |
| 26 | `pol` | Poland | eu | Warsaw 52.23, 21.01 | name only |
| 27 | `us-hi` | Hawaii | na | Honolulu 21.31, -157.86 | name only |
| 28 | `iow` | Iowa | - | Des Moines 41.59, -93.62 | name only |
| 29 | `local` | Local | - | (no place) | local |

The coordinates are KrunkNative's (the region's city, for the great-circle rule below); the
continents are v10's, including `af-ct` under 3 (eu) and `mx-cmx` under 5 (sa). `krunker_build.h`'s
`KRUNKER_REGIONS` / `KRUNKER_DEDICATED_ONLY_REGIONS` are the same two lists (the test pins them equal).

**Pinged** (`krunker_region_pinged`): `getRegion` pings a `/ping-list` key when `availableRegions`
has it and `dedicatedOnlyRegions` does not: rows 1-11 and `local`. The dedicated-only and
name-only ids are never pinged, so the brain never lists a node under them.

**Ping URL** (`krunker_region_ping_url`, v10.js:147489-147491): a value starting `ip_`, `localhost`
or `127.0.0.1` keeps its port and is `http://` unless the port is 443; any other host is
`https://<host>/ping` without its port. A KrunkNative brain's `host:6767` stays `http://host:6767/ping`.

## 1. The pickers

v10's Default Region dropdown (`settings.local.region`) lists `availableRegions` (rows 1-10,
production) by name, in that order. The native client's pickers do the same, whatever the
Server Type:

- Settings > Localization > **Default Region** (Box Office seeks): `servers_ui_region_options`
  (`client/src/ui/servers_ui.c`) over `region_picker_ids` (`region_names.c`): rows 1-10 in v10's
  order, then `local` (the dev host's list), then any other id the brain's `/mm/ping-list` names.
  Before this, only the brain's keys were listed (Silicon Valley, New York, Local).
- Settings > Quickplay > **krunker.io Region** (official seeks): `KRUNKER_REGIONS`
  (`client/src/ui/seek_prefs.c`), the same ten.

A label is the v10 name alone (`region_picker_label`): `Silicon Valley`, `New York`,
`Frankfurt`. The KrunkNative places a region holds are never appended in brackets (user,
2026-10-22: the labels got too long). Picking a region we do not host is fine: see 3.

Test: `client/tests/region_names_test.c` (`test_picker`: with the production brain's list, with
no list, with a list naming an unlisted region).

## 2. The default region: lowest ping (v10 `getRegion`)

"Nearest" in Krunker is the lowest latency, not geography. `ClientManager.getRegion()`
(v10.js:147469-147507) fetches `/ping-list`, fires every eligible key's `/ping` at once, and the
first to answer is the region, cached (`setDefaultRegion`, `pingRegion7`); the Default Region row
shows that cache until the player picks one.

The native client does the same for each matchmaker (`client/include/region_ping.h`;
`servers_ui.c` `on_ping_list` / `on_region_race` for the brain, `krunker_lobbies.c` for
krunker.io): with no saved Default Region and no confirmed cache, the race over the brain's list
(the regions we have nodes in) decides, and the winner is stored in `pingRegion7`. A saved Default
Region the brain does not list (Frankfurt) is not an error: the race runs and the seek goes to its
winner, the nearest hosted region by ping, logged `default region de-fra is not in the matchmaker's
list: seeking in us-nj instead` (`seek_choice.c`; `server/tests/seek_unlisted_region_e2e.py`).
`server/tests/seek_region_e2e.py` B1-B4 run the race against stub ping hosts with set delays.

The Default Region row shows the region the seek is made in while nothing is saved
(`seek_region_shown`, `client/src/ui/seek_choice.c`): the confirmed race result, else the region in
use (`servers_ui_seek_region`), which is the list's first when every ping failed. Before this, a failed
race showed `Local` (a live run: the seek joined Silicon Valley, the row read Local).

## 3. Routing: a region with no node goes to the nearest hosted one

The official client may seek with any id of its own cache (a `pingRegion7` from krunker.io), an
older native build with a saved id, the native client with a saved Default Region when the ping
list did not come. Until this, such a seek found no node and got 503 `NoAvailableServers` (or
`NoServersMatchQuery`) whenever `brain.matchmaking.cross_region` was off.

Now `registry_pick` and `registry_pick_host_node` (`services/brain/registry.c`) first run
`registry_route_region`: when no node of the flavor pool serves the requested region
(`registry_region_hosted`: by its KN code or its v10 id), the request is replaced by the nearest
hosted v10 id, **whatever `cross_region` says**. Nearest (`krunker_region_nearest`, pinned by
`krunker_regions_test`):

1. **The client's pings.** The native client sends `?pings=` with its seek, `{v10 id: ms}` measured
   to the listed regions' ping hosts (the server browser's row pings, `region_ping_measure`;
   `< 0` = failed). The hosted region with the lowest measured ping wins.
2. **Else great circle** from the requested region's city to each hosted region's city (the table
   above). A hosted region without a place (`local`, an unknown code) comes after every placed one.
3. **A request without a place** (`local`, an id v10 does not know) and no pings: the first hosted
   region in registry order.

A hosted region is always itself. Nothing registered at all is the one case nothing can be routed
to: the pick reports `NoAvailableServers` as before. Hosting a custom game (`/mm/host-game`) routes
by great circle only (no pings travel with it).

The answer's `region` is the node's own code (`brain_seek_reply`): the client shows where it
landed (`servers_ui.c` sets its current region from it). A routed answer also carries
`requestedRegion`, the id the seek asked for. The brain logs `seek region=de-fra routed to us-nj`.

**`cross_region` semantics, unchanged:** it only decides whether a *hosted* region whose lobbies
have no room may fall back to any other region's lobby (`doc/backend.md` 6.2 step 3). Routing an
*unhosted* region is not a fallback: there was never a lobby to be full there.

Tests: `services/brain/tests/test_registry.c` (`seek_region_checks`: every id of the table
resolves with `cross_region` off; Europe / Middle East / Africa / Brazil land in New York, Asia-Pacific
and the US west in Silicon Valley; pings beat geography; the host target is routed the same way),
`server/tests/seek_cross_region_e2e.py` (the real brain with four edge nodes: `de-fra` -> CH1/VA1,
`au-syd` -> SV1/LA1, `?pings=` wins, every v10.0.3 id gets a 200).

## The /mm/ping-list keys

`ClientManager.getRegion()` skips any key the client does not know:

- `if (-1 == availableRegions.indexOf(r)) return 0;`
- `if (dedicatedOnlyRegions.indexOf(r) >= 0) return 0;`

With `SV`, `LA`, `CH` and `VA` as keys nothing was pinged, no region was chosen and the seek
never went out (the "Lost Connection" box). The brain lists each node under the pinged v10 id
nearest to its real location:

| inventory host | KN code | location | v10 id | distance | why |
| --- | --- | --- | --- | --- | --- |
| `com.clara` | `SV` | Santa Clara, CA | `us-ca-sv` | 0 km | the same place (Silicon Valley) |
| `com.angeles` | `LA` | Los Angeles, CA | `us-ca-sv` | ~500 km | `us-ca-la` is not pinged; Silicon Valley is next (us-tx is ~2,000 km) |
| `com.chitown` | `CH` | Chicago, IL | `us-nj` | ~1,150 km | `us-il` is dedicated-only; New York is the nearest pinged id (us-tx ~1,300 km) |
| `amph.ashburn` | `VA` | Ashburn, VA | `us-nj` | ~350 km | no Virginia id; New York is the nearest pinged id (us-ga, a name only, is ~850 km) |
| (dev / offline) | `local` | this machine | `local` | - | the non-production list is `["local"]` |

A node whose region is already a pinged v10 id (`de-fra`, `us-nj`, `local`, as the federation test
and the example config use) is listed under that id. Any other code is not listed
(`registry_v10_region` returns NULL, the node is skipped; it is still a node a seek can be routed
to). The mapping is in `services/brain/registry.c` (`k_v10_regions`) and
`net-bridge/publicRoutes.ts` (`V10_REGION_IDS`); keep them the same.

- **Two nodes share an id.** `us-ca-sv` is listed for `SV` and `LA`; `us-nj` for `CH` and
  `VA`. The object can hold one key per id and the client pings one host per region, so the
  first live node in registry order gives the host (or `brain.regions[].ping_host`). Both
  nodes still take seeks in that region.
- **Only the ping list changed.** The node keeps its KN code in `MmNode.region`, and the lobby
  tuples (`tuple[1]`), `/mm/regions` and the admin views still use it.
- **A seek or a host names a region by either id.** `region_eq` (`services/brain/registry.c`)
  matches a node by its KN code (`SV`) or by its v10 id (`us-ca-sv`).
- **`brain.regions[].ping_host`** is looked up by the v10 id first, then by the KN code, so an
  operator can point `us-nj` at a specific host.
- **Ping host.** Without a `ping_host` the host is the owning brain's public URL (`/ping` is
  served there), as before.

## Readers of the shape

- Brain: `registry_ping_list`, `registry_route_region` (`services/brain/registry.c`), `h_ping_list`,
  `seek_with_account` (`services/brain/public_routes.c`). Tests: `services/brain/tests/test_registry.c`
  (`ping_list_checks`: the keys are v10 ids, every node is under its id, KN codes are absent;
  `seek_region_checks`: each side finds its node with cross_region off, every v10 id is routed).
- net-bridge: `net-bridge/publicRoutes.ts` maps `KRUNK_NATIVE_REGION` the same way.
- Native client (`client/src/ui/region_names.c`, `servers_ui.c`): the pickers' values are the v10
  ids (the whole dropdown), labelled with the v10 name and the edge places in brackets. The seek,
  the host and the race use the id the list gave, and the seek carries `?pings=`. The server
  browser still groups lobby rows by their edge code (`SV`, `CH`, names from the edge table) and
  matches the default and current region by the id. A saved Default Region or cached region holding
  an edge code is read as its id. Tests: `client/tests/region_names_test.c`,
  `server/tests/seek_region_e2e.py` (B1-B4), `server/tests/seek_unlisted_region_e2e.py`.
- Launcher test stub: `launcher/tests/launcher_online_e2e.py` answers `us-nj`.
