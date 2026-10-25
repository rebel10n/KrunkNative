# Live events (Krunker v10.0.3 config.events)

What Krunker v10.0.3 does with its live events, and how Krunker Box Office plays them. Line numbers are the decoded
bundle's, `deob-10.0.3.js:<line>` (`~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`). The kill pools of the same
events (`ongoingEvents`) are doc/v10/kill-pools.md.

## 1. The list and its fields

`config.events` (:22364, exported at :22553) is a list bundled with the game; 10.0.3 ships it empty. Its readers name
the fields an event has:

| field | read by | KrunkNative |
|---|---|---|
| `id` | updateEventIndex (the round's `eventIndex`), `event_<id>` images, `DAT.evt` challenges | same |
| `name` | the popup's title, the Event Challenges cards and category | same (escaped) |
| `start`, `end` | updateEventIndex and hasActiveEvents: now in [start, end] (:219737-219745, :28747-28757) | the feed's `startsAt` / `endsAt` (v10's names are read too); no `endsAt`: open-ended |
| `reveal` (ms) | getActiveEvent (:28758-28775): an event starting within `reveal` that has not ended | same |
| `imgExt` | `img/events/event_<id>.<imgExt \|\| "png">` (:250877, :260291) | letters and digits only |
| `longDesc` | the popup's text (:260317), written as markup | text: a string, or a list of lines (the popup's `<li>` items); the feed carries no markup |
| `noChallenges` | the popup: "playing the event", no View Challenges button | same |
| `rewards.skins` | the popup's 12 tiles (:260296-260307) | same; also reward candidates (section 4) |
| `rewards.weights` | endGame's rarity weights (getItemByWeights, :220754-220806; else `store.lootRars`) | same |
| `rewardChance` | endGame's `randInt(1, 100) <= (rewardChance \|\| 100)` (:221740) | same |
| `eligibleMaps` | the map indexes the reward pays on (:221737) | same |
| `limitedTimeMode` | genMatchList's first vote pick (:221830-221832) | same |
| `eventObjects` | (not in v10) | KN: the EVENT prefab shows during this event too (section 2) |

KrunkNative's events are kn_brain's community feed (`brain.community.feed`, `"gameEvents"`; example in
`config/community.example.json`), one list with the kill pools' fields (`services/brain/community.c`). The game servers
get the running ones in the internal WS `welcome` and `game-events` pushes; the clients read `GET /data/events`
(`{"data":{"events":[...]}}`: running and early-revealed events, no kill pools). `shared/src/live_events.c`
(`shared/include/live_events.h`) parses them and ports the readers; tests: `live_events_test`, `config_test`
(genMatchList), `services/brain/tests/live_events_test.py` (`brain_live_events`), `client/tests/live_events_e2e.py`
(`make live-events-e2e`), `server/tests/kill_pools_server_e2e.py` (the reward).

## 2. The round's event and the EVENT prefab

- **updateEventIndex** (game.init, :219733-219751): 0, and with no host, not custom / official / comp, the `id` of the
  first event running now; then `forceEvent` wins (cleared after one round). Sent as init [2]. KN:
  `server/src/live_events_server.c` `server_events_round_index` at every round (`accounts_round_changed`), carried by
  `NET_EXT_INIT_EVENT` (also re-sent when a peer's HELLO comes after its INIT: the offline sandbox). `forceEvent` is
  `gameserver.force_event` (every round, not one); the offline sandbox's is `KRUNKNATIVE_EVENT=<id>` (v10's sandbox
  inits with eventIndex 0, :261891). The official link forwards init [2].
