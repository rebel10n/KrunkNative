# v10.0.3 server-to-client packet coverage

Which of Krunker v10.0.3's server-to-client (S2C) packets the official-server link translates, which ones Krunker Box
Office covers some other way, and which ones it ignores on purpose. Line numbers are `deob-10.0.3.js:<line>`
(`/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js`). The client registers its handlers in one object at
:261708-261760 (`iíÍíiïï`). The socket class handles a few names itself at :111420-111455 (`_0`, `_1`, `io-init`,
`cpt`, `cptR`).

## Method

`deob/tables/s2c-1003.json` has 263 names. A name counts as handled when `client/src/net` references its
`KRUNKER_S2C_<NAME>` id (`shared/include/krunker_build.h`), or when it handles the wire name as a string
(`krunker_link.c` `account_scoped`: the account frames go to the account UI). The 2026-10-24 audit
(`deob/audit-2026-10-24/rendering-network.md` row 25) counted 111 unreferenced names. When this pass started there were
110. Each one's handler was read in the deob and given one of these classes:

| Class | Meaning |
|---|---|
| **ported** | Translated now (`client/src/net/krunker_link_s2c.c`). The KN wire carries the same event, and the server sends it when v10's server would. |
| **by name** | Already handled by wire name: `account_scoped` -> `io.account_frame` -> the account UI (`client/src/ui/account_ui.c`, `battlepass_ui.c`, `v10_daily_spin.c`). The link now also counts them as handled. |
| **KN equivalent** | Krunker Box Office does this through its own service (kn_brain, its own store or accounts). On an official server the packet is about the krunker.io account, which this client does not keep. Ignored on purpose. |
| **irrelevant** | Real-money store and payment providers, ads, surveys, third-party account linking, developer debug views, or an empty handler. Ignored on purpose. |
| **other agent** | Owned by another feature this round (trade, poses, AI parts). Not touched here. |
| **open** | Needed for UI or render parity and not ported yet. The reason is in the table. |

`krunker_link_s2c_frame` returns 1 for every **ported**, **by name**, **KN equivalent** and **irrelevant** row, so
`KrunkerLink.unknown_in` counts only the **open** and **other agent** names.

## New KN wire packets

