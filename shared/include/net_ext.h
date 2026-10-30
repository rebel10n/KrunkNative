#pragma once
// NET_PACKET_EXT (254): the envelope for packets added from Krunker v10.0.2 on (coordinator
// decision, doc/v10/net.md "KrunkNative packet ids"). The 1-byte NetPacket type stays; every new
// packet is an EXT whose payload starts with a little-endian u16 subtype, followed by the
// subtype's body (at most NET_EXT_BODY_MAX bytes):
//
//   NetPacket { type = NET_PACKET_EXT, length = 2 + n, payload = [subtype lo, subtype hi, body[n]] }
//
// EXT packets go only to peers whose HELLO / INIT announced at least NET_PROTOCOL_MINOR_EXT;
// receivers ignore subtypes they do not know (and a short or malformed envelope). Subtype ranges
// are owned per feature like the 1-byte blocks (doc/v7/decisions.md "Packet id blocks"); a body
// layout is the owner's, documented next to its struct. KrunkNative-only packets use 0x8000 up.
//
// Not ported, on purpose: v10 CHEAT_CHECK "cc" / CHEAT_CHECK_RESPONSE "ccr" (server-sent JS the
// client evals; doc/v10/net.md section 6). No subtype is reserved for them.
#include <stddef.h>
#include <stdint.h>

#define NET_PACKET_EXT 254
#define NET_EXT_HEADER_SIZE 2