- **The EVENT prefab** (prefab 40, `models/event_0.obj`, the egg hunt's eggs): map.generate skips EVENT objects in a
  custom game (:189737); in a public game they are ordinary `GameObject`s, with `uid: this.objects.length` when they
  collide (:187029-187059). Their meshes form the `event_0` group. The "start" handler (:256309-256313) shows the
  group for event 3 unless spectating and hides it for any other event; the renderer keeps a hidden group hidden for
  the next maps (:234342). KN keeps EVENT objects in `map->objects` on both ends, makes them inert in a custom game
  (`live_events_map_objects`), and applies the start rule at our spawn and at spectate_begin
  (`client_event_objects_on_start`). They cast shadows through the ordinary map-model path (`client/src/shadow.c`).
- **Object ids:** `shared/src/map_game_uids.c` reproduces v10's GameObject numbering (collidable objects, two for a
  FLAG, EVENT omitted in a custom game). The official link builds that table after `io.map` resolves the map and
  translates object uids in both directions (`client/src/net/krunker_link_world.c`). Thus an EVENT object inserted
  before another object does not shift the ids KN assigns to the official server's objects. KN's own server uses
  `map->objects` indexes on its native wire and uses the same table for egg keys.
- **The egg hunt** (`game.foundEventItem`, :220700-220728): during event 3, a shot, swing or damaging projectile on an
  EVENT object sends the finder a hit marker. Once per session per egg, a signed-in, unflagged account asks the brain
  (`egg_found`, `services/brain/db_eggs.c`, migration `0363_event_eggs`). A new find broadcasts
  `event.easter.found.new` with the count of 187 and gives the `eggs` challenge +1; the 187th broadcasts
  `event.easter.completed` and gives +10; an already-found egg sends `event.easter.found.old` only to the finder.
  Guests and offline players can hit eggs but have no database finds, as in v10. Leaving or changing account clears
  the session's temporary eggs and pending answers (`server_events_forget_client`).
- **Tests:** `live_events_test` covers the object lifecycle, hit kinds and the uid table;
  `services/brain/tests/eggs_test.py` covers new/old/completing finds, per-account isolation and the database rows;
  `client/tests/live_events_e2e.py` covers the rendered objects. The official-link packet tests are
  `krunker_link_test`.

## 3. The client: the popup and Event Challenges

- **window.eventPopup** (:260286-260320): getActiveEvent(Game.eventIndex) as a genericPopup "eventPop": the header
  image and name, longDesc, "Earn these skins from ..." with 12 tiles (sprays animated), "Starts / Ends in", View
  Challenges. 10.0.3 defines it but nothing calls it (v7.1.4's `#eventAd`, src/index.js:13755-13765, left the page);
  KN registers it as the `eventPopup` handler (`client/src/ui/v10_events.c`). KN: no "Ends in" line for an
  open-ended event.
- **Event Challenges** (windows[32], :250765-250879): with an event running (hasActiveEvents) and `event`
  challenges, the home shows the section header, a card per event with challenges (its image, name, progress), and
  Map Challenges moves 30 px down; the `event` category lists the event's challenges (`DAT.evt == id`). KN:
  `client/src/ui/v10_rewards.c`; the challenges are kn_brain's (`challenges` table rows of `ctype` "event").
- Images: `ui/img/events/` holds every event image assets.krunker.io serves (ids 4, 5, 6, 7, 8 webp, 10; `ui/README.md`).

## 4. The server: limited-time modes and the event reward

- **getPossibleMatches / genMatchList** (:222421-222463): a preventCustomGames mode whose `event` is the round's
  eventIndex (All or Nothing 4, Mag Dump 5) becomes possible, in an event bucket; with mode 0 in the rotation and the
  event's `limitedTimeMode`, its first shuffled pair is the first pick. KN: `shared/src/game_config.c`
  (`Game.event_ltm`); the event's mode needs no place in the server's own rotation. The Host Game list is unchanged
  (`!preventCustomGames || isDev`, :248853).
- **endGame's event reward** (:221732-221752), after the map reward, for an account that is not hack-flagged in a
  public round: the store skins with `eventReward == eventIndex` and `seas == currentSeason` on an eligible map; the
  roll; the ones the account lacks; getItemByWeights; giveSkin "eventreward". KN: `live_events_reward`,
  `server_events_reward` (the store from `assets/skins.json`), given through the brain's give_skin (the "Items Found"
  card). Difference: the event's `rewards.skins` are candidates too (a feed cannot tag store skins with
  `eventReward`; the store's only tagged skins are event 8's of season 9). The end table's `[reward, skin]` cell is not
  sent (as for the map reward).
