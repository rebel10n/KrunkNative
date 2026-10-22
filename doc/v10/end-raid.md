# End screen "Play Raid": v10.0.3 and Krunker Box Office

The end screen's raid promo ("Battle Bosses in Raids!", "Play Now ->") is the card KrunkNative calls
"Play Raid". This note records what v10.0.3 does when the card is clicked, what Box Office did before the
fix, and what it does now.

Offsets are bytes into the decoded bundle `~/.cache/kn-v103/deob-10.0.3.js` (game.js, deobfuscated; the
readable 10.0.2 copy is `~/.cache/kn-v10/v10.js`). Identifiers in parentheses are the minified names.

## v10.0.3: the card

The card's markup, the click and the roll:

| What | Offset | Notes |
| --- | --- | --- |
| end template: `<div id="raidPromoContainer">` | 11884791 | the card's mount point, after `.otherEarnings` |
| end screen mounts the card when `otherData.showRaidPromo` | 11894053 | `isDouble: !!this.otherData.isDoubleRaidDrops` |
| the roll: `showRaidPromo: this.modeIndex !== 23 && !this.isCustom && this.isPubType() && Math.random() < 0.5` | 10703086 (`Math.random` at 10703164) | not on a Raid match, not custom, public mode, half the time |
| card click handler `iìiíïîi()`: `if (typeof window.openHostWindow == "function") window.openHostWindow(false, 4);` | 11296202 (call at 11296278) | the whole click |
| card markup: `class="crStat raidPromo"`, "Play Now &rarr;" | 11296623 | `cursor: pointer`, one `click` listener |
| card texts: "2x Raid Drops This Weekend!" / "Battle Bosses in Raids!" | 11297046 / 11297144 | gold when `isDouble`, `#d95487` otherwise |

So the card does not call a matchmaker. Its click opens the host window's tab 4.

What `openHostWindow(false, 4)` opens (`window.openHostWindow`, defined at 12859443):

- On an official krunker.io session (`isOfficial`, check at 12859598) it shows window 0 (the server list),
  not the host window. No login is asked for.
- Otherwise `showWindow(8, false)` with the tab index 4: the **Raid Server Setup** (title at 752557).
  Its `forceDat` is map `[18]` (Tortuga, 753074) and mode `[23]` (Raid, 753092), 8 players.
- Create Game in that setup sends `CUSTOM_SERVER` (the `"hosting"` path, 12856978) to the server the client
  is connected to. It makes a **private** room. It is not a matchmaker seek.

The matchmaker is reached only through the Find Game windows and the Quick Play seek, never by the card:

| What | Offset | Notes |
| --- | --- | --- |
| `openServerWindow` (Find Game) | 12857211 | 0 Public, 1 Customs, 2 Raids, 3 Zombies, 4 Trade Plaza |
| Raids filter `{oc: 1, g: 23}` | 12857840 | `oc` official custom, `g` the mode; Zombies `g: 35` (12858408), Trade `g: 18` (12858984) |
| finder search `iìiîïìï(filter, n, anyRegion)`: `fetchGameList`, the filter's keys, drop full lobbies, sort by clients (the default region first unless any-region), keep `n` | 12843919 | |
| join a found game `iíííìîi(id)`: `fetchGameInfo`, then join unless full | 11529148 | |
| "Searching..." popup `iïîîíïï` | 12679118 | |
| message popup `iíìîíîï(kind, text)` | 12677882 | the messages below |
| "Took to long to find" (2 s after a join that did not start) | 12858142 | |
| "Couldnt find any" (no official custom of that mode with room, or the list failed) | 12858248 | |
| matchmaker endpoints: `matchmakerRequest` 7954290, `fetchGameList` 7952138 (`/game-list`), `fetchGameInfo` (`/game-info`), `matchmakerSeek` 7952613 | | list and seek send `hostname` (and region / game / dataQuery); no account token is required, `validationToken` is optional |

## v10.0.3: what happens with no lobby

- The card itself never searches, so with no Raid lobby nothing happens on the card's click: the host window
  opens and the player creates a room.