typedef enum {
    NET_EXT_NONE = 0x0000,

    // 0x0100 entities (v10 KrunkScript entities; doc/v10/net.md "Entities"), S -> C. Bodies: a
    // NetExtJsonChunk header + a slice of one JSON array (below); one EXT per entity record:
    NET_EXT_ENTITY_SPAWN = 0x0100,  // "e+"  [eid, archId, tick, state{}, tag, mode, hp] (also init syncData.ent, one each)
    NET_EXT_ENTITY_REMOVE = 0x0101, // "e-"  [eid, reason]
    NET_EXT_ENTITY_STATE = 0x0102,  // "es"  [eid, tick, state{}] (one EXT per baseline of the "es" list)
    NET_EXT_ENTITY_EVENT = 0x0103,  // "ee"  [eid, name, a, b]
    NET_EXT_ENTITY_ATTACH = 0x0104, // "ea+" [playerSid, eid, {o, lk, ctl, soft, nd}] (also init syncData.entAtt)
    NET_EXT_ENTITY_DETACH = 0x0105, // "ea-" [playerSid, eid]
    NET_EXT_ENTITY_CONTROLLED = 0x0106, // "l" SYNC_PLAYER's tail [eid, tick, state{}]: the entity we control (applyControlledSync)
    NET_EXT_ENTITY_SEED = 0x0107,   // init syncData.entSeed [seed]: the match seed of the entities' ctx.rand (entities.setup)

    // 0x0200 player state, S -> C
    NET_EXT_PLAYER_POSE = 0x0200,  // "pz"  NetPlayerPosePacket: sid, pose | 0 (player_pose.h)
    NET_EXT_PLAYER_LOCKS = 0x0201, // "plk" NetPlayerLocksPacket: disableShooting, disableMelee
    NET_EXT_PLAYER_SPEED_BOOST = 0x0202, // "spb" speedBoost, silent (NetSpeedBoostPacket; official lobbies, krunker_link.c)
    NET_EXT_PLAYER_NAME = 0x0203,        // "00" a player's new name (NetPlayerNamePacket; official lobbies, krunker_link.c)
    NET_EXT_PLAYER_ANIM = 0x0204,        // "c"  NetPlayerAnimPacket: a clip of an animated class model (doc/v10/s2c-coverage.md)
    NET_EXT_PLAYER_ASSET_ID = 0x0205,    // "uai" NetPlayerAssetIdPacket: a class model's asset swapped (players.updateAssetID)
    NET_EXT_PLAYER_INTER_PROG = 0x0208,  // "p" / "mp" NetInterProgPacket: the spectated player's interact / charge bar
    NET_EXT_PLAYER_MOVE_VALS = 0x0210,   // SYNC_PLAYER's sendVals: NetPlayerMoveValsPacket, the movement scheme's state with a RECON
    NET_EXT_PLAYER_SYNC = 0x0211,        // "k" UPDATE_PLAYERS' players.hideAll: no body, ahead of every player sync's STATEs
    NET_EXT_VALUE_TEXT = 0x0212,         // "cv" / "gv" with a string value: NetValueTextPacket (numbers stay NetValuePacket)

    // 0x0300 store / live service, S -> C
    NET_EXT_SKIN_DEFINITION_CHANGED = 0x0300, // "sdc" NetExtJsonChunk + JSON [skinId, skin] (doc/v10/s2c-coverage.md)
    NET_EXT_SPIN_WHEELS_UPDATED = 0x0301,     // "swu" spin data
    NET_EXT_GAME_ANNOUNCEMENT = 0x0302,       // "gameAnnouncement" NetGameAnnouncementPacket
    NET_EXT_SPEECH_BUBBLE = 0x0303,           // "sb" NetSpeechBubblePacket: the menu mascot's speech bubble
    NET_EXT_UNBOX_MESSAGE = 0x0304,           // "unb" / "unbS" NetUnboxMessagePacket: "<name> unboxed [item]" in the chat
    NET_EXT_LOTTERY_PRIZE_WON = 0x0305,       // "lotWin" NetLotteryPrizePacket: "<name> won <prize> in the raffle!"
    NET_EXT_SPIN_WHEEL_RESPONSE = 0x0306,     // "spinW" NetSpinWheelPacket: a map prize wheel (manager.spinners) spins

    // 0x0400 Discord link
    NET_EXT_DISCORD_LINK_WAIT = 0x0400,   // "dlnkW" C -> S
    NET_EXT_DISCORD_LINK_RESULT = 0x0401, // "dlnkR" S -> C

    // 0x0500 charge consent (KrunkScript purchases)
    NET_EXT_CHARGE_CONSENT_REQUEST = 0x0500,  // "chreq" S -> C NetChargeConsentRequestPacket: requestId, value, flag
    NET_EXT_CHARGE_CONSENT_RESPONSE = 0x0501, // "chrsp" C -> S NetChargeConsentResponsePacket: requestId[, 1]

    // 0x0600 ranked
    NET_EXT_JOINED_RANK = 0x0600, // "joinedRank" C -> S, no body (v7 key, first used by v10)
    NET_EXT_LATEST_RANKED_MMR_CHANGE = 0x0601, // "lrmr" S -> C NetLatestRankedMmrPacket: the roster player's rating change
    NET_EXT_WARS_CLAN = 0x0602,                // "warsClan" S -> C NetWarsClanPacket: the Turf Wars clan holding this map, or none

    // 0x0700 protocol / ruleset (KrunkNative, the version toggle)
    NET_EXT_BLOCK_PROTOCOL = 0x0700,

    // 0x0800 the player list's actions: vote kick and the K.P.D call
    NET_EXT_VOTE_KICK = 0x0800,       // "v2k" C -> S NetVoteKickPacket
    NET_EXT_VOTE_KICK_STATE = 0x0801, // "vk"  S -> C NetVoteKickStatePacket
    NET_EXT_KPD_CALL = 0x0802,        // "kpd" C -> S NetKpdCallPacket
    NET_EXT_KPD_RESPONSE = 0x0803,    // "kpd" S -> C NetKpdResponsePacket (+ JSON tail)
    NET_EXT_KPD_RESET = 0x0804,       // "kpdf" S -> C, no body: the officer's verdict is open again (kpdData kept)
    NET_EXT_KPD_DATA = 0x0805,        // startGame kpdData S -> C NetKpdDataPacket: the officer's case in this lobby
    NET_EXT_KPD_RATE = 0x0806,        // "rkpd" C -> S NetKpdRatePacket: the officer's verdict on the case
    NET_EXT_ADMIN_ACTION = 0x0807,    // v10 ADMIN_ACTIONS C -> S NetAdminActionPacket: the player list's kick / ban / transfer host
    NET_EXT_KPD_APPLY = 0x0808,       // "kpdA" C -> S NetKpdApplyPacket: a K.P.D application (answer: NET_EXT_KPD_RESPONSE code 6)
    NET_EXT_VOTE_FORFEIT_STATE = 0x0809, // "vf" S -> C NetVoteForfeitStatePacket: the ranked forfeit vote (#voteToForfeit)
    NET_EXT_KPD_LIST = 0x080A,        // "kpd" C -> S NetKpdListPacket: the K.P.D window's Demos (tab 4) / Reviewers (tab 5) list

    // 0x0900 in-game actions of v10 without a NetPacket id
    NET_EXT_MARK_POSITION = 0x0900, // "mpos" C -> S NetMarkPositionSendPacket / S -> C NetMarkPositionPacket (a ping)
    NET_EXT_GAME_STATE = 0x0901,    // "gsc" S -> C NetGameStatePacket: v10 gameState (INITIALIZING .. ENDED), every change
    NET_EXT_VOTE_FORFEIT = 0x0902,  // "vf" S -> C NetVoteForfeitPacket: the team's forfeit vote (team only; null when it ends)
    NET_EXT_INIT_EVENT = 0x0903,    // init [2] eventIndex S -> C NetInitEventPacket: the round's live event, right after the INIT

    // 0x0a00 Prop Hunt (shared/include/prop_hunt.h): Krunker's "up" / "prpR" / "prp" / "prpR" (v10 game.deob.js
    // players.updateProp :171837, the client's iìiiìïí / iîîììîi :221007 / :221019, the keys :221435)
    NET_EXT_PROP_UPDATE = 0x0a00,   // S -> C NetPropUpdatePacket ("up" sid, propID, skip, propSwitches)
    NET_EXT_PROP_ROTATION = 0x0a01, // S -> C NetPropRotationPacket ("prpR" sid, propRot, propRotSaved, reset)
    NET_EXT_PROP_PICK = 0x0a02,     // C -> S NetPropPickPacket ("prp" propID: the wheel / a random prop)
    NET_EXT_PROP_ROTATE = 0x0a03,   // C -> S NetPropRotatePacket ("prpR" [reset]: toggle / reset the rotation)

    // 0x0b00 the live trade of Trade mode (doc/v10/trade.md; bodies in shared/include/live_trade.h). C -> S:
    NET_EXT_TRADE_REQUEST = 0x0b00,  // "strd"  NetTradeRequestPacket (the player list's Trade button; int [5, sid] is NET_PACKET_INTERACT)
    NET_EXT_TRADE_INIT = 0x0b01,     // "trd"   NetTradeInitPacket: [sid, 1] accepts sid's request
    NET_EXT_TRADE_UPDATE = 0x0b02,   // "utrd"  NetTradeUpdatePacket: [skin, action, copyId]
    NET_EXT_TRADE_CURRENCY = 0x0b03, // "utrdc" NetTradeCurrencyPacket: [currency, action, amount]
    NET_EXT_TRADE_ACCEPT = 0x0b04,   // "atrd"  no body
    NET_EXT_TRADE_DECLINE = 0x0b05,  // "dtrd"  no body
    NET_EXT_TRADE_CANCEL = 0x0b06,   // "ctrd"  no body
    NET_EXT_TRADE_CHAT = 0x0b07,     // "tct"   the line, NUL-terminated
    // S -> C:
    NET_EXT_TRADE_RESPONSE = 0x0b10,       // "trd"   NetTradeResponsePacket: types 0 request, 2 canceled, 3 accepted, 4 process, 5 reset
    NET_EXT_TRADE_OPEN = 0x0b11,           // "trd"   type 1: NetTradeChunkHeader + a slice of the JSON [mine, theirs]
    NET_EXT_TRADE_ITEM = 0x0b12,           // "utrd"  NetTradeItemPacket: [ownerSid, add, skin, copyId]
    NET_EXT_TRADE_CURRENCY_STATE = 0x0b13, // "utrdc" NetTradeCurrencyStatePacket: [ownerSid, action, currency, amount]
    NET_EXT_TRADE_RESULT = 0x0b14,         // "atrd"  NetTradeChunkHeader + a slice of the JSON [error, funds, removed, added, meta]
    NET_EXT_TRADE_CHAT_LINE = 0x0b15,      // "tch"   NetTradeChatLinePacket: [name, msg, type, color]
    // 0x0a10 Stalker (shared/include/stalker.h): Krunker's "stlkrt" / "stlkpd" (deob-10.0.3.js:111022, :111363)
    NET_EXT_STALKER_TAUNT = 0x0a10,  // C -> S NetStalkerTauntPacket ("stlkrt" i: the stalker's taunt key, :260109)
    NET_EXT_STALKER_PICKED = 0x0a11, // S -> C, no body (pickStalker's broadcastEx to all but the stalker, :217949)
    NET_EXT_PLAYER_SOUND_FALLOFF = 0x0a12, // S -> C NetPlayerSoundFalloffPacket: "asnd" data.dist / roll, right before its NET_PACKET_PLAYER_SOUND

    // 0x0c00 the in-game captcha: v10.0.3's "cpt" / "cptR" (deob-10.0.3.js:111424-111437; doc/v10/captcha.md). Bodies
    // are text, NUL-terminated: the ALTCHA challenge JSON / the solved payload (base64), each under NET_EXT_BODY_MAX
    NET_EXT_CAPTCHA = 0x0c00,        // "cpt"  S -> C: {algorithm, challenge, maxnumber, salt, signature}
    NET_EXT_CAPTCHA_ANSWER = 0x0c01, // "cptR" C -> S: the payload; S -> C with no body: accepted (v10's _hideCaptcha)

    // 0x8000.. KrunkNative-only
    NET_EXT_BLOCK_KN = 0x8000,

    // 0x8000 live account (KrunkNative): signing in, switching accounts or signing out from the menu
    // during a session re-verifies the live connection, no rejoin (server/src/accounts.c "live sign-in")
    NET_EXT_KN_ACCOUNT_AUTH = 0x8000,   // C -> S NetAccountAuthPacket
    NET_EXT_KN_ACCOUNT_RIGHTS = 0x8001, // S -> C NetAccountRightsPacket

    // 0x8010 KrunkNative gameplay (shared/include/drank.h): the Purple Drank's laced event
    NET_EXT_KN_LACED = 0x8010, // S -> C NetLacedPacket (everyone, the laced player included)

    // 0x8011 bots (KrunkNative): what a spectator following a bot sees of its AI (0x8010 is nettspend's NET_EXT_KN_LACED)
    NET_EXT_KN_BOT_PATH = 0x8011, // S -> C NetBotPathPacket (variable size, NET_BOT_PATH_SIZE(n))

    // 0x8020 official krunker.io sessions (client/src/net/krunker_link.c), client-local: the link
    // queues them into the client's own inbound stream, no server sends them
    NET_EXT_KN_INPUTS_DROPPED = 0x8020, // no body: Krunker's "f" SYNC_FAILED cleared tmpInpts (the
                                        // pending prediction inputs) (game.deob.js:220333)
    NET_EXT_KN_END_TYPE = 0x8021,       // body: u8, the official "end"'s arg0, right before its NET_PACKET_END:
                                        // 1 = show the end screen and its results at once even when we are not
                                        // in the game (a joiner's: v10.js:219715 `2 == a0 || a0 && showEndScreen`)
    // 0x8030 official krunker.io HUD (client/src/net/krunker_link.c -> client/src/net/game_events.c),
    // client-local: the link turns an official packet our own NetPackets cannot carry into one of
    // these for the client's inbound queue; no server sends or receives them, except _COUNTDOWN, which our own
    // server also sends (a wait timer's countdown, shared/src/modes/mode_net.c mode_send_countdown)
    NET_EXT_KN_OFFICIAL_AMMO = 0x8030,         // "uamo" NetOfficialAmmoPacket
    NET_EXT_KN_OFFICIAL_CHAT_I18N = 0x8031,    // "chi" NetOfficialChatI18nHeader + strings
    NET_EXT_KN_OFFICIAL_LEADER_BOARD = 0x8032, // "ulb" NetOfficialLeaderBoardHeader + strings
    NET_EXT_KN_OFFICIAL_ENTER_FAILED = 0x8033, // "fe" FAILED_TO_ENDER: the server's message, NUL-terminated ("" none)
    NET_EXT_KN_OFFICIAL_MEDAL = 0x8034,        // "am" NetOfficialMedalPacket (a text score, sound, count)
    NET_EXT_KN_OFFICIAL_END_ROW = 0x8035,      // "end" NetOfficialEndRowPacket per endData.ed row, ahead of the END
    NET_EXT_KN_OFFICIAL_ROUND_SWITCH = 0x8036, // "rds" Switching Sides, no body: every player inactive, the menu in 3 s
    NET_EXT_KN_OFFICIAL_TEXT_POPUP = 0x8037,   // "tx" / "stlkpd" / "gmsg" [3]: NetOfficialTextPopupHeader + title + sub (NUL-terminated)
    NET_EXT_KN_OFFICIAL_COUNTDOWN = 0x8038,    // "gmsg" [2]: NetOfficialCountdownPacket (cntd_<n> + the big #timerText number)
    // 0x8040 official combat effects (client-local, like 0x8030)
    NET_EXT_KN_OFFICIAL_SHAKE = 0x8040,        // "shk" NetOfficialShakePacket: renderer.shake(amount)
    // 0x8050 ranked match flag (KrunkNative server, client/src/net/game_events.c -> hud_ui_match_gate)
    NET_EXT_KN_RANKED_MATCH = 0x8050, // S -> C NetRankedMatchPacket: 1 in a ranked match (v10 INIT isRanked, the in-match gates)
    // 0x8090 Krunker Strike: the round's commit (S -> C, the player whose commit the brain answered)
    NET_EXT_KN_STRIKE_ROUND = 0x8090,          // "srnd" NetStrikeRoundPacket: the account's Strike XP after the round

    // 0x8100..0x810F reserved for settings-wiring (the v10 settings package; coordinator, 2026-10-15)
    NET_EXT_KN_HIT_DAMAGE = 0x8100, // S -> C NetHitDamagePacket, right before a NET_PACKET_HIT whose damage has a fraction
    // 0x8060 official lobby UI (client/src/net/krunker_link.c / krunker_link_lobby.c, client-local, like 0x8030)
    NET_EXT_KN_OFFICIAL_CLOCK_STATE = 0x8060,  // "t" arg 1, u8: 1 "Next Round in", 2 "MATCH OVER", 3 "MATCH ABANDONED" (#endTimer)
    NET_EXT_KN_OFFICIAL_CHAT = 0x8061,         // "ch" NetOfficialChatHeader + name + text: a line with its tab and colours
    NET_EXT_KN_OFFICIAL_CODE_PROMPT = 0x8062,  // "code" ENTER_CODE, no body: v10's gameCode() password popup
    NET_EXT_KN_OFFICIAL_CODE_TIMER = 0x8063,   // "codeT" UPDATE_CODE_TIMER, int32 seconds: "Submit - Ns"
    NET_EXT_KN_OFFICIAL_CODE_PASSED = 0x8064,  // "codeP" CODE_PASSED, no body: clearPops
    NET_EXT_KN_OFFICIAL_GAME_CODE = 0x8065,    // client -> link only: the typed password, NUL-terminated -> C2S "code" [value]
    // 0x8070 the official socket's editor handoff (client-local, like 0x8030)
    NET_EXT_KN_OFFICIAL_EDIT_MAP = 0x8070,     // "edMp" EDIT_MAP: NetExtBlobChunk + a slice of the map JSON (grabMap's answer)
    // 0x8080 official KrunkScript runtime objects (client-local, client/src/entities.c)
    NET_EXT_KN_OFFICIAL_LIVE_OBJECT = 0x8080,  // "lo" one [id, {type, x, y, z, w, l, h, s, data}] (NetExtJsonChunk; init syncData.lo)
    NET_EXT_KN_OFFICIAL_LIVE_SYNC = 0x8081,    // "slo" one [id, op] ("remove") (NetExtJsonChunk)
    NET_EXT_KN_OFFICIAL_OBJECT_MOVED = 0x8082, // "objmv" NetObjectMovedPacket (init syncData.chg too)

    // 0x8110 Krunker Strike's Tap-to-Play "Switch Teams" (client/src/ui/strike_menu.c, mode_swap_team)
    NET_EXT_KN_SWAP_TEAM = 0x8110, // C -> S NetSwapTeamPacket: the team this player takes between lives (FRVR SWAP_TEAM "swpt")
    // 0x8120 a server line about a player (KrunkNative server and bots -> client/src/net/game_events.c events_server_line)
    NET_EXT_KN_SERVER_LINE = 0x8120, // S -> C NetServerLinePacket: config.serverMessages' join / leave line, the player apart from the text
    NET_EXT_KN_SERVER_NAMES = 0x8121, // S -> C NetServerNamesPacket: a server line naming several players (the chat lists, a finish line)
    NET_EXT_KN_CHAT_NAMES = 0x8122,   // S -> C NetChatNamesPacket: a bot's chat line naming another player (its kill and round comments)
    // 0x8131 Krunker Strike kill card flag (the first id after 0x8130 no queued patch takes)
    NET_EXT_KN_STRIKE_KILL_FLAG = 0x8131, // S -> C NetStrikeKillFlagPacket, right before the victim's NET_PACKET_KILL
    // 0x8140 a guest's Unclaimed Rewards (v10's guest profile: GET /guest/profile, the "ger" push; kn_brain
    // services/brain/guest.c, the server's round credit accounts.c acc_guest_*, client/src/ui/v10_rewards.c)
    NET_EXT_KN_GUEST_ID = 0x8140,     // C -> S NetGuestIdPacket: the menu's guest id, sent while signed out, once per connection
    NET_EXT_KN_GUEST_EARNED = 0x8141, // S -> C, no body: v10's "ger" (GUEST_EARNED_REWARDS): this guest's balance changed, fetch it again
    NET_EXT_KN_RANKED_NOTICE = 0x8142, // S -> C NetRankedNoticePacket: the brain's "queued for ranked" line, the player apart from the text
    // 0x8150 v10.0.3 event kill pools (shared/include/kill_pools.h, doc/v10/kill-pools.md): the END's otherData.items
    NET_EXT_KN_END_ITEM = 0x8150, // S -> C NetEndItemPacket, one per item the match paid, right after the END
    // 0x8160 v10.0.4 counter skins (client/include/skin_counter.h, server/src/skin_counters_server.c)
    NET_EXT_KN_SKIN_COPIES = 0x8160, // S -> C NetSkinCopiesPacket, right after a player's SKINS: each worn copy's stat and seed
    NET_EXT_KN_SKIN_STAT = 0x8161,     // S -> C NetSkinStatPacket: "sks" SKIN_STAT, a server-only stat counted for you
    // 0x8170 v10 video ads (client/include/kn_ad.h: the placeholder ad, doc/v10/ads.md)
    NET_EXT_KN_VIDEO_AD = 0x8170, // S -> C no body: FORCE_VIDEO_AD "vad"; C -> S NetVideoAdPacket: CUSTOM_VIDEO_AD "vad" [state]
} NetExtSubtype;

