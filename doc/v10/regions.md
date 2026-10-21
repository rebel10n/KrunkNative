# Regions in /mm/ping-list (Krunker v10.0.2)

The brain's `GET /mm/ping-list` (and `/ping-list`) answers `{region: "host[:port]"}`. The
official v10 client only pings region ids of its own list, so its keys must be v10 ids.
KrunkNative's region codes (`SV`, `LA`, `CH`, `VA`, from the inventory's
`kn_gameserver_region`, `deploy/ansible/inventory/hosts.yml`) are not among them.

## Why the keys changed

`ClientManager.getRegion()` (v10.js:147469-147507) fetches `/ping-list` and then pings
each key in turn, but first skips any key the client does not know:

- `if (-1 == availableRegions.indexOf(r)) return 0;` (the list is at v10.js:16443)
- `if (dedicatedOnlyRegions.indexOf(r) >= 0) return 0;` (v10.js:16444-16446)

With `SV`, `LA`, `CH` and `VA` as keys nothing was pinged, no region was chosen and the seek
never went out (the "Lost Connection" box). The brain now lists each node under the v10 id
nearest to its real location.

## The v10 ids the client accepts

Checked against `~/.cache/kn-v10/v10.js` (read only).

| list | ids | where |
| --- | --- | --- |
| `availableRegions`, krunker.io hosts | `de-fra`, `us-ca-sv`, `au-syd`, `jb-hnd`, `sgp`, `us-nj`, `as-mb`, `us-tx`, `brz`, `me-bhn`, `sss` | v10.js:16443 (`_7263`: the production host) |
| `availableRegions`, local / dev host | `local` | v10.js:16443 (the non-production branch) |
| `dedicatedOnlyRegions` (never pinged) | `us-wa`, `us-il`, `gb-lon`, `mx-cmx`, `as-seoul` | v10.js:16444 |
| names only (not pinged) | `us-ca-la`, `us-ga`, `us-fl`, `us-hi`, `tor`, ... | the region name table, v10.js:18672-18678 |

So `us-ca-la` and `us-il` look like real regions in v10 but the client never pings them. The
nearest ids it does ping are used below.

## The mapping

Locations come from the inventory comments and host names (`com.clara`, `com.angeles`,
`com.chitown`, `amph.ashburn`). Distances are great-circle, from the node's city to the
region's city.

| inventory host | KN code | location | v10 id | distance | why |
| --- | --- | --- | --- | --- | --- |
| `com.clara` | `SV` | Santa Clara, CA | `us-ca-sv` | 0 km | the same place (Silicon Valley) |
| `com.angeles` | `LA` | Los Angeles, CA | `us-ca-sv` | ~500 km | `us-ca-la` is not pinged; Silicon Valley is next (us-tx is ~2,000 km) |
| `com.chitown` | `CH` | Chicago, IL | `us-nj` | ~1,150 km | `us-il` is dedicated-only; New York is the nearest pinged id (us-tx ~1,300 km) |
| `amph.ashburn` | `VA` | Ashburn, VA | `us-nj` | ~350 km | no Virginia id; New York is the nearest pinged id (us-ga, a name only, is ~850 km) |
| (dev / offline) | `local` | this machine | `local` | - | the non-production list is `["local"]` |

A node whose region is already a v10 id (`de-fra`, `us-nj`, `local`, as the federation test and
the example config use) is listed under that id. Any other code is not listed
(`registry_v10_region` returns NULL, the node is skipped).
The table is in `services/brain/registry.c` (`k_v10_regions`) and `net-bridge/publicRoutes.ts`
(`V10_REGION_IDS`); keep them the same.

## What this means

- **Two nodes share an id.** `us-ca-sv` is listed for `SV` and `LA`; `us-nj` for `CH` and
  `VA`. The object can hold one key per id and the client pings one host per region, so the
  first live node in registry order gives the host (or `brain.regions[].ping_host`). Both
  nodes still take seeks in that region (see the next point).
- **Only the ping list changed.** The node keeps its KN code in `MmNode.region`, and the lobby
  tuples (`tuple[1]`), `/mm/regions`, host / seek filters and the admin views still use it.
- **Seeks still compare KN codes.** The v10 client seeks with `region` set to the v10 id it
  picked. The registry's region filter (`region_eq`, `services/brain/registry.c`) compares the
  node's KN code, so the seek finds no node in that region and falls back to any node only when
  `brain.matchmaking.cross_region` is on. It defaults to on (`services/brain/brain_util.c`), and
  the deploy template does not turn it off. Mapping the seek region is a separate change.
- **`brain.regions[].ping_host`** is looked up by the v10 id first, then by the KN code, so an
  operator can point `us-nj` at a specific host.
- **Ping host.** Without a `ping_host` the host is the owning brain's public URL (`/ping` is
  served there), as before.

## Readers of the shape

- Brain: `registry_ping_list` (`services/brain/registry.c`), `h_ping_list`
  (`services/brain/public_routes.c`). Test: `services/brain/tests/test_registry.c`
  (`ping_list_checks`): the keys are v10 ids, every node is under its id, KN codes are absent.
- net-bridge: `net-bridge/publicRoutes.ts` maps `KRUNK_NATIVE_REGION` the same way.
- Launcher test stub: `launcher/tests/launcher_online_e2e.py` answers `us-nj`.
- Native KrunkNative client (`client/src/ui/servers_ui.c`, `k_regions`: `SV`, `LA`, `CH`, `VA`,
  `local`; the `seek_region` tests): it still reads the list as KN codes, so with v10 keys its
  region picker shows the raw ids and it seeks in `us-nj` / `us-ca-sv`. Not changed here: the
  client is out of this change and needs its own decision.