- Find Game's Raids (2) searches the official customs of mode 23 with room, the krunker.io region first,
  then the busiest. A join that does not start within 2 s shows "Took to long to find"; no custom at all
  shows "Couldnt find any". Find Game never starts an offline game.

## Box Office before the fix (HEAD d8470fa6)

The card is `client/src/ui/hud_endscreen_v10.c:186-196` (`mount_raid`): `onclick="knPlayRaid()"` (193-194).
The handler and the seek lived in:

- `client/src/ui/ui_handlers.c:273-278` `h_play_raid`: `servers_ui_play_map_mode(RAID_PLAY_MAP, index >= 0,
  MODE_ID_RAID)`, and if that returned 0, `host_catalog_map(index, MODE_ID_RAID)`. The second line is a direct
  offline start (`host_catalog_map` -> the sandbox).
- `client/src/ui/servers_ui.c:4361-4392` `servers_ui_play_map_mode`: with a mode it skips the play seek
  (`mode < 0` at 4379), asks the brain's game list (4388), and returns 0 for a catalogue map (4389). It also
  returns 0 at once when no backend is available (4362), which sends `h_play_raid` to the direct offline start.
- `client/src/ui/servers_ui.c:4304-4352` `on_play_list`: when no Raid lobby on Tortuga is listed it reaches
  `else if (g_play_catalogue)` (4330) and starts the offline game on the map (`local_game_host` 4346, or
  `ui_console_go_offline(..., "play")` 4351), in Raid mode (`net_mode_index`).

Cause: a failed seek falls back to the offline sandbox (on_play_list 4330-4351, and the catalogue return at
4389), and with no backend the handler starts the offline game directly (ui_handlers.c:277). The mode id is
right (`MODE_ID_RAID` is 23). `client/src/net/offline.c` is the sandbox engine only; the callers start it.

## Box Office after the fix

- The card's click calls `servers_ui_play_raid("Tortuga")` (`ui_handlers.c` `h_play_raid`, one call).
- `servers_ui_play_raid` (`client/src/ui/servers_ui.c`, after `servers_ui_play_map`) picks the matchmaker by
  the Server Type (`seek_prefs_servers()`, Quickplay's setting):
  - krunker.io: `open_server_window(2)`, Find Game's Raids (the v10 path above).
  - Box Office and Both: `/mm/game-list?hostname=krunknative` with `g_play_raid` set. `on_play_list` joins a
    Raid lobby on Tortuga by its id (`switch_server`, the seek by id). Both then asks krunker.io's Raids when
    the Box Office list has none.
  - no brain (or the list is refused at once): the no-lobby path.
- No lobby: `raid_no_lobby` shows v10's "Couldnt find any" (`kn.finder.none`, the text of v10's
  `iíìîíîï` at 12858248) and prints `[mm] raid play: no Raid lobby on Tortuga: Couldnt find any`.
- The offline sandbox is not started by the card. Offline starts only when the player picks it (the
  console's `offline`, the offline button, Host Game without a server).

Intentional difference from v10: v10's card opens the Raid Server Setup (a private room). Box Office has no
client-side room host (doc/backend.md 8.11), so the card seeks a public Raid lobby instead, as the user asked.

## Tests

`make raid-play-e2e` (`client/tests/raid_play_e2e.py`, stub brain, krunker.io off, no asset fetch):

- C0: the card's onclick is `knPlayRaid()` and it no longer opens the Raid Server Setup.
- C1: the brain lists no Raid on Tortuga: the game list is asked, no lobby is sought or joined, "Couldnt find
  any" shows, the log has the no-lobby line, and the console status shows no Raid on Tortuga (no offline game).
- C2: a Raid lobby on Tortuga with room: its seek goes out and the client joins it, with no message.
- C3: no brain at all (the brain URL refuses): "Couldnt find any" shows and no offline game starts.

Before the fix C1 and C3 fail (the sandbox loads `Tortuga (34), mode Raid (23)`); C2 passes. After the fix all
15 checks pass.