// NET_EXT_KN_VIDEO_AD (C -> S): forceVideoAd's CUSTOM_VIDEO_AD states (deob-10.0.4.js:226675-226692): 1 asked, 2 shown,
// 3 completed, -1 failed. The server fires KrunkScript's onAdFinished(playerID, success) on 3 (true) and -1 (false).
typedef struct {
    int32_t state;
} NetVideoAdPacket;
_Static_assert(sizeof(NetVideoAdPacket) == 4, "NetVideoAdPacket: the wire body is 4 bytes");

// NET_EXT_KN_SKIN_COPIES (S -> C): what player `uid`'s worn copies carry, in PlayerSkins order (primary, secondary, melee):
// base, a counter skin's copy stat, generateMeshes' counter base m[stat] (deob-10.0.4.js:172726-172728), -1 = no counter
// skin on that slot; seed, the copy's seed for the seeded custom shader 15 (iîîíïiï, :24344-24353: skin_copy_seed), 0 none.
// Sent after every SKINS that has either, so a client sees it before the SPAWN builds the meshes.
typedef struct {
    int32_t uid;
    int32_t base[3];
    int32_t seed[3];
} NetSkinCopiesPacket;
_Static_assert(sizeof(NetSkinCopiesPacket) == 28, "NetSkinCopiesPacket: the wire body is 28 bytes");

// NET_EXT_KN_SKIN_STAT (S -> C, to the counting player only): SKIN_STAT "sks" [weapon, stat, amount] (deob-10.0.4.js:184311),
// handled as countSkinStat(you, weapon, stat, amount) (:221919-221923). stat: the index in SKIN_STATS (shared.h).
typedef struct {
    int32_t weapon;
    int32_t amount;
    uint8_t stat;
    uint8_t pad[3];
} NetSkinStatPacket;
_Static_assert(sizeof(NetSkinStatPacket) == 12, "NetSkinStatPacket: the wire body is 12 bytes");

// NET_EXT_KN_END_ITEM (S -> C): one of the items a public match paid this player from the ongoing events'
// kill pools (v10.0.3 endGame's itemRewards, sent as the END's otherData.items {t: "skin", i, r},
// deob-10.0.3.js:221797-221807, :221896-221900). The server sends them in draw order once the END is out;
// the end screen holds the ones with an icon (r) for its Collect Rewards button. The account already has
// them (the stats commit grants them).
typedef struct {
    int32_t skin;   // i: the store skin index
    char icon[28];  // r: the event's killPoolsIcon (./img/<icon>.webp), "" none (NUL-terminated)
} NetEndItemPacket;
_Static_assert(sizeof(NetEndItemPacket) == 32, "NetEndItemPacket: the wire body is 32 bytes");

// NET_EXT_KN_GUEST_ID (C -> S): the guest id the menu keeps in its own storage: 32 lowercase hex digits
// (16 random bytes), NUL-terminated. It is the guest's identity for the brain's guest_profiles table, as
// v10's cookie is its guest profile: the server keeps it on the connection, credits the guest's END
// reward and score under it, and a live sign-in claims it (zeroes the balance). Never trusted as an
// account: the brain only ever answers for the balance under it.
typedef struct {
    char id[33];
} NetGuestIdPacket;
_Static_assert(sizeof(NetGuestIdPacket) == 33, "NetGuestIdPacket: the wire body is 33 bytes");

// NET_EXT_KN_ACCOUNT_AUTH (C -> S): the identity the player now has in the menu. The server verifies
// it exactly as a JOIN's (a session token through the coordinator's token auth, a "hj1:" through
// hasJoined), never trusting the claim, then re-derives the connection's account, DEV, op and host
// rights and answers with NET_EXT_KN_ACCOUNT_RIGHTS (the same seq). GUEST signs out: the
// connection drops to a guest at once. A client only sends it after this connection's server
// announced the kind in a RIGHTS push (live_kinds); others keep the rejoin (servers_ui.c).
enum {
    NET_ACCOUNT_AUTH_GUEST = 0,      // signed out: back to Guest_N
    NET_ACCOUNT_AUTH_SESSION = 1,    // key = the coordinator session token (brain-handed lobbies only, as JOIN.session)
    NET_ACCOUNT_AUTH_HAS_JOINED = 2, // key = "hj1:" + a fresh client nonce after POST /auth/join, name = the account
};
typedef struct {
    uint8_t kind;  // NET_ACCOUNT_AUTH_*
    uint8_t seq;   // echoed by the answer
    char name[32]; // HAS_JOINED: the account name /auth/join was told
    char key[64];  // SESSION: the token (NetJoinPacket.session's size); HAS_JOINED: "hj1:" + 32 hex
} NetAccountAuthPacket;

// NET_EXT_KN_ACCOUNT_RIGHTS (S -> C): the connection's identity and rights. Pushed once its join
// is settled (status STATE: tells the client this server takes live sign-ins), and as the answer
// to every NET_EXT_KN_ACCOUNT_AUTH. REFUSED / BUSY / UNAVAILABLE changed nothing.
enum {
    NET_ACCOUNT_RIGHTS_STATE = 0,       // push (join settled), seq 0
    NET_ACCOUNT_RIGHTS_OK = 1,          // verified and applied (or signed out)
    NET_ACCOUNT_RIGHTS_REFUSED = 2,     // the token / hasJoined did not verify, or not allowed here (ranked)
    NET_ACCOUNT_RIGHTS_BUSY = 3,        // rate limited, or another verification is still running
    NET_ACCOUNT_RIGHTS_UNAVAILABLE = 4, // this server cannot verify that kind (no coordinator / auth service)
};
#define NET_RIGHTS_DEV 1u  // passes the DEV checks (verified DEV account, op, the server's own machine)
#define NET_RIGHTS_HOST 2u // hosts this custom lobby (premium custom host)
#define NET_RIGHTS_OP 4u   // a config / CLI op
#define NET_RIGHTS_MOD 8u  // a moderator account
#define NET_ACCOUNT_LIVE_SESSION (1u << NET_ACCOUNT_AUTH_SESSION)       // live_kinds: takes session tokens
#define NET_ACCOUNT_LIVE_HAS_JOINED (1u << NET_ACCOUNT_AUTH_HAS_JOINED) // live_kinds: takes hasJoined
typedef struct {
    uint8_t status;     // NET_ACCOUNT_RIGHTS_*
    uint8_t seq;        // the AUTH answered, 0 for a push
    uint8_t rights;     // NET_RIGHTS_*
    uint8_t live_kinds; // NET_ACCOUNT_LIVE_* (always takes GUEST)
    int32_t conn_id;    // the connection (NetInitPacket.client_id): the same one before and after
    int32_t account_id; // 0 guest
    char name[32];      // the connection's name (the next spawn's)
    char message[64];   // why, for REFUSED / BUSY / UNAVAILABLE
} NetAccountRightsPacket;