| EXT subtype | Name | v10 | Body | Sent by KN's server |
|---|---|---|---|---|
| 0x0204 | `NET_EXT_PLAYER_ANIM` | `c` PLAYER_ANIM | `NetPlayerAnimPacket {int32 sid, uint8 clip, float reps}`. `clip` is an index into `["Idle", "Move", "MoveFast", "Jump", "Melee", "Shoot", "Crouch"]` (:21165). | Yes. `shared/src/player_anim.c` runs `player_play_clip` from `player_jump` and `player_melee` on the authority. It sends only for a class whose `aID` holds `g`, `q` or `f` (hasAnims, :194877). It goes to the spectators (`NET_TARGET_NON_PLAYERS`) and to every other active player within `mode.viewDis \|\| 600` (:193806-193815). |
| 0x0205 | `NET_EXT_PLAYER_ASSET_ID` | `uai` UPDATE_ASSET_ID | `NetPlayerAssetIdPacket {int32 sid, char asset[32]}` (`assetTxt += ""`) | Yes. KrunkScript's `player.assetID = v` runs `player_update_asset_id` (players.updateAssetID :197964): with `config.mdl` and a non-empty text the authority broadcasts it (:197968-197970). |
| 0x0300 | `NET_EXT_SKIN_DEFINITION_CHANGED` | `sdc` | `NetExtJsonChunk` slices of one JSON array `[skinId, skin]`, like the entity records | No. kn_brain owns KN's skin definitions and has no push yet. The subtype is ready for one. |
| 0x0303 | `NET_EXT_SPEECH_BUBBLE` | `sb` SHOW_SPEECH_BUBBLE | `NetSpeechBubblePacket {uint32 ms (0 = 2500), uint8 sigh, char text[256]}` | No. The welcome text answers v10's `["sb", "welc", username]`, and the official game server writes it. It is not in the bundle. |
| 0x0208 | `NET_EXT_PLAYER_INTER_PROG` | `p` INTER_PROG / `mp` INTER_M_PROG | `NetInterProgPacket {uint8 bar (0 interact, 1 charge), float prog}` | Yes. Each tick, to every spectator following an active player, when that player's `interact_progress` or charge (`charge_time / (chrgTime \|\| throwCharge)`), rounded to 3 places, differs from what the spectator got last (:222274-222292). v10 keeps the last value per player; KN per spectator, so a switch shows the new player's bars at once. |
| 0x0304 | `NET_EXT_UNBOX_MESSAGE` | `unb` / `unbS` | `NetUnboxMessagePacket {int32 uid (-1 named), int32 item, int32 kr (> 0: freeKR[item]), uint8 kind, char name[32]}` | Yes: `unbS` when its map wheel stops, `unb` for kn_brain's `unbox` push (the rows) |
| 0x0305 | `NET_EXT_LOTTERY_PRIZE_WON` | `lotWin` | `NetLotteryPrizePacket {double quantity, int32 uid, uint8 type (other, skin, currency, subscription), char ref[51]}` | Yes, for kn_brain's `raffle-win` push |
| 0x0306 | `NET_EXT_SPIN_WHEEL_RESPONSE` | `spinW` | `NetSpinWheelPacket {double funds, int32 uid, float rand, int32 skin, uint8 mine}` | Yes: a map wheel spun through kn_brain (funds only in the spinner's own) |
| 0x0500 / 0x0501 | `NET_EXT_CHARGE_CONSENT_REQUEST` / `_RESPONSE` | `chreq` / `chrsp` | `NetChargeConsentRequestPacket {double amount, int32 id, uint8 modal}`, `NetChargeConsentResponsePacket {int32 id, uint8 accept}` | Yes, on a monetized studio game; the response grants or declines (ks_server_host.c) |
| 0x0602 | `NET_EXT_WARS_CLAN` | `warsClan` | `NetWarsClanPacket {int32 clan_id, uint8 has, char clan_name[32], clan_logo[64], banner_color[24]}` | Yes: kn_brain's map controller on an eligible Turf Wars round |
| 0x8070 | `NET_EXT_KN_OFFICIAL_EDIT_MAP` | `edMp` | `NetExtBlobChunk {uint16 part, parts}` slices of the map JSON (more than `NetExtJsonChunk`'s 255) | Client-local (the link only). |

Existing subtypes and packets the link now also fills from official packets: `NET_PACKET_COMP_LOBBY` (`cd` / `cda`),
`NET_EXT_LATEST_RANKED_MMR_CHANGE` (`lrmr`), `NET_EXT_GAME_ANNOUNCEMENT` (`gameAnnouncement`), `NET_PACKET_CHAT`
`NET_CHAT_FOUND_JUNK` (`jnk`) and `NET_PACKET_SCRIPT_LOG` (`ksL`).

Client consumers are in `client/src/net/s2c_events.c`:

- `c` plays the clip on the player's drawn class model when the player is active and its class's `aID` is animated
  (`client/src/player_class_model.c`, doc/v10/class-models.md).
- `uai` keeps the asset in `player.asset_txt` (only for a class with a model) for the next generateMeshes, which takes
  it, and rebuilds an active player's model at once (regenMeshes). One for a player this client has not added is
  dropped (findBySid, as v10); KN's server sends the swap of a spawn's `onPlayerSpawn` after that SPAWN.
- `sb` shows the bubble on `#txtBubble` with tags dropped, because `menu_show_speech_bubble` escapes its text. When
  `sigh` is set it first plays `sigh_0` or `sigh_1` at 0.2.
- `sdc` is applied by `viewer_skin_define` (`client/src/viewer/viewer_skin_define.c`). It maps the fields as
  `tools/ui-import/gen_viewer_store.js` does and repeats v10's entry setup (:116530): BACK gets `assetType` 2, HEAD gets
  `assetType` 1, and a `free` item gets `noSpin` and is left out of the wheels. Not applied: a `template` key (the
  generated table folds v10's template rows in already), a dye's colours, and a new index in the dropdown and wheel
  lists. Models already built keep their look until rebuilt, as in v10.
- `p` / `mp` keep the followed player's bars; `s2c_events_spectate_bar` gives them to `#interactProg` (ui_interact.c) and
  the charge bar (build_ui.c) while we spectate, and they start at 0 with every spectate.
- `unb` / `unbS` / `lotWin` are chat lines (`hud_ui_unboxed`, `hud_ui_raffle_won` in hud_ui.c): store names from the viewer
  table, rarity colours, `rainbowText` for Unobtainable, `viewItem` on skins. `unb` / `unbS` wait for Show Unboxings.
  The raffle line escapes the prize text, which v10 writes as markup.
- `spinW` pays our account (`account_set_funds`, `account_add_skin`) and spins `client/src/prize_wheels.c`'s spinner:
  the map's PRIZE_WHEEL objects whose wheel (`sty`, store.wheels 0..2) is not missing, ticket-only, hidden or locked,
  stepped every 50 ms like `updateSpinners` with its tick_0, reward, cheer and explosion sounds. A pool wheel throws in
  v10 at its stop (`wheel.rarities` undefined), so it ends silently here too. The wheels are drawn
  (`client/src/prize_wheel_render.c`: render.addSpinner's canvas plane, painted at each step while needsUpdate: the
  rarity / pool slices, the stop's widening, the pointer, the SPIN / price hub, the context's lingering 20 px shadow);
  the "Press [G] to Spin <wheel> [<price> KR]" prompt (interact 9, uid = the spinner's place,
  `shared/src/map_spinners.c`) sends `int` 9 and the confirm key `cnfm` 2. At a rarity wheel's stop the won item goes up in
  front of the hub (meshPrev): its store model through `client/src/store_item_model.c` (the viewer's loadMesh /
  loadAnimMesh code), a spray's plane, bobbing; a dye's preview player is not drawn.
- `chreq`, `warsClan` and `edMp` are `client/src/ui/s2c_ui.c`. The charge modal holds the pointer lock off (menu_ui.c).
  The keys answer a request when the vote kick did not take them. The Turf Wars popup is scheduled when we enter (the
  menu closes on a ready game we play or spectate) or when the clan comes while we are in; it shows once per game.

## Classification of the 110 names

| Name | Wire | Handler | v10 behaviour | Class | KN mapping / reason |
|---|---|---|---|---|---|
| RPC_SUCCESS | `_0` | :111438 (socket) | resolves the rpc promise | by name | `account_scoped`; krunker_net.c matches the rpc |
| RPC_ERROR | `_1` | :111438 (socket) | rejects the rpc promise | by name | as above |
| FORCE_PING | `fp` | none | no handler registered in 10.0.3 | irrelevant | nothing to do |
| GAME_INSTANCE_ID | `inst-id` | :257259 | fetches game info for the menu's region label | KN equivalent | the seek result already names the game (krunker_net.c) |
| REDEEM_VOUCHER_RESPONSE | `vr` | :253771 | voucher window result | KN equivalent | vouchers are kn_brain's |
| ADD_MAIL | `kml` | :253788 | mail window data | KN equivalent | kn_brain |
| READY | `ready` | :261397 | news maps, register-ad popup, then `send("sb", "welc", username)` (:261464); falsy: `socket.close()` | **ported** | sends `["sb", "welc", account name \| null]`; falsy raises `KRUNKER_LINK_EV_CLOSE` |
| CLAN_RESPONSE | `cln` | :255453 | clan window | by name | `account_scoped` |
| PURCHASE_RESPONSE | `pur` | :242316 | KR real-money purchase | irrelevant | no real-money store |
| PURCHASE_PRODUCT_RESPONSE | `ppr` | :242357 | shop product purchase | KN equivalent | the shop is kn_brain's |
| PURCHASE_PREMIUM_RESPONSE | `purprm` | :242434 | premium purchase | irrelevant | real money |
| PURCHASE_BUNDLE_RESPONSE | `purb` | :242470 | bundle purchase | irrelevant | real money |
| UPDATE_PREMIUM_AMOUNT | `uprm` | :242710 | `account.premiumT` | KN equivalent | kn_brain account |
| PURCHASE_RESPONSE_2 | `purbp` | windows[5].purchaseRes :248266 | battle pass purchase | KN equivalent | the pass on the official socket is `bpdl` / `bppd` (by name); purchases are not offered |
| PURCHASE_LEVEL_RESPONSE | `purbpl` | windows[5].purchaseLvlRes :248160 | pass level purchase | KN equivalent | as above |
| CLAIM_ITEM_RESPONSE | `bpclmr` | windows[5].claimItemRes :248052 | pass claim | KN equivalent | as above |
| SHOW_SPEECH_BUBBLE | `sb` | :261181 | menu mascot bubble, `ms \|\| 2500`, optional sigh | **ported** | `NET_EXT_SPEECH_BUBBLE` |
| CLAN_LEAVE_RESPONSE | `clnL` | :255456 | clan window | KN equivalent | kn_brain clans |
| WARS_CLAN | `warsClan` | :255551 | `territorialWars.setClanInfo`, then once per game a "Turf Wars / Territory controlled by X" textPopup 1 s after entering | **ported** | `NET_EXT_WARS_CLAN`, also from init's `syncData.warsClan` (:256999, `krunker_link_s2c_init`); the clan and the popup are `client/src/ui/s2c_ui.c`; the `wars_banner*` server nodes' banners (models 64295 / 64301 / 64303, the banner colour as their paint, the logo or the name as a decal) are `client/src/wars_banners.c` (no KN map has the nodes yet). KN's server sends it too: at an eligible round start (the Turf Wars kill rule) it asks kn_brain for the clan that won the map yesterday (`turf_clan`), broadcasts it and sends it after every joiner's INIT (server/src/brain_events.c) |
| UPLOAD_RESPONSE | `upR` | :255675 | upload window text | KN equivalent | kn_brain uploads |
| UPLOAD_PROFILE_RESPONSE | `upPR` | :255687 | profile picture uploaded | KN equivalent | kn_brain |
| SPIN_RESPONSE | `spin` | :254882 | prize wheel result | KN equivalent | spins are kn_brain's (spin_wheel.c) |
| SAVE_SPIN_HISTORY | `spinH` | :254698 | spin history popup | KN equivalent | kn_brain |
| SPIN_WHEEL_RESPONSE | `spinW` | :254991 | a map spinner object's spin: `updateSpinner(uid, ..., skin)`, funds and skin to the spinner | **ported** | `NET_EXT_SPIN_WHEEL_RESPONSE`; `client/src/prize_wheels.c` keeps the map's spinners and their spinInfo, steps them every 50 ms and plays the ticks / reward sounds; ours pays the account. The wheel canvas and the won item's model are `client/src/prize_wheel_render.c` (a dye's preview player is not drawn). KN's server sends it too: interact 9 in reach with the KR offers the spin (`cnfm` [uid, 2]), `cnfm` 2 spins that store wheel through kn_brain's KR wallet (`map_spin`: the KR, the skin and their ledger rows in one transaction) and the stop goes to everyone (server/src/brain_events.c). One spin per wheel at a time (KN: v10 restarts a turning wheel and loses its unbS) |
| SPIN_KR | `spinKR` | :255333 | free KR spin | KN equivalent | kn_brain daily spin |
| PURCHASE_ERROR | `pErr` | window.purchaseError :243648 | purchase error popup | irrelevant | real money |
| UNBOX_MESSAGE | `unb` | :254321 | chat line "X unboxed [item]" (showUnboxings) | **ported** | `NET_EXT_UNBOX_MESSAGE` NAMED -> `hud_ui_unboxed` (store names, rarity colours, Free KR items). KN's server sends it too: kn_brain pushes every store and Free KR win (`unbox`, services/brain/node_events.c) to the game server the account plays on, which sends it to everyone for rarity >= 2, else to the unboxer (KN's rule: the map wheel's unbS rule; v10's server is not in the bundle) |
| UNBOX_MESSSAGE_S | `unbS` | :254331 | chat line "<player> unboxed [item]" (a map prize wheel's stop, :219473) | **ported** | `NET_EXT_UNBOX_MESSAGE` PLAYER -> `hud_ui_unboxed`. KN's server sends it when its own spinner stops (updateSpinners on every server update, :219470-219476): everyone for rarity >= 2, else the spinner |
| UPDATE_FUNDS | `uf` | :242199 | account funds / junk | by name | `account_scoped` |
| SET_DEBUG | `debug` | :258527 | developer debug mode line | irrelevant | developer only |
| SET_EXPERIMENTAL | `exf` | :258490 | developer flag line | irrelevant | developer only |
| PLAYER_ANIM | `c` | :257769 | `playClip(player, animList[i], reps \|\| 1)` on the class model | **ported** | `NET_EXT_PLAYER_ANIM`, sent by KN's server too; played on the class model (player_class_model.c) |
| ADD_TRADE_CHAT | `tch` | :259539 | trade window chat | other agent | trade |
| INTER_PROG | `p` | :259394 | spectated player's interact bar, sent to `specStream` viewers (:222279) | **ported** | `NET_EXT_PLAYER_INTER_PROG` bar 0; KN's server sends it to each spectator following the player (server/src/net.c `net_broadcast_inter_progress`); the client draws it while spectating (ui_interact.c) |
| INTER_M_PROG | `mp` | :259398 | spectated player's charge bar (:222288) | **ported** | `NET_EXT_PLAYER_INTER_PROG` bar 1, sent and drawn as `p` (build_ui.c) |
| NEW_PASS_RESPONSE | `upp` | :244558 | password change | irrelevant | the official account's auth is FRVR's |
| ADD_DEBUG_POING | `dbgp` | :241808 | debug sphere | irrelevant | developer view |
| CRAFT_RESPONSE | `crftr` | windows[13].craftRes :249907 | crafting window | KN equivalent | kn_brain |
| CUSTOM_NETWORK_MESSAGE | `sd` | :241493 | `mainScript.onNetworkMessage(id, data)` | done | `NET_PACKET_SCRIPT_MSG` (krunker_link_s2c.c `on_network_message`); the msgpack data's `V_` keys are stripped into the `ks_serialize` body by `ks_net_body_from_official` (shared/src/ks/ks_net_official.c). The client's `GAME.NETWORK.send` goes back out as C2S `sd` (krunker_link.c). doc/v10/krunkscript-1003.md section 6 |
| CONSOLE_LOG | `clog` | console.log | developer log | irrelevant | nothing shown |
| OPEN_MAIL | `rml` | :253782 | mail window | KN equivalent | kn_brain |
| UPDATE_MAIL | `gmc` | :253785 | mail count | KN equivalent | kn_brain |
| UPDATE_CHALLENGES | `chg` | :255485 | challenge progress | by name | `account_scoped` |
| UPDATE_CHALLENGES_PROGRESS | `uchp` | :255501 | challenge progress | by name | `account_scoped` |
| SOCKET_PING_RESULT | `pir` | :258337 | empty handler | irrelevant | nothing to do |
| CHALLENGE_COMPLETED | `chgC` | :255516 | end-screen challenge card | by name | `account_scoped` |
| COUNTRY_FLAG_UPDATE | `flgr` | :255545 | profile flag | KN equivalent | kn_brain profile |
| BADGE_UPDATE | `bdgr` | :255573 | profile badge | KN equivalent | kn_brain |
| BADGE_CLAIM_RESPONSE | `cbdgr` | :255580 | badge claimed popup | KN equivalent | kn_brain |
| BADGE_CLAIM_MULTI_RES | `nbdgr` | :255586 | badges unlocked popup | KN equivalent | kn_brain |
| HIDE_STATUS | `hdsr` | :255593 | profile hide status | KN equivalent | kn_brain |
| PREMIUM_RESPONSE | `prmR` | :255424 | windows[13].resp | KN equivalent | kn_brain |
| ALIAS_RESPONSE | `nmeR` | :255429 | windows[4].aResp (alias change) | KN equivalent | kn_brain |
| TRADE_RESPONSE | `trd` | :253238 | trade flow | other agent | trade |
| SHOW_POPUP | `pRes` | :252578 | generic success / info / error popup | KN equivalent | the popups of KN's own windows; no sender in the bundle's game code |
| LOADING_POPUP | `lmsg` | :252589 | loading popup | KN equivalent | as above |
| ANNOUNCEMENT | `ann` | :244311 | empty handler | irrelevant | nothing to do |
| UPDATE_TRADE_RESPONSE | `utrd` | :253308 | trade items | other agent | trade |
| UPDATE_TRADE_CURRENCY_RESPONSE | `utrdc` | :253316 | trade currency | other agent | trade |
| TRADE_A_RESPONSE | `atrd` | :253269 | trade done | other agent | trade |
| TWITCH_DROPS_RESPONSE | `twitchDropsResponse` | :255459 | Twitch drops | irrelevant | third-party linking |
| TWITCH_REMOVE_RESPONSE | `twitchRemoveResponse` | :261619 | Twitch unlink | irrelevant | third-party linking |
| TWITCH_VERIFY_RESPONSE | `twitchVerifyResponse` | :261606 | Twitch link | irrelevant | third-party linking |
| DISCORD_LINK_RESULT | `dlnkR` | :244326 | Discord link result popup | KN equivalent | answers `dlnkW`, which this client never sends on the official socket (KN's Discord link is kn_brain's, EXT 0x0401) |
| NFT_RESPONSE | `nftR` | :252555 | NFT claim popup | irrelevant | web3 |
| PROMOS_RESPONSE | `pdR` | :255462 | promos list | irrelevant | live ops |
| DEDI_BUY_RESPONSE | `dsrvl` | :255465 | dedicated server purchase | irrelevant | real money (KN hosting is kn_brain's) |
| FRIENDS_LIST_RESPONSE | `gfrnd` | :255468 | profile friends list | KN equivalent | kn_brain / event bus |
| VOTE_MAP | `vm` | window.voteMap :257932 | map up / down vote | KN equivalent | community map votes go through kn_brain |
| UPDATE_SETTINGS | `loadS` | :244091 | cloud settings import | KN equivalent | settings are local / kn_brain |
| AI_PARTS | `aip` | AI.syncParts :206506 (sent :206566) | AI part glow, hidden nodes, held clip, solid pose | **ported** | EXT `AI_PARTS_EXT_SUBTYPE` (0x0c00), one JSON record in `NetExtJsonChunk` slices (krunker_link_world.c `ai_parts_emit`), applied by `ai_parts_on_ext` (shared/src/ai_parts.c, doc/v10/krunkscript-1003.md) |
| EDIT_MAP | `edMp` | :259803 | opens the editor on a map | **ported** | `NET_EXT_KN_OFFICIAL_EDIT_MAP` (client-local, `NetExtBlobChunk` slices) -> `s2c_ui_edit_map`: col / l false dropped, staged as `mapToEdit`, KN's editor opened (as F6). KN's own `edMp` is kn_brain's social socket (services/brain/creations.c) |
| LOAD_XSOLLA | `xst` | :242164 | payment widget | irrelevant | real money |
| SYNC_P_CON_DATA | `cd` | :256533 | the comp lobby table `{key: entry}` | **ported** | `NET_PACKET_COMP_LOBBY` CLEAR + SET per entry; KN's server already sends it (shared/src/comp_lobby.c) |
| SYNC_P_CON_OBJ | `cda` | :256526 | one comp lobby entry, null removes | **ported** | `NET_PACKET_COMP_LOBBY` SET / REMOVE |
| CON_UID | `uid` | :261633 | our lobbyPlayers key; `conUID_` for a reconnect's `clientUID` | **ported** | the comp lobby's own entry (uid 0 = the INIT client_id); the table is sent again when it comes late. Reconnect `clientUID` is not used (krunker_net.c joins fresh) |
| ADD_HELPER_C | `hlpr` | render.addHelperC :232576 | debug helper | irrelevant | developer view |
| DEBUG_COMPLEX_HITBOX | `dch` | :257798 | debug hitbox view | irrelevant | developer view |
| ADD_HITBOX_HELPER | `hbdb` | :258441 | debug hitbox boxes | irrelevant | developer view |
| SCRIPT_LOG | `ksL` | :261492 | console `[KRUNKSCRIPT] args.join(" ")` | **ported** | `NET_PACKET_SCRIPT_LOG` (KN's server sends it from server/src/ks_server_host.c) |
| UPDATE_ASSET_ID | `uai` | :257784 | `players.updateAssetID`: a class model's asset changes | **ported** | `NET_EXT_PLAYER_ASSET_ID`, sent by KN's server too (KrunkScript `player.assetID`); the model is rebuilt with it (player_class_model.c) |
| FOUND_JUNK | `jnk` | :242285 (sent :221143) | "You found [x.xx] Junk" line, junk balance | **ported** | `NET_PACKET_CHAT` `NET_CHAT_FOUND_JUNK`, the line KN's server sends (shared/src/triggers.c `game_found_junk`); the balance stays the account frames' |
| AVAILABLE_BUNDLE_DATA | `gbdr` | :242875 | store bundle ad | irrelevant | real money |
| ALL_BUNDLE_DATA | `abd` | :242852 | store bundle ad | irrelevant | real money |
| FORCE_VIDEO_AD | `vad` | window.forceVideoAd :262361 | video ad | irrelevant | ads |
| OPEN_METAMASK_AUTH | `vmm` | :252520 | MetaMask sign-in | irrelevant | web3 |
| METAMASK_LINK_RESPONSE | `vmms` | :252541 | MetaMask linked | irrelevant | web3 |
| METAMASK_UNLINK_RESPONSE | `rmm` | :252548 | MetaMask unlinked | irrelevant | web3 |
| BOTS_POPUP | `bcfg` | window.botsPopup :254234 | developer bot debugging popup | irrelevant | developer only |
| UPDATE_COUNTRY | `cntry` | :242964 | empty handler | irrelevant | nothing to do |
| DATA_LOADED | `bpdl` | :255599 | battle pass data | by name | `account_scoped` (battlepass_ui.c) |
| RECIEVE_DOUBLE_XP | `rrqdxp` | :262335 | XP doubled after an ad | irrelevant | ad reward |
| SYNC_BP_PROG | `bppd` | :255602 | battle pass progress | by name | `account_scoped` |
| SURVEY_BUNDLE_RESPONSE | `svybRes` | :242299 | survey reward | irrelevant | surveys |
| GAME_ANNOUNCEMENT | `gameAnnouncement` | :244564 | announcement banner | **ported** | `NET_EXT_GAME_ANNOUNCEMENT`: the message and the `ranked-queue` action's label (:133490). Not carried: target, trackId, bundleId, displayIds, the other actions (`profile`, `shop`, `events`, `raffle`, `leaderboards`, `ranked`, `login`, `register`, :133340) |
| GAME_SURVEY | `gameSurvey` | :244851 | survey toast | irrelevant | surveys |
| SKIN_DEFINITION_CHANGED | `sdc` | :261708-261714 | `store.skins[skinId] = skin` + entry setup (:116530) | **ported** | `NET_EXT_SKIN_DEFINITION_CHANGED` -> `viewer_skin_define` |
| SPIN_WHEELS_UPDATED | `swu` | :261715 | `store.applySpinData`, prize wheel redraw | KN equivalent | the wheels this client shows are kn_brain's (shared/src/store_spins_table.c) |
| DEBUG_RAYCAST | `debug_rc` | :261745 | collision debug drawing | irrelevant | developer view |
| POPUP_REGISTER_REWARDS | `prr` | :242717 | register-rewards popup | irrelevant | FRVR registration promo |
| LATEST_RANKED_MMR_CHANGE | `lrmr` | :255609 | end-screen rank card values when the change is non-zero | **ported** | `NET_EXT_LATEST_RANKED_MMR_CHANGE` (rp_change 0: the official balance comes as `ub`) |
| GUEST_EARNED_REWARDS | `ger` | :133742 | refetches the FRVR guest profile | KN equivalent | about krunker.io's guest; KN's own `ger` is `NET_EXT_KN_GUEST_EARNED` from server/src/accounts.c for kn_brain's guest |
| DAILY_SPIN_CLAIM_RESPONSE | `dslr` | :261748 | daily spin ticket | by name | `account_scoped` (v10_daily_spin.c) |
| UPDATE_LOTTERY_ENTRIES | `lotEnt` | :255149 | raffle window entries | KN equivalent | the raffle is gapi / kn_brain's |
| LOTTERY_PRIZE_WON | `lotWin` | :255154 | chat line "<player> won <prize> in the raffle!" | **ported** | `NET_EXT_LOTTERY_PRIZE_WON` -> `hud_ui_raffle_won` (skin / currency / subscription prizes). KN's server sends it too: kn_brain's draw pushes each prize (`raffle-win`) to the game server its winner plays on, which sends it to everyone there (the winner's uid) |
| LOTTERY_PRIZE_WON_PLAYER | `lotWinP` | :255184 | our prize into the account | KN equivalent | kn_brain account |
| LOTTERY_STARTED | `lotStart` | :255225 | raffle window refresh | KN equivalent | gapi / kn_brain |
| LOTTERY_ENDED | `lotEnd` | :255230 | raffle window refresh | KN equivalent | gapi / kn_brain |
| CHARGE_CONSENT_REQUEST | `chreq` | :257673 (sent :221249) | "This game wants to charge you N KR" modal, 15 s, answer `chrsp` | **ported** | `NET_EXT_CHARGE_CONSENT_REQUEST` -> `s2c_ui_charge_request`: #chargeModal or #chargeConfirm ([1] / [2] = kickVoteY / kickVoteN), closed after 15 s, at init and on leaving; `NET_EXT_CHARGE_CONSENT_RESPONSE` goes out as `chrsp`. KN's server sends it on a monetized studio game (the hosted published map's studio, kn_brain `studio_game`): `authorizeSpend` / `charge` ask, an accepted `chrsp` is granted for 30 s, `transact` / `charge` take the KR through kn_brain (`studio_charge`: the ledger row, the studio's 90 %, idempotent); declined or 15 s unanswered: false (server/src/ks_server_host.c). Elsewhere it answers false first, as v10 does |
| PLAYER_POSE | `pz` | :261743 | `players.setPose` | ported | `NET_EXT_PLAYER_POSE` (krunker_link_script.c, init `plrPose` too; client/src/player_pose_render.c; doc/v10/player-pose.md) |

Totals: 24 ported (`c`, `ready`, `sb`, `cd`, `cda`, `uid`, `sdc`, `lrmr`, `gameAnnouncement`, `jnk`, `ksL`, `pz`, `sd`,
`p`, `mp`, `unb`, `unbS`, `lotWin`, `spinW`, `chreq`, `warsClan`, `edMp`, `uai`, `aip`), 10 by name, 36 KN equivalent, 35
irrelevant, 5 other agent, 0 open.

## Open work

- Class models are drawn (doc/v10/class-models.md); its open items are listed there (the class camera types, getAsset).
- Drawing what the client now keeps: a won dye's class-9 preview player in front of a prize wheel's hub (it needs a
  preview player outside the game, as viewer_scene.c's preview_player has).
