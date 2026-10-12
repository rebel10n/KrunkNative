# v7.1.4 upgrade: decisions

Decisions taken for the in-place upgrade from Krunker v3.1.1 to v7.1.4. The v3.1.1 port
stays on branch `v3.1.1` / tag `v3.1.1-parity`. Specs: krunkscript.md, map-logic.md,
editor.md, player-viewer.md (this directory).

| Topic | Decision |
|---|---|
| Parity target | v7.1.4 in place, no v3.1.1 mode. v3.1.1 map JSON still loads, the way v7 loads it. |
| Gameplay values | Adopt v7 movement, class, weapon and hitbox values. Re-record the shared/tests/inputs movement fixtures against v7.1.4. |
| Official maps | The catalogue becomes v7's 32 official maps with v7 indices (map-logic A6). The v3 maps stay playable as `old_*` custom maps. |
| Ranked | Keep it. v7 moves Classic FFA and Deposit onto mode ids 13/14, so the Ranked modes move to new ids 37+. Ranked matchmaking, 1v1 and Elo keep working. |
| UI | Re-import `ui/` from v7.1.4's pages with tools/ui-import, then re-wire the C UI logic (client/src/ui). Don't hand-port changes onto the v3 page. |
| GLTF and assets | Add cgltf (single header) and import v7's model and texture sets into `assets/` for skins, pets and animated models in the game and the viewer. |
| v7 bugs | Fix them, and document each fix in code. Don't replicate them. Byte-compatible serialisation quirks stay where the specs say so. |
| Offline account | The offline sandbox player counts as having an account (account-locked classes such as Hunter, Vince, Detective and Bowman unlock), as in v7's sandbox and custom games. |
| Embedded map config | It applies offline too. v7's sandbox discards it; we don't. |
| "player spawn" trigger event | It fires to map objects. v7.1.4 never dispatches it, but zombie_high relies on it. |
| Backend-only triggers | Triggers that give or take KR, save player data, or check premium/verified/level do nothing (or evaluate false) unless kn_brain supplies account data. |
| Editor one-sided scaling | Keep Ctrl-hold to disable snapping, except while dragging an X/Z scale handle on prefabs that support one-sided scaling. |
| Out of scope for now | The store economy (Battle Pass, crafting, trading), until requested. KrunkScript is in scope. |

## Packet id blocks

| Ids | Owner |
|---|---|
| 120-169, 180-189 | map-logic.md packages (C triggers, B world, D modes, G zombies, E AI, A lobby), as in map-logic.md §5.3 |
| 170-176 | player-viewer WP-B: 170 EFFECT, 171 STREAK, 172 USE_STREAK, 173 NUKE, 174 JUGGERNAUT, 175 STREAK_SLOTS, 176 reserved (bbm) |
| 177-179 | map-logic package H: AIRDROP |
| 190-194 | player-viewer WP-B: 190 PERKS (upk; map-logic's zombies block must not add a second one), 191 UPGRADES, 192 WEAPON_STREAK |
| 195-199 | player-viewer WP-C: protocol/input |
| 200-239 | KrunkScript |
| 240-249 | player-viewer WP-E (`NET_PACKET_BLOCK_SOCIAL`; `NET_PACKET_BLOCK_END` is 250): 240 LEADER_STATS; 241 DEATH_INFO, 242 PC_STAT; 243 END_MEDALS (part 2c: endData.mdls); 244 reserved; 245 CHAT_V7, 246 VOICE_V7, 247 SPRAY_V7, 248 TAUNT, 249 SOCIAL_V7 (shared/include/social_v7.h) |

New packets are gated on `NET_PROTOCOL_MINOR` (3 = `NET_PROTOCOL_MINOR_V7`, set by map-A; don't bump it again without asking the coordinator). The client negotiates per server, from the INIT version.

`NET_PROTOCOL_PATCH` (1, player-viewer WP-C) rides in HELLO and INIT (a 2-byte HELLO reads as patch 0). From 1.3.1, ENT V8, SPAWN V3 and RECON V2 carry the v7 fields; `net_peer_has_wpc(minor, patch)` gates them, and 1.3.0 peers keep the shorter forms. The ENT, SPAWN, RECON, HELLO and INIT readers accept longer payloads (prefix read).