// NET_EXT_KN_LACED (S -> C, to everyone): the authority rolled a Purple Drank pour-up (reload) as
// laced (shared/src/drank.c drank_pour): `uid` is high for `duration_ms` from now. The client's
// own player gets the distortion post-effect; others may show an indicator. Replaces any spell
// still running (the timer restarts). drank_apply_laced reads it into Player.laced_timer.
#pragma pack(push, 1)
typedef struct {
    int32_t uid;          // the laced player (Player.uid / NetInitPacket client id)
    uint32_t duration_ms; // the spell from now (DRANK: WeaponStream.laced_ms, 8000)
} NetLacedPacket;
#pragma pack(pop)

// NET_EXT_KN_HIT_DAMAGE (S -> C, to the dealer): the exact damage of the NET_PACKET_HIT that follows
// (NetHitPacket.damage is whole: Math.round of changeHealth's dmg.round(1)). v10's DO_DAMAGE "4" carries
// the float, and its dmgType setting draws it as is ("Float", the default) or Math.ceil'd ("Default",
// v10.js:221015). Sent only when the damage has a fraction, so a peer without it shows the whole number.
// game_send_hit (shared/src/game.c) sends the pair; krunker_link.c makes it from "4" for official lobbies.
#pragma pack(push, 1)
typedef struct {
    int32_t target_uid;    // NetHitPacket.target_uid of the HIT it belongs to
    int32_t damage_tenths; // the damage x 10 (dmg.round(1) * 10)
    uint8_t ai;            // NetHitPacket.ai
} NetHitDamagePacket;
#pragma pack(pop)

// NET_EXT_KN_SWAP_TEAM (C -> S, FRVR's SWAP_TEAM "swpt" [team] from Tap-to-Play's Switch Teams, sent
// when its 5 s countdown ends): `team` 1 or 2, the team the player takes. The server accepts it only
// between lives (the player is not alive: the Tap-to-Play screen), in a team mode with selTeam on and
// not in a comp / ranked lobby (mode_swap_team, shared/src/modes/mode_actions.c). A refused request
// changes nothing. An accepted one is broadcast as NET_PACKET_TEAM (the scoreboards); the next spawn
// (Play) is on the new team, as FRVR's Play is.
typedef struct {
    uint8_t team; // 1 or 2
} NetSwapTeamPacket;
// NET_EXT_KN_STRIKE_KILL_FLAG (S -> C, to the victim, Krunker Strike peers only): the killer's Country
// Flag for the Strike kill card. FRVR's killCardInfo.flag is img/flags/3x2/<CODE>.svg from the killer's
// player record (flagType, the account's country); the card is the victim's alone, so only the victim
// gets it. The server's kill hook (server/src/net.c net_game_kill) sends it before the victim's KILL
// (player_kill runs the hook first), so the client holds it when the KILL arrives. `flag` is the
// killer's account stats.flg, an index into client/src/ui/account_flags.h's Country Flag list; -1 for
// none (guests, bots). `killer_uid` is the NetKillPacket.killer_uid it belongs to, so a stale flag is
// ignored. Older peers ignore the subtype.
#pragma pack(push, 1)
typedef struct {
    int32_t killer_uid; // NetKillPacket.killer_uid
    int16_t flag;       // the Country Flag index, -1 none
} NetStrikeKillFlagPacket;
#pragma pack(pop)
_Static_assert(sizeof(NetStrikeKillFlagPacket) == 6, "NetStrikeKillFlagPacket: the wire body is 6 bytes");

// NET_EXT_PLAYER_SPEED_BOOST (S -> C, to the player): Krunker "spb" [speedBoost, silent]
// (game.deob.js:220498): Player.speed_boost (the movement acceleration x it, 0 = none); unless
// silent the chat shows "SpeedBoost - <value>".
// NET_EXT_PLAYER_NAME (S -> C): one rename of Krunker "00" UPDATE_NAMES [[id, name, alias]..., keepOpen]
// (game.deob.js:220322), the player's display name from now on.
#pragma pack(push, 1)
typedef struct {
    float boost;    // speedBoost (the server clamps it to 0.1..10; 0 = none)
    uint8_t silent; // arg 1: no chat line
} NetSpeedBoostPacket;

typedef struct {
    int32_t uid;   // the player's sid
    char name[32]; // PLAYER_NAME_SIZE, NUL-terminated
} NetPlayerNamePacket;

// NET_EXT_PLAYER_MOVE_VALS (S -> C): v10 SYNC_PLAYER's movement-scheme sendVals (deob-10.0.3.js:222398-222403; the
// owner reads them back at :258289-258295 before replaying): the class movSch's state right after input
// `ack_seq`, sent just before the RECON of the same ack and only for a scheme with sendVals (SRM 8 values, Vehicle
// 3; player_movement_send_vals, shared.h). The client applies it with that RECON (client/src/net/recon.c).
typedef struct {
    int32_t ack_seq; // the NetReconPacket.ack_seq it goes with
    int32_t scheme;  // the MovementScheme the values are of
    int32_t count;   // values used (<= PLAYER_MOVE_VALS_MAX)
    float vals[8];   // SRM: onGroundFrames, onWallFrames, wishDash, dashTimer, walldashTimer, tmpVel x, y, z; Vehicle: forwardV, reverseV, angVel
} NetPlayerMoveValsPacket;
_Static_assert(sizeof(NetPlayerMoveValsPacket) == 44, "NetPlayerMoveValsPacket: the wire body is 44 bytes");

// NET_EXT_PLAYER_SYNC (S -> C, no body): the start of one player sync, v10's "k" UPDATE_PLAYERS. Its handler runs
// players.hideAll() first (deob-10.0.3.js:258582, :194883-194894): every other active player whose model was not in
// the previous sync gets forcePos, every one is marked not latestData and hidden; the STATEs of this sync then show
// theirs again (snapping a forcePos one, :258600-258610). A player the sync leaves out (past mode.viewDis, isHidden,
// a K.P.D suspect, :222346) is not drawn until a sync has it again. KrunkNative's server sends it to EXT peers right
// before each sync's STATE batch, an empty sync included (syncState sends "k" with an empty list, :222350); the
// official link turns every "k" into one. Clients of servers that never send it keep drawing every player. The
// client settles each sync at the next marker, as one sync's STATEs may reach it over two frames (main.c
// client_players_hide_all).
//
// NET_EXT_VALUE_TEXT (S -> C): a "cv" CUSTOM_VAL / "gv" UPDATE_GLOBAL_VALUE whose value is a string. v10 keeps either
// type in custVals / globalVals (handlers :258558-258560, :257834-257836); Load Data stores a stored value that is not
// a number as the string (`isNaN(parseFloat(v)) ? v : parseFloat(v)`, :183563), the HUD's text elements show it
// (:177319, :180099) and the conditions compare it as JS does (a number-string comparison: Number(text)). A number
// keeps its NetValuePacket (NET_PACKET_CUSTOM_VALUE / NET_PACKET_GLOBAL_VALUE); peers before EXT get no text.
#define NET_VALUE_TEXT_SIZE 301 // TRIGGER_VALUE_TEXT_SIZE: a stored value (KS_STORAGE_VALUE_BYTES) whole
typedef struct {
    uint8_t type;                   // NET_PACKET_CUSTOM_VALUE ("cv", to the player) or NET_PACKET_GLOBAL_VALUE ("gv", to all)
    uint8_t flags;                  // NET_VALUE_* (NET_VALUE_RESET: a global set cleared first)
    char key[71];                   // TRIGGER_VALUE_KEY_SIZE, NUL-terminated
    char text[NET_VALUE_TEXT_SIZE]; // the value, UTF-8, NUL-terminated
} NetValueTextPacket;
_Static_assert(sizeof(NetValueTextPacket) == 374, "NetValueTextPacket: the wire body is 374 bytes");

// NET_EXT_INIT_EVENT (S -> C): v10 init's [2] eventIndex (game.init, deob-10.0.3.js:219812), which our INIT has no room
// for: sent right after every INIT (KrunkNative's server: Game.event_index; the official link: init [2]). A server that
// never sends it plays no event (0).
typedef struct {
    int32_t event_index; // the event's id, 0 none
} NetInitEventPacket;
_Static_assert(sizeof(NetInitEventPacket) == 4, "NetInitEventPacket: the wire body is 4 bytes");

// NET_EXT_KN_OFFICIAL_AMMO: Krunker "uamo" UPDATE_AMMO [ammos, reserveAmmos] (game.deob.js:220425 ->
// players.updatePlayerAmmo): our player's magazine per loadout slot and, in finiteAmmo modes, its
// reserve per slot. -1 = null in the array (a melee slot: no count).
#define NET_OFFICIAL_AMMO_SLOTS 8
typedef struct {
    uint8_t count;                          // ammos.length (<= NET_OFFICIAL_AMMO_SLOTS)
    uint8_t reserve_count;                  // reserveAmmos.length, 0 = none sent
    int32_t ammo[NET_OFFICIAL_AMMO_SLOTS];
    int32_t reserve[NET_OFFICIAL_AMMO_SLOTS];
} NetOfficialAmmoPacket;

// NET_EXT_KN_OFFICIAL_SHAKE: Krunker "shk" SHAKE [amount] (renderer.shake, game.deob.js 10.0.3; the
// server already scaled it by distance, game.rumble): the camera shake our camera_feel_shake keeps the max of
typedef struct {
    float amount;
} NetOfficialShakePacket;

