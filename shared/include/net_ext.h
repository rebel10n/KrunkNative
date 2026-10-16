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

    // 0x0200 player state, S -> C
    NET_EXT_PLAYER_POSE = 0x0200,  // "pz"  sid, pose | 0
    NET_EXT_PLAYER_LOCKS = 0x0201, // "plk" NetPlayerLocksPacket: disableShooting, disableMelee
    NET_EXT_PLAYER_SPEED_BOOST = 0x0202, // "spb" speedBoost, silent (NetSpeedBoostPacket; official lobbies, krunker_link.c)
    NET_EXT_PLAYER_NAME = 0x0203,        // "00" a player's new name (NetPlayerNamePacket; official lobbies, krunker_link.c)

    // 0x0300 store / live service, S -> C
    NET_EXT_SKIN_DEFINITION_CHANGED = 0x0300, // "sdc" {skinId, skin}
    NET_EXT_SPIN_WHEELS_UPDATED = 0x0301,     // "swu" spin data
    NET_EXT_GAME_ANNOUNCEMENT = 0x0302,       // "gameAnnouncement" NetGameAnnouncementPacket

    // 0x0400 Discord link
    NET_EXT_DISCORD_LINK_WAIT = 0x0400,   // "dlnkW" C -> S
    NET_EXT_DISCORD_LINK_RESULT = 0x0401, // "dlnkR" S -> C

    // 0x0500 charge consent (KrunkScript purchases)
    NET_EXT_CHARGE_CONSENT_REQUEST = 0x0500,  // "chreq" S -> C: requestId, value, flag
    NET_EXT_CHARGE_CONSENT_RESPONSE = 0x0501, // "chrsp" C -> S: requestId[, 1]

    // 0x0600 ranked
    NET_EXT_JOINED_RANK = 0x0600, // "joinedRank" C -> S, no body (v7 key, first used by v10)

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
    NET_EXT_ADMIN_ACTION = 0x0807,    // v10 ADMIN_ACTIONS C -> S NetAdminActionPacket: a DEV's kick / ban from the player list
    NET_EXT_KPD_APPLY = 0x0808,       // "kpdA" C -> S NetKpdApplyPacket: a K.P.D application (answer: NET_EXT_KPD_RESPONSE code 6)
    NET_EXT_VOTE_FORFEIT_STATE = 0x0809, // "vf" S -> C NetVoteForfeitStatePacket: the ranked forfeit vote (#voteToForfeit)

    // 0x0900 in-game actions of v10 without a NetPacket id
    NET_EXT_MARK_POSITION = 0x0900, // "mpos" C -> S NetMarkPositionSendPacket / S -> C NetMarkPositionPacket (a ping)
    NET_EXT_GAME_STATE = 0x0901,    // "gsc" S -> C NetGameStatePacket: v10 gameState (INITIALIZING .. ENDED), every change
    NET_EXT_VOTE_FORFEIT = 0x0902,  // "vf" S -> C NetVoteForfeitPacket: the team's forfeit vote (team only; null when it ends)

    // 0x0a00 Prop Hunt (shared/include/prop_hunt.h): Krunker's "up" / "prpR" / "prp" / "prpR" (v10 game.deob.js
    // players.updateProp :171837, the client's iìiiìïí / iîîììîi :221007 / :221019, the keys :221435)
    NET_EXT_PROP_UPDATE = 0x0a00,   // S -> C NetPropUpdatePacket ("up" sid, propID, skip, propSwitches)
    NET_EXT_PROP_ROTATION = 0x0a01, // S -> C NetPropRotationPacket ("prpR" sid, propRot, propRotSaved, reset)
    NET_EXT_PROP_PICK = 0x0a02,     // C -> S NetPropPickPacket ("prp" propID: the wheel / a random prop)
    NET_EXT_PROP_ROTATE = 0x0a03,   // C -> S NetPropRotatePacket ("prpR" [reset]: toggle / reset the rotation)

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
    // these for the client's inbound queue; no server sends or receives them
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

    // 0x8100..0x810F reserved for settings-wiring (the v10 settings package; coordinator, 2026-10-15)
    NET_EXT_KN_HIT_DAMAGE = 0x8100, // S -> C NetHitDamagePacket, right before a NET_PACKET_HIT whose damage has a fraction
    // 0x8060 official lobby UI (client/src/net/krunker_link.c / krunker_link_lobby.c, client-local, like 0x8030)
    NET_EXT_KN_OFFICIAL_CLOCK_STATE = 0x8060,  // "t" arg 1, u8: 1 "Next Round in", 2 "MATCH OVER", 3 "MATCH ABANDONED" (#endTimer)
    NET_EXT_KN_OFFICIAL_CHAT = 0x8061,         // "ch" NetOfficialChatHeader + name + text: a line with its tab and colours
    NET_EXT_KN_OFFICIAL_CODE_PROMPT = 0x8062,  // "code" ENTER_CODE, no body: v10's gameCode() password popup
    NET_EXT_KN_OFFICIAL_CODE_TIMER = 0x8063,   // "codeT" UPDATE_CODE_TIMER, int32 seconds: "Submit - Ns"
    NET_EXT_KN_OFFICIAL_CODE_PASSED = 0x8064,  // "codeP" CODE_PASSED, no body: clearPops
    NET_EXT_KN_OFFICIAL_GAME_CODE = 0x8065,    // client -> link only: the typed password, NUL-terminated -> C2S "code" [value]
    // 0x8080 official KrunkScript runtime objects (client-local, client/src/entities.c)
    NET_EXT_KN_OFFICIAL_LIVE_OBJECT = 0x8080,  // "lo" one [id, {type, x, y, z, w, l, h, s, data}] (NetExtJsonChunk; init syncData.lo)
    NET_EXT_KN_OFFICIAL_LIVE_SYNC = 0x8081,    // "slo" one [id, op] ("remove") (NetExtJsonChunk)
    NET_EXT_KN_OFFICIAL_OBJECT_MOVED = 0x8082, // "objmv" NetObjectMovedPacket (init syncData.chg too)
} NetExtSubtype;

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
// Kick and Ban buttons): action 0 kicks player `uid` (a Game uid), 1 bans it (its account, when it
// has one, from this server's bans). The server checks the sender is DEV on every request and that
// the target is a player here other than the sender; the client's own check only shows the button.
// Other actions (kill, tp2, tpme, ...) stay chat commands. A refused request changes nothing and
// the sender gets a Server line.
typedef struct {
    int32_t uid;
    uint8_t action; // 0 kick, 1 ban
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
#pragma pack(pop)

// NET_EXT_GAME_ANNOUNCEMENT (S -> C): v10's GAME_ANNOUNCEMENT ("gameAnnouncement" {message, target,
// trackId, bundleId, displayIds, actions}, g:210119) as KrunkNative sends it: the message only
// (UTF-8, NUL-terminated; "" hides the banner). server_bin sends the current one to each player whose
// join settles and to everyone when the console's `announce` changes it (server/src/net.c).
#define NET_GAME_ANNOUNCEMENT_TEXT 256
typedef struct {
    char message[NET_GAME_ANNOUNCEMENT_TEXT];
} NetGameAnnouncementPacket;

// Bodies fit one NetPacket (NET_MAX_PAYLOAD 512) after the subtype.
#define NET_EXT_BODY_MAX (512 - NET_EXT_HEADER_SIZE)

#pragma pack(push, 1)
// The JSON-bodied subtypes (entities 0x0100-0x0105, live objects 0x8080-0x8081): one UTF-8 JSON
// array per record, split over consecutive EXTs of the same subtype when longer than one body
// (an entity's state alone may be 512 bytes): part 0..parts-1 in order, nothing in between.
typedef struct {
    uint8_t part;  // this slice's index
    uint8_t parts; // slices of the record (>= 1)
} NetExtJsonChunk;
#define NET_EXT_JSON_CHUNK_DATA (NET_EXT_BODY_MAX - (int) sizeof(NetExtJsonChunk))

// NET_EXT_PLAYER_LOCKS: v10 "plk" PLAYER_LOCKS [disableShooting, disableMelee] (iìïîììî :220492):
// our player's KrunkScript locks (player.disableShooting / disableMelee), until the next "plk"
typedef struct {
    uint8_t disable_shooting;
    uint8_t disable_melee;
} NetPlayerLocksPacket;

// NET_EXT_KN_OFFICIAL_OBJECT_MOVED: v10 "objmv" OBJECT_MOVED [uid, x, y, z] (game.moveObject :187893):
// a dyn map object moved; y is the GameObject's centre (bottom + h / 2); riders move with it
typedef struct {
    int32_t uid;
    float x, y, z;
} NetObjectMovedPacket;
#pragma pack(pop)

// Writes the envelope (subtype + body) into out (at least NET_EXT_HEADER_SIZE + body_length
// bytes); returns its length, 0 when the body is too long.
uint16_t net_ext_pack(uint8_t *out, size_t out_size, uint16_t subtype, const void *body, uint16_t body_length);
// Splits an EXT payload; 0 when shorter than the header.
int net_ext_unpack(const void *payload, uint16_t length, uint16_t *subtype, const uint8_t **body, uint16_t *body_length);