// NET_EXT_KN_STRIKE_ROUND (S -> C, Krunker Strike): the account's Strike XP after the round's commit and
// the XP the round gave (kn_brain strike_econ_commit's xp / xpGained; 0 for a duplicate commit). The round
// end's XP card, XP bar and level-up (FRVR's playerEndData XP rewards) read it.
typedef struct {
    int32_t xp;
    int32_t xp_gained;
} NetStrikeRoundPacket;

// NET_EXT_KN_OFFICIAL_CHAT_I18N: Krunker "chi" ADD_CHATI18N [team, name, key | [key, ...args], kind,
// color] (game.deob.js:221142): addChat with i18n.t(key, ...args), translated on the client's main
// thread (i18n is its). The header is followed by `argc + 2` NUL-terminated UTF-8 strings: the
// sender name ("" none), the key, then the arguments (numbers in JS String() form).
typedef struct {
    uint8_t kind;     // NetChatKind of the line (Krunker 1 / 4 game, 2 server, 0 player)
    uint8_t argc;     // i18n arguments
    char color[16];   // CSS colour of a kind-2 line ("" = the default #fc03ec)
    int8_t channel;   // arg 0 team: the chat tab (0 global, 1 team; the HUD forces -1 outside team modes)
    char name_color[16]; // arg 5 nameColor of a player line's name ("" none)
} NetOfficialChatI18nHeader;

// NET_EXT_KN_OFFICIAL_CHAT: Krunker "ch" ADD_CHAT [team, name, text, kind, color, nameColor, senderAccountId,
// extra] (iíïíííï, game.deob.js:221172): the line on its chat tab (data-tab = team || 0; -1 outside team modes),
// a kind-2 line in `color` (default #fc03ec), a player line's name in nameColor. Followed by the sender's name and
// the text, NUL-terminated. Not carried: extra.actions (v10's only one, "Join Queue", needs its ranked queue).
typedef struct {
    int8_t channel;      // team arg: 0 global, 1 team, -1 every tab
    uint8_t kind;        // NetChatKind (Krunker 0 player, 2 server, 1 / 4 game)
    int32_t account_id;  // the sender's account (the mute list), 0 none
    char color[16];      // kind 2: the line's colour ("" = #fc03ec)
    char name_color[16]; // kind 0: the name's colour ("" none)
} NetOfficialChatHeader;

// NET_EXT_KN_OFFICIAL_LEADER_BOARD: Krunker "ulb" UPDATE_LEADER_BOARD [index, [player, value, ...]]
// (game.deob.js:220065 -> map.manager.updatePendingLeader): the rows of a map's LEADERBOARD object.
// The header is followed by `cells` NUL-terminated strings (player, value pairs).
typedef struct {
    int32_t index;    // the leaderboard (pendingLeaders key: the map's n-th LEADERBOARD object)
    uint8_t cells;    // strings that follow
} NetOfficialLeaderBoardHeader;

// NET_EXT_KN_OFFICIAL_MEDAL: one Krunker "am" ADD_MEDAL entry [name | medal index, score, sound, count]
// (iîíìiïì :220960): v10 shows the score as given, so the server's ["Reward", "12 KR", 0, 1] reads
// "12 KR" under "Reward" (NET_PACKET_MEDAL's score is a number); count stacks a medal already up.
typedef struct {
    char text[32];    // the medal's name (overlay.medalsList[index] for an index)
    char score[24];   // "" none; a number as "+N" / "-N"
    uint8_t sound;    // medal_<sound>
    uint8_t count;    // >= 1
} NetOfficialMedalPacket;

// NET_EXT_KN_OFFICIAL_END_ROW: one endData.ed row of an official "end" (game.deob.js:219871-219890):
// v10 prints every row, a player who already left by the row's own [name, alias] cell, so the END's
// board (which our client resolves to its players) is completed by these. Values are the row's cells.
typedef struct {
    int32_t uid;
    char name[32];    // the name cell: alias || name
    int32_t score, kills, deaths;
    int32_t dmg;      // the "dmg" cell (Hardpoint / Raid endStats), -1 none
    int32_t reward;   // the reward cell's "+N" (0 = "NONE")
} NetOfficialEndRowPacket;

// NET_EXT_KN_OFFICIAL_TEXT_POPUP: v10's textPopup(title, sub, silent, subSize) (game.deob.js iìîíììì
// :220929): infoText / infoSub on the overlay; followed by the title and the sub, NUL-terminated
typedef struct {
    uint8_t silent;   // no pop_3
    uint8_t sub_size; // infoSub's font size, 0 = 28
} NetOfficialTextPopupHeader;

// NET_EXT_KN_OFFICIAL_COUNTDOWN: "gmsg" arg 2 (iíïììií :220406): cntd_<n> at 0.2 and timerText's big n
typedef struct {
    int32_t n;
} NetOfficialCountdownPacket;
#pragma pack(pop)
// NET_EXT_KN_BOT_PATH (S -> C): the spectated bot's AI state for the show_navmeshes overlay
// (client/src/nav_overlay.c) and the spectator HUD's state line (hud_spectate.c). The server
// sends it only to a spectator following a bot (NET_PACKET_SPECTATE_TARGET), for that bot,
// NET_BOT_PATH_INTERVAL_S apart (server/src/net.c net_broadcast_bot_paths), from
// bot_path_current() / bot_path_goal() (shared/include/bot_path.h) and the bot's target memory.
// The body is the struct cut after the last point: NET_BOT_PATH_SIZE(n) bytes
// (shared/src/bot_path_net.c fills and decodes it). Positions are feet, world units.
#define NET_BOT_PATH_POINTS_MAX 28        // 60-byte head + 28 x 16 = 508 <= NET_EXT_BODY_MAX
#define NET_BOT_PATH_STATE_SIZE 12        // state_name: bot_state_name() ("Hunt", "Attack", ...)
#define NET_BOT_PATH_INTERVAL_S 0.2f      // the server's send interval per spectator
#define NET_BOT_PATH_HAS_GOAL 1u          // flags: goal[] is where the bot is heading
#define NET_BOT_PATH_HAS_TARGET 2u        // target_sid / target[]: the bot has a target player
#define NET_BOT_PATH_SEES_TARGET 4u       // ... and has line of sight to it right now
#define NET_BOT_PATH_HAS_LAST_KNOWN 8u    // last_known[]: where it last saw its target
#define NET_BOT_PATH_PARTIAL 16u          // the route does not reach the goal
#define NET_BOT_PATH_STRANDED 32u         // the goal is unreachable: the bot waits for a new one
#define NET_BOT_PATH_TRUNCATED 64u        // the route has more waypoints than fit (NET_BOT_PATH_POINTS_MAX)
#define NET_BOT_PATH_ACTIVE 128u          // the bot's player is alive (dead: state only, no points)
typedef struct {
    float x, y, z;       // feet
    uint8_t kind;        // BotPathKind of the segment ending here (0 for point 0)
    uint8_t flags;       // BOT_PATH_FLAG_* of that segment
    uint8_t reserved[2];
} NetBotPathPoint;
typedef struct {
    int32_t bot_sid;     // the bot's Player uid (the spectator's target)
    int32_t target_sid;  // its target's uid, -1 none
    uint8_t state;       // BotState
    uint8_t flags;       // NET_BOT_PATH_*
    uint8_t n;           // points that follow: 0 = no route; else point 0 is the bot's feet
    uint8_t reserved;
    char state_name[NET_BOT_PATH_STATE_SIZE]; // bot_state_name(state), so new states need no client change
    float goal[3];       // NET_BOT_PATH_HAS_GOAL
    float target[3];     // NET_BOT_PATH_HAS_TARGET: the target's feet when sent (the client follows the live player)
    float last_known[3]; // NET_BOT_PATH_HAS_LAST_KNOWN
    NetBotPathPoint points[NET_BOT_PATH_POINTS_MAX];
} NetBotPathPacket;
#define NET_BOT_PATH_SIZE(n) (offsetof(NetBotPathPacket, points) + (size_t) (n) * sizeof(NetBotPathPoint))

// ---- 0x0800 the player list's actions (client/src/ui/hud_votekick.c, server/src/votekick.c)
#pragma pack(push, 1)
//
// NET_EXT_VOTE_KICK (C -> S, v10 "v2k" [alias, 1]): `vote` 1 opens a vote against `target` (an
// alias as the player list shows it) or votes yes on the running one, 0 votes no (the [2] key).
// The server's rules (v10 config: voteKickReq 4, voteKickTimer 35 s, voteKickBan 30 min,
// voteKickMaxLvl 20) are server/src/votekick.c's.
typedef struct {
    char target[32];
    uint8_t vote;
} NetVoteKickPacket;

// NET_EXT_VOTE_KICK_STATE (S -> C, v10 "vk" [alias, votes, needed, isNew, voter]): the running
// vote (active 1; opened 1 on the broadcast that opened it, with the initiator in `voter`), or
// active 0 = the vote ended (passed or failed: v10 "vk" null): the prompt hides.
// NET_EXT_VOTE_FORFEIT_STATE (S -> C, v10 "vf" [voter, votes, needed, isNew, initiator, updateOnly],
// game.deob.js iíìiíìí :220002; ranked, signed-in players only): active 0 = the vote ended ("vf" null:
// #voteToForfeit hides). opened: "<initiator> initiated vote"; update_only: the count only; otherwise
// `voter` voted (us: the "Vote Submitted - a/b" line instead of the box).
typedef struct NetVoteForfeitStatePacket {
    uint8_t active;
    uint8_t opened;
    uint8_t update_only;
    int32_t votes;
    int32_t needed;
    char voter[32];
    char initiator[32];
} NetVoteForfeitStatePacket;

typedef struct NetVoteKickStatePacket {
    uint8_t active;
    uint8_t opened;
    uint8_t passed;    // with active 0: 1 the target was kicked, 0 the vote failed / timed out
    int32_t votes;
    int32_t needed;
    char target[32];
    char voter[32];
} NetVoteKickStatePacket;

// NET_EXT_KPD_CALL (C -> S, v10 "kpd" [tab, {id}]): tab 0 calls the K.P.D on player `uid`
// (a Game uid); tabs 1.. ask for the K.P.D window's lists (1 distress calls, 2 the moderation
// log, 3 tag stats: moderators only).
typedef struct {
    uint8_t tab;
    int32_t uid;
} NetKpdCallPacket;

// NET_EXT_ADMIN_ACTION (C -> S, v10 "ADMIN_ACTIONS" [action, id, extraBan] from the player list's
// Kick, Ban and Transfer Host buttons): action 0 kicks player `uid` (a Game uid: a DEV, the custom
// host or a moderator), 1 bans it (a DEV only: its account, when it has one, from this server's
// bans), 8 makes it the custom game's host (the host only). The server checks the sender's right on
// every request and that the target is a player here other than the sender (server/src/votekick.c);
// the client's own check only shows the button.
// Other actions (kill, tp2, tpme, ...) stay chat commands. A refused request changes nothing and
// the sender gets a Server line.
typedef struct {
    int32_t uid;
    uint8_t action; // 0 kick, 1 ban, 8 transfer host (v10.0.3 punishPlayer's action, deob-10.0.3.js:254059-254071)
} NetAdminActionPacket;

// NET_EXT_KPD_RESPONSE (S -> C, v10 "kpd" [code, message, data]): code 0 answers a call
// (status: NET_KPD_* with the message to show and the suspect's name), code 6 an application
// (NET_EXT_KPD_APPLY, KrunkNative); codes 1..3 carry a list
// for the K.P.D window as a JSON array after the struct (the rest of the body, not 0-terminated). A
// list longer than one packet comes in pages, each a whole array with `name` "page/pages" (1-based).
enum {
    NET_KPD_REPORTED = 0,   // "Reported <name> to the K.P.D"
    NET_KPD_BUSY = 1,       // already reported / cooling down / rate limited: `message` says why
    NET_KPD_REFUSED = 2,    // not allowed (level, guest, the target)
    NET_KPD_UNAVAILABLE = 3 // no moderation backend on this server
};
typedef struct NetKpdResponsePacket {
    uint8_t code;
    uint8_t status;
    char message[64];
    char name[32];
} NetKpdResponsePacket;

// NET_EXT_KPD_DATA (S -> C, v10 startGame's kpdData {sus, caller}): a K.P.D officer (a moderator or
// DEV account) joined a lobby with an open call: `sus` is the suspect's uid (the player list's red
// row, the spectator panel "Is <name> hacking?"), `caller` the reporter's (-1 left). sus -1: no case.
typedef struct NetKpdDataPacket {
    int32_t sus;
    int32_t caller;
} NetKpdDataPacket;

// NET_EXT_KPD_APPLY (C -> S, v10 "kpdA" [discord, reason] from kpdSubmit): an account of level 50 or more,
// not flagged and not an officer, applies to the K.P.D; kn_brain keeps it (kpd_applications) when the
// reporter has 20 positive reports and a rating above 0.7. The answer is NET_EXT_KPD_RESPONSE code 6
// (status NET_KPD_REPORTED: received, else refused with the message). Strings 0-terminated.
typedef struct {
    char discord[40];
    char reason[300];
} NetKpdApplyPacket;

// NET_EXT_KPD_LIST (C -> S, v10 kpd.listDemos {off, q} / kpd.reviewers()): an officer's Demos (tab 4)
// or Reviewers (tab 5) list. off: the page's first suspect group (0, then 10, 20 ... for Prev / Next);
// q: the player name of the search (empty: all). The answer is NET_EXT_KPD_RESPONSE code 4 (demos,
// data {rows, more}) or code 5 (reviewers, data {month, all}); a non-officer is refused (code 4 / 5).
typedef struct {
    uint8_t tab;   // 4 Demos, 5 Reviewers
    int32_t off;   // the Demos page (suspect groups)
    char q[24];    // the Demos search (player name), 0-terminated
} NetKpdListPacket;

// NET_EXT_KPD_RATE (C -> S, v10 "rkpd" [tagged]): the officer's verdict, 1 tagged (hacking: [Y] twice,
// "Are you 100% sure?"), 0 not tagged ([N]). The server records it on the call (kn_brain kpd_rate:
// kpd_calls.status handled, .rating) once the officer watched for kpdWatchTime (10 s).
typedef struct {
    uint8_t tagged;
} NetKpdRatePacket;

// NET_EXT_MARK_POSITION, v10's ping (ranked / comp: markPositionKey, M2): C -> S the camera ray's first
// map hit (tmpPlayer.markPosition :170208, range 1000); S -> C addPingIcon(key, position) (:160954):
// one marker per key (the pinging player's id), drawn 4 s at position.y + 2 (img/ping.png).
// NET_EXT_GAME_STATE (S -> C, v10 "gsc" [gameState]): the match phase, a GameState value (shared.h).
typedef struct {
    uint8_t state;
} NetGameStatePacket;

// NET_EXT_VOTE_FORFEIT (S -> C, v10 "vf" [initiator | null, num, req, isStart, initiatorName, 1]): the
// forfeit vote of the recipient's team (server/src/forfeit.c). active 0 is v10's "vf" null (the panel
// hides). opened 1 is the broadcast that opened the vote (the panel names the initiator); the rest
// are the counts (votes / needed) after a vote or a change of the eligible voters.
typedef struct {
    uint8_t active;
    uint8_t opened;
    uint8_t team;
    uint8_t reserved;
    int32_t votes;
    int32_t needed;
    char initiator[32];
} NetVoteForfeitPacket;
typedef struct {
    float x, y, z;
} NetMarkPositionSendPacket;
typedef struct {
    char key[48];
    float x, y, z;
} NetMarkPositionPacket;

// NET_EXT_PROP_UPDATE: a player's prop. skip (Krunker's "up" arg 2): the prop is not applied (only the
// switches); switches -1 = none sent (Krunker null); limit: the server refused a pick (no switches left),
// "[Prop Hunt] You are out of prop switches" for that player
typedef struct {
    int32_t uid;
    int16_t prop_id;  // prefab id of a PF_ISPROP prefab
    uint8_t skip;
    int8_t switches;  // player.propSwitches, -1 = not sent
    uint8_t limit;
} NetPropUpdatePacket;

// NET_EXT_PROP_ROTATION: propRot (the model follows the look), propRotSaved (the yaw it keeps while
// rotation is off; saved_set 0 = null: yaw 0), reset (the "Rotation reset." line)
typedef struct {
    int32_t uid;
    uint8_t rot;
    uint8_t saved_set;
    uint8_t reset;
    float saved;
} NetPropRotationPacket;

typedef struct {
    int16_t prop_id;
} NetPropPickPacket;

typedef struct {
    uint8_t reset; // 1: reset ("prpR", true), 0: toggle
} NetPropRotatePacket;

// NET_EXT_KN_RANKED_MATCH (S -> C): sent to each EXT peer right after its INIT (the client's game is reset there),
// so the in-match UI follows the lobby: the Invite button greyed, the spectate button hidden and the comp box titled
// "Krunker Ranked" (v10 initGame isRanked, g:219126-219156 and g:219431).
typedef struct {
    uint8_t ranked; // 1 in a ranked match
} NetRankedMatchPacket;

// NET_EXT_KN_SERVER_LINE (S -> C, EXT peers): a server chat line about one player, config.serverMessages[0 / 1]
// ("$ joined the game" / "$ left the game": a player's first spawn and its leave, bots included) with the player apart
// from the template, so the client formats the line itself and shows the name the way it shows that player
// everywhere else (Hide player names, client/include/player_names.h: Player_<n> by uid). Krunker v10.0.2 sends these
// as "chi" [i18n key, name] for the same reason (its client formats i18n.t(key, name)). The first "$" of `text`
// stands for `name`; a template without one shows as it is. A pre-EXT peer gets the composed NET_CHAT_SERVER line
// (NET_PACKET_CHAT, uid -1, no name) as before: the server picks per peer (server/src/net.c net_send_server_line_to).
typedef struct {
    int32_t uid;          // the player the line is about (its SPAWN uid), -1 unknown
    char name[32];        // PLAYER_NAME_SIZE: its name as this recipient may see it (a ranked warmup masks the others' names)
    char text[288];       // NET_CHAT_TEXT_SIZE: the template, "$" = the name; NUL-terminated
} NetServerLinePacket;

// NET_EXT_KN_SERVER_NAMES (S -> C, EXT peers): a server chat line that names players, as NET_EXT_KN_SERVER_LINE does for
// one: the chat lists ("Players (N): ...", "Killed N: ...", "Red (N): ..."), the bots' list, a finish line, a line
// with two names. Each name travels apart from `text` with its uid, and the client formats the line: the i-th "$" of
// `text` stands for names[i], shown as Hide player names shows it (player_names_shown by uid), and the names are
// never searched for in the text, so a name equal to a word of the template garbles nothing. uid: the player's uid;
// -1 a player with no uid yet (numbered by its name, as an official server's line); -2 not a player (an AI, the
// server's own text): shown as it is. Lists longer than NET_SERVER_NAMES_MAX names go out as several lines. A pre-EXT
// peer gets the composed NET_CHAT_SERVER line (server/src/net.c net_send_server_names_to).
#define NET_SERVER_NAMES_MAX 8     // names per line (count is a u8; 8 x 36 + the text fits NET_EXT_BODY_MAX)
#define NET_SERVER_NAMES_TEXT 200  // the template's bytes, NUL included
#define NET_SERVER_NAME_NONE (-1)  // a player with no uid yet
#define NET_SERVER_NAME_RAW (-2)   // not a player: its name is shown as it is
typedef struct {
    int32_t uid;   // the player (NET_SERVER_NAME_NONE / NET_SERVER_NAME_RAW as above)
    char name[32]; // PLAYER_NAME_SIZE: its name as this recipient may see it
} NetServerNameEntry;
typedef struct {
    uint8_t count;                                // names used, 0..NET_SERVER_NAMES_MAX
    NetServerNameEntry names[NET_SERVER_NAMES_MAX];
    char text[NET_SERVER_NAMES_TEXT];             // the template, NUL-terminated; "$" = names[i] in order
} NetServerNamesPacket;

// NET_EXT_KN_CHAT_NAMES (S -> C, EXT peers): a bot's chat line naming another player (the kill and round comments, "{o}"
// -> "$"). The line is the bot's own, shown by its uid as any chat line is (sender); the other player's name travels apart
// from `text` by its uid, and the client formats the template as NET_EXT_KN_SERVER_NAMES does (the i-th "$" is names[i]).
// A pre-EXT peer gets the bot's composed NET_PACKET_CHAT line (server/src/net.c net_send_chat_names_to).
#define NET_CHAT_NAMES_MAX 1 // names in a bot's line: its one "{o}" (the body limit NET_EXT_BODY_MAX holds one)
typedef struct {
    NetServerNameEntry sender;                    // the bot: its uid and its name
    uint8_t count;                                // names used, 0..NET_CHAT_NAMES_MAX
    NetServerNameEntry names[NET_CHAT_NAMES_MAX];
    char text[NET_SERVER_NAMES_TEXT];             // the template, NUL-terminated; "$" = names[i] in order
} NetChatNamesPacket;

// NET_EXT_LATEST_RANKED_MMR_CHANGE (S -> C, to each roster player when the brain's rating reply arrives; v10 lrmr
// LATEST_RANKED_MMR_CHANGE [mmrChange, latestRank], g:218186 sets $latestMMRChange / $latestRank, the end screen's
// rank card reads them): mmr_change = the new rating - the old (the TrueSkill display rating, 0 when it did not move),
// latest_rank = the rank after the match (0 while placing: no card, v10 shows it only for latestRank > 0), rp_change =
// the ranked points the match gave (KrunkNative's: v10 shows them from its balance update, which KN does not send).
typedef struct {
    int32_t mmr_change;
    int32_t latest_rank;
    int32_t rp_change;
} NetLatestRankedMmrPacket;
#pragma pack(pop)

// NET_EXT_GAME_ANNOUNCEMENT (S -> C): v10's GAME_ANNOUNCEMENT ("gameAnnouncement" {message, target,
// trackId, bundleId, displayIds, actions}, g:210119) as KrunkNative sends it: the message (UTF-8,
// NUL-terminated; "" = no message) and, when ranked_queue is 1, v10's "ranked-queue" action with the
// server's label (ranked_label, NUL-terminated). Nothing else of v10's announcement is sent (no bundle,
// shop display, other action, target). server_bin sends the current one to each player whose join
// settles and to everyone when the console's `announce` / `announce ranked-queue` changes it (server/src/net.c).
#define NET_GAME_ANNOUNCEMENT_TEXT 256
#define NET_GAME_RANKED_LABEL 64
typedef struct {
    char message[NET_GAME_ANNOUNCEMENT_TEXT];
    uint8_t ranked_queue;                    // 1: the card has the ranked-queue action
    char ranked_label[NET_GAME_RANKED_LABEL];
} NetGameAnnouncementPacket;

// NET_EXT_KN_RANKED_NOTICE (S -> C, EXT peers): the brain's "queued for ranked" line (NET_CHAT_RANKED's green), the queued
// player apart from `text` ("$" = name, or no "$" for a line with no name). The brain knows the account, not the in-game uid
// (a queue starts before any match), so the name has no uid: the client numbers it by name as an official line's name is
// numbered (player_names_shown with uid -1), and never shows it raw while names are hidden. A pre-EXT peer gets the
// composed NET_CHAT_RANKED line with the name (server/src/net.c net_broadcast_ranked_notice).
typedef struct {
    char name[32];   // PLAYER_NAME_SIZE: the queued player's name, NUL-terminated ("" for no name)
    char text[288];  // NET_CHAT_TEXT_SIZE: the template, NUL-terminated
} NetRankedNoticePacket;

// Bodies fit one NetPacket (NET_MAX_PAYLOAD 512) after the subtype.
#define NET_EXT_BODY_MAX (512 - NET_EXT_HEADER_SIZE)
_Static_assert(sizeof(NetServerLinePacket) <= NET_EXT_BODY_MAX, "NetServerLinePacket exceeds NET_EXT_BODY_MAX");
_Static_assert(sizeof(NetServerNamesPacket) <= NET_EXT_BODY_MAX, "NetServerNamesPacket exceeds NET_EXT_BODY_MAX");
_Static_assert(sizeof(NetChatNamesPacket) <= NET_EXT_BODY_MAX, "NetChatNamesPacket exceeds NET_EXT_BODY_MAX");
_Static_assert(sizeof(NetRankedNoticePacket) <= NET_EXT_BODY_MAX, "NetRankedNoticePacket exceeds NET_EXT_BODY_MAX");

#pragma pack(push, 1)
// The JSON-bodied subtypes (entities 0x0100-0x0105, live objects 0x8080-0x8081): one UTF-8 JSON
// array per record, split over consecutive EXTs of the same subtype when longer than one body
// (an entity's state alone may be 512 bytes): part 0..parts-1 in order, nothing in between.
typedef struct {
    uint8_t part;  // this slice's index
    uint8_t parts; // slices of the record (>= 1)
} NetExtJsonChunk;
#define NET_EXT_JSON_CHUNK_DATA (NET_EXT_BODY_MAX - (int) sizeof(NetExtJsonChunk))

// NET_EXT_PLAYER_SOUND_FALLOFF: the 3D falloff of v10's addPlayerSound data ({vol, rate, dist, roll, loop},
// game.addPlayerSound deob-10.0.3.js:220423-220437; the stalker's taunt sends dist 20, roll 1.1 :198335-198340),
// which NetPlayerSoundPacket has no room for. Sent right before the NET_PACKET_PLAYER_SOUND add of the same
// player (the reliable stream keeps them in order); the client plays that sound with this refDistance /
// rolloffFactor (Howler's pannerAttr) instead of its defaults (25 / 1). A peer without EXT plays the defaults.
typedef struct {
    int32_t uid;    // the player of the PLAYER_SOUND that follows
    float distance; // data.dist: refDistance
    float rolloff;  // data.roll: rolloffFactor
} NetPlayerSoundFalloffPacket;

// NET_EXT_PLAYER_LOCKS: v10 "plk" PLAYER_LOCKS [disableShooting, disableMelee] (iìïîììî :220492):
// our player's KrunkScript locks (player.disableShooting / disableMelee), until the next "plk"
typedef struct {
    uint8_t disable_shooting;
    uint8_t disable_melee;
} NetPlayerLocksPacket;

// NET_EXT_PLAYER_POSE: v10 "pz" PLAYER_POSE [sid, pose || 0] (game.setPlayerPose deob-10.0.3.js:221041,
// client players.setPose :195354): the player's sanitized pose (shared/include/player_pose.h), `has`
// 0 = pose 0 (cleared). Our server also sends it after a posed player's SPAWN (v10's init plrPose).
typedef struct {
    int32_t uid;          // sid
    uint16_t has;         // PLAYER_POSE_* keys present
    uint8_t legs, weapon, bob, face_ride, hold, hold_l;
    float leg_x, body_y, torso_x;
    float arm_l[3], arm_r[3];
} NetPlayerPosePacket;

// NET_EXT_KN_OFFICIAL_OBJECT_MOVED: v10 "objmv" OBJECT_MOVED [uid, x, y, z] (game.moveObject :187893):
// a dyn map object moved; y is the GameObject's centre (bottom + h / 2); riders move with it
typedef struct {
    int32_t uid;
    float x, y, z;
} NetObjectMovedPacket;

// NET_EXT_PLAYER_ANIM (S -> C): v10 "c" PLAYER_ANIM [sid, clipIndex] (iíîiiìï, deob-10.0.3.js:257769 -> players.playClip
// :193806): the player's class model plays clip `clip` of NET_PLAYER_CLIP_* (the clip list :21165) `reps` times (the
// handler's arg 2 || 1; the server sends two arguments). Only a player whose class has an animated model plays clips
// (hasAnims = config.aID holds "g", "q" or "f", :194877 / :195127; the model is config.mdl 2's loadAnimMesh, :194251).
// The authority sends it (playClip's isServer branch :193808-193815) for "Jump" (players.jump, not a recon :195271) and
// "Melee" (players.melee :195909) to every spectator and to every other active player within mode.viewDis || 600 of
// the player (shared/src/player_anim.c); never to the player itself (its own client plays them locally).
enum {
    NET_PLAYER_CLIP_IDLE, NET_PLAYER_CLIP_MOVE, NET_PLAYER_CLIP_MOVE_FAST, NET_PLAYER_CLIP_JUMP,
    NET_PLAYER_CLIP_MELEE, NET_PLAYER_CLIP_SHOOT, NET_PLAYER_CLIP_CROUCH, NET_PLAYER_CLIP_COUNT
};
typedef struct {
    int32_t sid;   // the player (its uid on the KrunkNative wire)
    uint8_t clip;  // NET_PLAYER_CLIP_*
    float reps;    // repetitions: v10's playClip(player, name, arg2 || 1)
} NetPlayerAnimPacket;

// NET_EXT_PLAYER_ASSET_ID (S -> C): v10 "uai" UPDATE_ASSET_ID [sid, assetTxt] (iíiïiiï, deob-10.0.3.js:257784 ->
// players.updateAssetID :197964): `if (config.mdl && assetTxt.length)` the player's next model build loads that asset
// instead of the class's aID (player.assetTxt, nulled by generateMeshes :194265), and an active player is rebuilt at
// once (regenMeshes). The authority broadcasts it from updateAssetID (KrunkScript's `player.assetID = ...` setter,
// :207883-207887; shared/src/player_anim.c player_update_asset_id; one made in a spawn's onPlayerSpawn goes out after
// that SPAWN). The client drops it for a sid it has not added (findBySid) and keeps it in player.assetTxt until the
// next generateMeshes takes it (client/src/player_class_model.c).
#define NET_PLAYER_ASSET_TEXT 32
typedef struct {
    int32_t sid;                       // the player (its uid on the KrunkNative wire)
    char asset[NET_PLAYER_ASSET_TEXT]; // assetTxt (`+= ""`), NUL-terminated, cut to 31 bytes
} NetPlayerAssetIdPacket;

// NET_EXT_SPEECH_BUBBLE (S -> C): v10 "sb" SHOW_SPEECH_BUBBLE [html, ms, sigh] (iíììiîì, deob-10.0.3.js:261181): the menu
// mascot's bubble (#txtBubble's innerHTML, #bubbleContainer scale(1.0), back to scale(0.0) after ms || 2500), a sigh_0 /
// sigh_1 at volume 0.2 first when sigh. v10 sends ["sb", "welc", username] after "ready" (:261464); the official game
// server answers it. KrunkNative's own server sends none (its welcome text is the official server's, not in the bundle).
#define NET_SPEECH_BUBBLE_TEXT 256
typedef struct {
    uint32_t ms;                       // how long it shows, 0 = 2500
    uint8_t sigh;                      // play a sigh first
    char text[NET_SPEECH_BUBBLE_TEXT]; // the server's HTML, NUL-terminated (the client shows its text, tags dropped)
} NetSpeechBubblePacket;

// NET_EXT_PLAYER_INTER_PROG (S -> C): v10 "p" INTER_PROG [prog] (iìììîíí, deob-10.0.3.js:259394) and "mp" INTER_M_PROG
// [prog] (iiííïïi :259398): the bars of the player a spectator follows, interactProg (an interact hold, #interactProg)
// and interactMProg (a weapon charge, the game overlay's upright bar). display = prog > 0. The authority sends them
// from its update loop (:222274-222292) to the connections whose specStream is that player, (prog || 0).round(3),
// when the value changed. KrunkNative's server keeps the last value per spectator (server/src/net.c), so a spectator
// switching to another player gets that player's bar at once; v10 keeps it per player and waits for its next change.
enum { NET_INTER_BAR_INTERACT, NET_INTER_BAR_CHARGE, NET_INTER_BAR_COUNT };
typedef struct {
    uint8_t bar;  // NET_INTER_BAR_*
    float prog;   // 0..1 (a charge past 1 is an ovrChrg weapon's over-charge), 0 hides the bar
} NetInterProgPacket;
_Static_assert(sizeof(NetInterProgPacket) == 5, "NetInterProgPacket: the wire body is 5 bytes");

// NET_EXT_UNBOX_MESSAGE (S -> C): v10 "unb" UNBOX_MESSAGE [name, itemIndex, kr] (iíïîiìí, deob-10.0.3.js:254321) and
// "unbS" UNBOX_MESSSAGE_S [playerId, skinIndex] (iíiíïíi :254331): with Show Unboxings on, the chat line
// "<span style='color:#fff'>{name}</span> unboxed <span class='click' style='color:{rarity colour}'>[{item}]</span>"
// (i18n windows.store.unboxed). unb names the unboxer itself; a kr > 0 is a Free KR spin: item = store.freeKR[itemIndex]
// ({rarity}, name kr + " KR", no viewItem). unbS names a player of the game (players.findById); the server sends it when a
// map prize wheel stops (updateSpinners :219471-219476: everyone for rarity >= 2, else the spinner alone).
enum { NET_UNBOX_NAMED, NET_UNBOX_PLAYER };
#define NET_UNBOX_NAME_SIZE 32
typedef struct {
    int32_t uid;                    // NET_UNBOX_PLAYER: the player's uid; NET_UNBOX_NAMED: -1
    int32_t item;                   // store.skins index, or store.freeKR index when kr > 0
    int32_t kr;                     // the KR a Free KR spin won, 0 = a skin
    uint8_t kind;                   // NET_UNBOX_*
    char name[NET_UNBOX_NAME_SIZE]; // NET_UNBOX_NAMED: the unboxer's name (NUL-terminated)
} NetUnboxMessagePacket;
_Static_assert(sizeof(NetUnboxMessagePacket) == 45, "NetUnboxMessagePacket: the wire body is 45 bytes");

// NET_EXT_LOTTERY_PRIZE_WON (S -> C): v10 "lotWin" LOTTERY_PRIZE_WON [lotteryId, entryId, prize {type, ref, quantity},
// playerId] (iíiìïïi, deob-10.0.3.js:255154): "<span style='color:#fff'>{alias}</span> won{prize} in the raffle!" for a
// winner in this game (players.findById). prize: skin -> " [name]" in the rarity colour (no such skin: " Nx Skin ref"),
// currency -> " N Krunkies" / " N Junk" / " N Ranked Points" (ref 1 / 2 / 3, else "Currency ref"), subscription ->
// " N days of ref", anything else nothing.
enum { NET_PRIZE_OTHER, NET_PRIZE_SKIN, NET_PRIZE_CURRENCY, NET_PRIZE_SUBSCRIPTION };
typedef struct {
    double quantity; // prize.quantity
    int32_t uid;     // the winner's uid
    uint8_t type;    // NET_PRIZE_*
    char ref[51];    // prize.ref as text ("812", "1", "Premium"), NUL-terminated
} NetLotteryPrizePacket;
_Static_assert(sizeof(NetLotteryPrizePacket) == 64, "NetLotteryPrizePacket: the wire body is 64 bytes");

// NET_EXT_SPIN_WHEEL_RESPONSE (S -> C): v10 "spinW" SPIN_WHEEL_RESPONSE [socketId, uid, error, randNum, skinIndex, funds]
// (iiîïiiì, deob-10.0.3.js:254991): without an error, a map prize wheel (manager.spinners[uid], a PRIZE_WHEEL object)
// spins to randNum and wins skinIndex (updateSpinner :219408); when it is our spin the account takes the skin and its
// KR becomes funds.
typedef struct {
    double funds;  // mine: the account's KR after the spin
    int32_t uid;   // the spinner's uid: its place among the map's spinners (MapManager.addSpinner :186551)
    float rand;    // randNum: the stop, spinVal = 2 pi (rand / 100 + 13) + pi / 2
    int32_t skin;  // the item won (store.skins index)
    uint8_t mine;  // socketId was ours
} NetSpinWheelPacket;
_Static_assert(sizeof(NetSpinWheelPacket) == 21, "NetSpinWheelPacket: the wire body is 21 bytes");

// NET_EXT_CHARGE_CONSENT_REQUEST (S -> C): v10 "chreq" CHARGE_CONSENT_REQUEST [id, amount, modal] (iîiîiïi,
// deob-10.0.3.js:257673; the server's authorizeSpend :221223): a map script asks to charge "This game wants to charge you
// {floor(amount)} KR": the #chargeModal card (modal) or the #chargeConfirm [1] / [2] prompt. It closes after 15 s
// unanswered (the server declines then, :221241). NET_EXT_CHARGE_CONSENT_RESPONSE (C -> S): "chrsp" [id, 1] accepts,
// [id] declines (:257700).
typedef struct {
    double amount;  // KR asked
    int32_t id;     // the request's id, echoed in the response
    uint8_t modal;  // 1: the #chargeModal card
} NetChargeConsentRequestPacket;
_Static_assert(sizeof(NetChargeConsentRequestPacket) == 13, "NetChargeConsentRequestPacket: the wire body is 13 bytes");
typedef struct {
    int32_t id;
    uint8_t accept;
} NetChargeConsentResponsePacket;
_Static_assert(sizeof(NetChargeConsentResponsePacket) == 5, "NetChargeConsentResponsePacket: the wire body is 5 bytes");

// NET_EXT_WARS_CLAN (S -> C): v10 "warsClan" WARS_CLAN [clan_id, clan_name, clan_logo, clan_banner_color] (iíìïïíì,
// deob-10.0.3.js:255551; init's syncData.warsClan :256999, the server's territorialWars.setClanInfo :215092): the Turf
// Wars clan holding this map (has 0: none, ["warsClan", null]). Once per game, 1 s after we entered (or after it came
// while in the game), textPopup("Turf Wars", "Territory controlled by " + clan_name) (:242096, :255563).
typedef struct {
    int32_t clan_id;
    uint8_t has;
    char clan_name[32];
    char clan_logo[64];    // String(clan_logo), "" none
    char banner_color[24]; // String(clan_banner_color)
} NetWarsClanPacket;
_Static_assert(sizeof(NetWarsClanPacket) == 125, "NetWarsClanPacket: the wire body is 125 bytes");

// NET_EXT_KN_OFFICIAL_EDIT_MAP: a slice of one blob too big for NetExtJsonChunk's 255 slices (a map JSON)
typedef struct {
    uint16_t part;  // this slice's index
    uint16_t parts; // slices of the blob (>= 1)
} NetExtBlobChunk;
#define NET_EXT_BLOB_CHUNK_DATA (NET_EXT_BODY_MAX - (int) sizeof(NetExtBlobChunk))
#pragma pack(pop)

// The name of a NET_PLAYER_CLIP_* (v10's clip list, deob-10.0.3.js:21165), NULL out of range.
const char *net_player_clip_name(int clip);
// The NET_PLAYER_CLIP_* of a clip name, -1 none.
int net_player_clip_index(const char *name);
_Static_assert(sizeof(NetPlayerPosePacket) == 48, "NetPlayerPosePacket: the wire body is 48 bytes");

// Writes the envelope (subtype + body) into out (at least NET_EXT_HEADER_SIZE + body_length
// bytes); returns its length, 0 when the body is too long.
uint16_t net_ext_pack(uint8_t *out, size_t out_size, uint16_t subtype, const void *body, uint16_t body_length);
// Splits an EXT payload; 0 when shorter than the header.
int net_ext_unpack(const void *payload, uint16_t length, uint16_t *subtype, const uint8_t **body, uint16_t *body_length);
