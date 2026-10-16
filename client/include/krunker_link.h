#pragma once
// The official-Krunker session's translation layer (client/src/net/krunker_link.c): Krunker's
// packets <-> this client's NetPackets. No sockets, no globals: krunker_net.c (the transport)
// feeds it received frames and sends what it encodes; client/tests/krunker_link_test.c drives it
// directly. Packet layouts are read from the live build's game.deob.js (line numbers below).
//
// Inbound (S->C), each turned into the NetPacket(s) our client already handles:
//   io-init [socketId]                       remember the socket id (players.add compares ids with it)
//   load    [...]                            answered with "load" (INIT_LOAD, :222180)
//   init    [mapIndex, modeIndex, ..., custom]  io.map (krunker_map_source.h: Krunker's official list,
//                                            krunker_maps.h, or arg 9's inline custom map), which emits
//                                            NET_PACKET_INIT with the client's map slot; without io.map:
//                                            NET_PACKET_INIT with mapIndex as is (modeIndex = the Krunker
//                                            mode id) (:219289)
//   frs     [...]                            ENTER_GAME acknowledgement: logged (the spawn is "0")
//   0       [flat list, stride 53]           NET_PACKET_SPAWN per player (:220266, players.add :171902)
//   k       [flat list, stride 13, rate, t]  NET_PACKET_STATE per other player (:220529)
//   init    ... [6] config [7] weps/cls/prks [12] networking   NET_PACKET_GAME_CONFIG + NETWORK_DATA first
//   l       [state array]                    NET_PACKET_RECON + NET_PACKET_PING_RESULT; answered "v" (:220335)
//   f       [reason]                         SYNC_FAILED: the "q" compression state restarts and
//                                            NET_EXT_KN_INPUTS_DROPPED empties the pending inputs (:220333)
//   0       ... [10] level [11] clan [16] isHacker [25] featured [27] premiumT [28] alias [31] accid
//           [44] isBot [51] creatorBadge     NET_PACKET_PLAYER_INFO per player (the board, nametags, kill card);
//                                            names are alias || name (players.add :171939)
//   2       [sid]                            NET_PACKET_LEAVE (:220567)
//   3       [victim, deaths, killer, stats, info, hideFeed]  NET_PACKET_KILL (:220579): killInfo wId / fs /
//                                            hs / wb / pj / stk, hideFeed (NET_KILL_* bits), the killer's class;
//                                            to us also NET_PACKET_DEATH_INFO first (deathStats, :220591)
//   4       [sid, dmg, crit, kill, isAI]     NET_PACKET_HIT, dmg Math.ceil'd (dmgType 0, :221259)
//   h       [health, sid|null, srcX, srcZ, noHitMarker, resetMax]  NET_PACKET_HEALTH (:221040; no source
//                                            with noHitMarker)
//   uh      [hostSocketId, silent]           the host's PLAYER_INFO HOST flag + "host.transfer" line (:219273)
//   t       [formatted, state, rawMs]        NET_PACKET_TIMER: the match clock (:220234, broadcast :191565)
//   start   [formatted, ...]                 seeds the round clock from arg0 (:218939 -> :218948); clears the
//                                            "q" compression state and resync (:218971-218974): the life's first "q" is full
//   ch      [team, name, text, kind, color]  NET_PACKET_CHAT (:221172)
//   pi      []                               answered "po" (:220395)
//   cc      [nonce, unused, ciphertext]      krunker_cheat_answer -> "ccr" (:218169)
//   cpt     [challenge]                      the ALTCHA proof of work (krunker_altcha.h) -> "cptR" [payload]
//                                            (no ahNum step); unsolvable: KRUNKER_LINK_EV_CAPTCHA (:98132)
//   error   [message, code]                  NET_PACKET_ERROR + KRUNKER_LINK_EV_ERROR
// Movement locks and modifiers of our own player (cluster A of the packet audit):
//   lm      [x, y, z, xDire]                 LOCK_MOVE: NET_PACKET_LOCK (MOVE, 1) + TELEPORT_ACK (resetPosition)
//                                            (iîiîìïì :220428 -> game.lockMove :188432)
//   ulm     []                               UNLOCK_MOVE: NET_PACKET_LOCK (MOVE, 0) (game.unlockMove :188442)
//   lock    [locked]                         LOCK_PLAYER: NET_PACKET_LOCK (PLAYER, locked) (:219705)
//   frz noc unl [on]                         NET_PACKET_CHEAT FROZEN / NOCLIP / UNLIMITED_AMMO (:220495 :220488 :220504)
//   spb     [speedBoost, silent]             NET_EXT_PLAYER_SPEED_BOOST (:220498)
//   sic     [on]                             NET_PACKET_SHARING (:220511)
//   jg      [sid, on]                        NET_PACKET_JUGGERNAUT (:220482)
//   busy    [sid, on]                        KrunkerLinkPlayer.busy (setBusy :188630: a nametag icon only)
//   chrg    [sid, krankedTimer ms]           NET_PACKET_MODE_COUNTER CHARGE (:220033)
//   While lm holds, every "q" frame sends moveDir -1 and no jump (:221748); while "lock" holds the
//   live client builds no input at all, and neither does ours (krunker_link_inputs_paused): no "q"
//   leaves and the isn does not count on until the lock ends.
// Player-state corrections (cluster B):
//   chgp    [socketId, x, y, z, doRecon, stopMomentum, dir]  NET_PACKET_CHANGE_POSITION; the player is
//                                            found by its socket id (findById), not its sid (:220937)
//   tm      [sid, team]                      NET_PACKET_TEAM (health kept) (:220422)
//   spk     [key, value]                     you[key] = value: the keys this client models (:220514)
//   wep     [weaponId]                       NET_PACKET_OBJECTIVE WEAPON: game.setWeapon = activeObjective (:189457)
//   rg      [sid, changeClass, items]        a class change: NET_PACKET_LOADOUT + NET_PACKET_TEAM (:220027)
//   rfl     []                               NET_PACKET_REFILL (:220520)
//   gsc     [gameState]                      KrunkerLink.game_state (:219143)
//   00      [[id, name, alias]..., keepOpen] NET_EXT_PLAYER_NAME per rename; keepOpen falsy: the client
//                                            closes the socket (KRUNKER_LINK_EV_CLOSE) (:220322)
//   vk      [alias, votes, needed, isNew, voter] | [null]  NET_EXT_VOTE_KICK_STATE (io "vk", :219974)
//   kpd     [code, message, data]            NET_EXT_KPD_RESPONSE (policeRes, :216287): code 0 a call's
//                                            answer; 1..5 an officer's list, data as JSON in pages
//   kpdf    []                               NET_EXT_KPD_RESET (iiíìîìi: the officer's verdict open again)
//   start   [.., kpdData {sus, caller}]      NET_EXT_KPD_DATA (socket ids as our uids) for a K.P.D officer
//   a       [error, id, name, data, ...]     ACCOUNT_RESPONSE (iîïïiii :210016): our krunker.io account
//                                            signed in on this socket -> KRUNKER_LINK_EV_ACCOUNT (name,
//                                            level = floor(0.03 * sqrt(data.player_score)))
// Score / kill / ammo HUD (cluster C of the packet audit):
//   ts      [team, score] | []               NET_PACKET_TEAM_SCORE; no team clears the scores (team 0) (:220857)
//   5       [delta, relay, absolute, class]  NET_PACKET_SCORE: the popup unless absolute, our score unless relay (:220916)
//   6       [medals, sound, totalKills, inc] NET_PACKET_KILL_MEDALS (kills = totalKills) (:220948)
//   7       [sid, ...leaderStats]*           NET_PACKET_LEADERBOARD + NET_PACKET_LEADER_STATS (mode leaderStats) (:220727)
//   ulb     [index, [player, value]*]        NET_EXT_KN_OFFICIAL_LEADER_BOARD: a map LEADERBOARD's rows (:220065)
//   lv      [lives]                          NET_PACKET_LIVES (:220940)
//   uamo    [ammos, reserveAmmos]            NET_EXT_KN_OFFICIAL_AMMO (:220425)
//   st      [streakId, count]                NET_PACKET_STREAK (:220651)
//   kst     [sid, streak]                    NET_PACKET_KILL_STREAK (:220705)
//   wstk    [weapon, streak]                 NET_PACKET_WEAPON_STREAK (:220669)
//   10      []                               NET_PACKET_ASSIST (:220957)
//   am      [[medal, score, ?, count]*]      NET_PACKET_MEDAL per medal (:220960)
//   krt     [dogTags, depositType]           NET_PACKET_MODE_COUNTER KR_TAGS (:220680)
// Objectives / flags / rounds / end (cluster D):
//   ufi     [team, text]                     NET_PACKET_FLAG index -1: the team's flag line only (:220911)
//   ufl     [uid, x, y, z, carrier]          NET_PACKET_FLAG (game.updateFlag :188774); uid -> index by init's syncData.flg
//   ufp     [uid, x, y, z]                   NET_PACKET_FLAG, the position only (game.updateFlagPing :188830)
//   obj     [uid, ms, initial]               NET_PACKET_OBJECTIVE_V7 (game.setObjective :189407)
//   uobj    [uid, onZone]                    NET_PACKET_OBJECTIVE_OCCUPANCY (game.updateObjective :189448)
//   rds     [roundsLeft, false, win, wins]   NET_PACKET_ROUNDS (:219243)
//   zn      [[x, z, w, l]]                   NET_PACKET_ZONE (game.updateZone :189401)
//   end     [type, didWin, endData, winTeam, endIndex, kr, bp, items]  NET_PACKET_END + NET_PACKET_MODE_END (:219712)
//   ac      [sid, sid2, text]                NET_PACKET_ACTION (:220711)
//   chi     [team, name, key|[key, args], kind, color]  NET_EXT_KN_OFFICIAL_CHAT_I18N (:221142)
//   vc      [dataURL, alias, sid, channel, volume, isFinal, senderAccountId]  a voice clip (iîiïììî :209668)
//                                            -> NET_PACKET_VOICE_V7 chunks (voice.h v7 relay framing, chunk id = sid)
//   init's team scores and syncData (flg, obj, zone) seed the same packets for a joiner.
// Player actions (official-ingame-actions):
//   init    after INIT: NET_PACKET_SOCIAL_V7 (the server takes "ct" channels, "spry", "prpt"), so the
//           client sends its v7 shapes; [15] isComp, [16] isRanked kept
//   start   [.., spectating (2), ..]          NET_PACKET_SPECTATE (window.spectating, :218944)
//   end     endData.mts / vo                  NET_PACKET_MATCH_VOTE_STATE (the vote cards, :219805)
//   mv      [vo, mts]                         NET_PACKET_MATCH_VOTE_STATE (updMatchVote :220107)
//   sp      ["<sid>_<n>", sprayIndex, x, y, z, rotY, rotX, rotZ, dye]  NET_PACKET_SPRAY_V7 (addSpray :189765)
//   mpos    [key, {x, y, z}]                  NET_EXT_MARK_POSITION (addPingIcon :160954)
// Outbound actions: STREAK_SLOTS / PC_STAT kept for "en" [26..28] / [13]; USE_STREAK -> "k0".."k2"
// (callInStreak :220943); SPECTATE_TARGET -> "fpSp" [sid] / [] (:209610); SPRAY_V7 -> "spry" [, index];
// TAUNT -> "prpt" [i]; INTERACT -> "int" / "intSec" [type, id], "drpw", v3's spray "spry";
// CONFIRM_INTERACT -> "cnfm" [, 2]; COMP_SET -> "cd" "t" team / "c" {c, s}; MATCH_VOTE -> "maVote"
// [the option's own mts string]; NET_EXT_MARK_POSITION -> "mpos" [{x, y, z}] (ranked / comp only).
// Outbound: our NET_PACKET_ENT -> "en", NET_PACKET_INPUT(_BATCH) -> "q" (the delta-compressed
// window of :221748-221830), NET_PACKET_CHAT_SEND / CHAT_V7 -> "ct", our NET_PACKET_VOICE_V7 chunks
// (one clip) -> "vc" [dataURL, channel, micVolume, micQuality] (the reader's "load", :209711), the player list's EXT
// NET_EXT_VOTE_KICK -> "v2k" [alias, 1] / [alias] (kickVote :220193, the [2] key :221373) and
// NET_EXT_KPD_CALL -> "kpd" [0, {id: socket id}] (callPolice :216272) / [tab], NET_EXT_KPD_RATE ->
// "rkpd" [0 | 1] (the officer's verdict, iiïíïíì); the rest is dropped.
// Combat visuals and sounds (fx):
//   9       [[sid, hx, hy, hz, impYaw, impPitch, trail, ...]]   NET_PACKET_SHOT (first 0: the gun sound is "s")
//   s       [sound, sid, x, y, z, vol, rate, dist, roll, force, isAsset]  NET_PACKET_SOUND (krunker_sounds.h names)
//   so      [sound, vol, mode, x, y, z, rate, dist, roll, isAsset]        NET_PACKET_SOUND (mode 1: 2D, uid -2)
//   sso     [sound, isAsset]                 NET_PACKET_STOP_SOUND
//   pr      [x, y, z, yaw, pitch, wep|stored, sid, isStored, custom, owner]  NET_PACKET_PROJECTILE (slot remapped)
//   pre     [sid|shotCntP, isOwn, x, y, z, impact]  NET_PACKET_PROJECTILE_END (own: NET_PROJECTILE_END_OWN - cnt)
//   ex      [x, y, z, radius, smCnt, lgCnt]  NET_PACKET_EXPLOSION
//   k [11]  grapple [x, y, z] | null         NET_PACKET_GRAPPLE on a change
//   pk upk bh n bbm fh scr is camb asnd rsnd shk ana   EFFECT / PERKS / MODE_COUNTER HEAD / NUKE / SOUND
//                                            adrop_0 / CHEAT FORCE_HIDE / JUMP_SCARE / SOUND / AMBIENT_CHANGE /
//                                            PLAYER_SOUND / NET_EXT_KN_OFFICIAL_SHAKE / OBJECT_ANIM | AI_ANIM
// Muting is client-side in v10 too (muteUser :217141 only saves mute_chat / mute_voice), and
// reportPlayer opens the profile: neither has a packet.
#include <krunker_cheat.h>
#include <krunker_proto.h>
#include <social.h> // SocialAssembly: our outbound voice clip ("vc")
#include <cJSON.h>
#include <shared.h>
#include <stddef.h>
#include <stdint.h>

#define KRUNKER_LINK_MAX_PLAYERS 64

typedef enum {
    KRUNKER_LINK_EV_NOTE = 0,   // informational (logged)
    KRUNKER_LINK_EV_CAPTCHA,    // the server asked for a Turnstile captcha ("cpt")
    KRUNKER_LINK_EV_ERROR,      // the server's "error" (it closes right after)
    KRUNKER_LINK_EV_JOINED,     // our own player was added (the "0" with our socket id)
    KRUNKER_LINK_EV_UNSUPPORTED,// something this client cannot show (an unknown map index, ...)
    KRUNKER_LINK_EV_CLOSE,      // the server told us to close the socket ("00" with keepOpen falsy)
    KRUNKER_LINK_EV_ACCOUNT,    // "a" ACCOUNT_RESPONSE: the game server signed our krunker.io account in
                                // (text: its name; account_name / account_level of the link)
    KRUNKER_LINK_EV_ACCOUNT_POINTS, // "ua" UPDATE_ACCOUNT: the account row's rankedPoints (account_ranked_points)
} KrunkerLinkEvent;

// init [mapIndex, modeIndex, eventIndex, ..., customsData] (INIT_GAME, game.deob.js:219289): what the
// client needs to find the map (io.map).
typedef struct {
    NetInitPacket init; // the INIT to emit once the map is the client's (init.map_index = map_index until then)
    int map_index;      // arg 0: a position in Krunker's official list (krunker_maps.h), -1 when absent
    int mode_index;     // arg 1: the Krunker mode id
    cJSON *custom;      // arg 9 .data of a custom game (a JSON string or an object), parsed; NULL otherwise.
                        // io.map takes it (frees or keeps it)
    char custom_id[48]; // arg 9 .id ("" none)
} KrunkerLinkInit;

typedef struct {
    // a translated packet for the client's inbound queue
    void (*emit)(void *user, const NetPacket *packet);
    // an encoded frame to write to the WebSocket (in order: ahNum already applied)
    void (*send)(void *user, const uint8_t *frame, size_t len);
    // KRUNKER_LINK_EV_* with a sentence (may be NULL)
    void (*event)(void *user, KrunkerLinkEvent kind, const char *text);
    // optional: answers a "cc" instead of krunker_cheat_answer (same contract: 0 = answered). The
    // native transport releases its lock around the evaluation (QuickJS: up to ~250 ms).
    int (*cheat)(void *user, const char *nonce, const char *ciphertext_b64, KrunkerCcr *out);
    // optional: takes a "cpt" ALTCHA proof of work instead of krunker_altcha_solve: 0 = solved now
    // (*payload_b64 malloc'd, the link sends "cptR"), 1 = the transport answers later itself
    // (krunker_link_send "cptR"), -1 = refused / unsolvable (KRUNKER_LINK_EV_CAPTCHA)
    int (*altcha)(void *user, const char *algorithm, const char *challenge, const char *salt, const char *signature, long max_number,
                  char **payload_b64);
    // optional: an init's map. Called instead of emitting NET_PACKET_INIT: the transport resolves the map
    // (krunker_map_source.h), holds what follows meanwhile and emits info->init with its own map slot.
    void (*map)(void *user, KrunkerLinkInit *info);
    // optional: the account-scoped frames, as a JSON array ["name", args...] (the socket thread):
    // "a" ACCOUNT_RESPONSE (its token argument dropped), "ua" UPDATE_ACCOUNT, "uf" UPDATE_FUNDS, "bpdl"
    // DATA_LOADED, "bppd" SYNC_BP_PROG, "chg" UPDATE_CHALLENGES, "uchp" UPDATE_CHALLENGES_PROGRESS, "cln"
    // CLAN_RESPONSE (the Clans window's clanData):
    // v10 keeps the signed-in account (its KR, skins, stats, battle pass) from these (v10:222148)
    void (*account_frame)(void *user, const char *json, size_t len);
    void *user;
} KrunkerLinkIO;

typedef struct {
    int sid;
    float health;
    int max_health;
    char id[64];   // the socket id of its "0" ([0]): "chgp" finds players by it (findById)
    int team;
    int busy;      // "busy" setBusy (a nametag icon; nothing in this client shows it)
    int class_index; // "0" [6] / "rg" items [15]: the kill card's killer class icon
    char name[32];   // "0" [5] / "00" name (not the alias): the "uh" host line names it
    NetPlayerInfoPacket info; // "0"'s account fields (level, clan, flags) as emitted (HOST follows "uh")
    int grapple;   // "k"[11] grapple: 1 hooked at grapple_point (NET_PACKET_GRAPPLE goes out on a change)
    float grapple_point[3];
} KrunkerLinkPlayer;

// what LEADERBOARD carries of a player that the official packets give piecemeal ("7" columns, "3"
// deaths and kills, our "5" / "6")
typedef struct {
    int sid;
    int score, kills, deaths;
} KrunkerLinkStats;

// a CTF flag by its map object uid ("ufl" / "ufp" / init syncData.flg)
typedef struct {
    int uid;
    float position[3];
    int carrier; // sid, 0 none
} KrunkerLinkFlag;

typedef struct {
    KrunkerLinkIO io;
    KrunkerSession codec;           // ahNum (krunker_proto.h)
    char socket_id[64];             // io-init
    int my_sid;                     // -1 until our "0"
    int joined;                     // "en" sent
    // score / kill / objective HUD (clusters C / D): reset by every "init"
    int mode;                       // init's modeIndex (mode_config: leaderStats / endStats), -1 before
    int my_score;                   // "5": our score as the live client keeps it (myScoreVal)
    KrunkerLinkStats stats[KRUNKER_LINK_MAX_PLAYERS];
    int stats_count;
    KrunkerLinkFlag flags[MODE_MAX_FLAGS]; // in map order (init syncData.flg): the index is our flag index
    int flag_count;
    KrunkerLinkPlayer players[KRUNKER_LINK_MAX_PLAYERS];
    int kpd_case;       // startGame gave a K.P.D case (kpdData): a later one without clears it
    char host_id[64];   // init arg 5 / "uh": the custom lobby's host socket id ("" none): PLAYER_INFO's HOST flag
    // "pr" / "pre": the server's projectile sid (+ 1, 0 = free) of each of our projectile slots (the
    // official pool is not bounded by our GAME_MAX_PROJECTILES), filled round-robin from proj_next
    int proj_sid[GAME_MAX_PROJECTILES];
    int proj_next;
    int player_count;
    // "q" window compression (game.deob.js:221765-221830): the last value of every input field
    // (iîîíîîi, kept across windows), the previous window's angle list (iîiìîií) and the resync
    // flag a SYNC_FAILED sets for the next window (iïíîîìï)
    double last_vals[15];
    uint8_t last_set[15];
    double prev_angles[2 + 2 * NET_INPUT_BATCH_MAX];
    int prev_angle_count;
    int resync;
    int unsupported_map;            // init named no map (no index, no custom data), -1 = none (KRUNKER_LINK_EV_UNSUPPORTED);
                                    // the transport sets it for a map it cannot resolve (krunker_net.c)
    // our player's movement state as the server set it ("lm"/"ulm"/"start" game.moveLock, "lock"
    // window.locked, "noc" noclip): shapes the "q" frames and the "l" RECON flags
    int move_lock, locked, noclip;
    int game_state;                 // "gsc" game.gameState, -1 = not sent
    // init's lobby settings (emit_lobby_config): applyNetworking's rates (snf, tf, sf) and how many
    // config keys the lobby set
    int send_hz, tick_hz, sync_hz, config_keys;
    // the socket carries a krunker.io account's token (`at=`, krunker_account.h): set by the transport
    // after krunker_link_reset; "en" then sends the player's own loadout instead of a guest's
    int account;
    char account_name[64];          // "a" ACCOUNT_RESPONSE: the account's name, "" none yet
    int account_level;              // its level (Account.updateLevel over player_score), 0 = not in it
    // the account row's K.P.D fields (Account :112781-112904): player_role (2 isMod "Marshal", 3
    // isDeputy), player_developer, jobrating / jobratingpositive (the Helpful Reports)
    int account_role, account_dev, account_job_rating, account_job_rating_pos;
    // the account row's rankedPoints (v10 Account.setData, row[47] = rankedPoints, v10:112873) from "a" or
    // "ua": account_rp_known = 1 once a frame carried it
    int account_rp_known;
    double account_ranked_points;
    // init after we had a player: v10 drops it (you = null, game.deob.js initGame) and builds no input
    // frame until our next "0" brings it back; the old round's player must not send into the new one
    int you_gone;
    // init [15] isComp / [16] isRanked (initGame :219366-219367): "mpos" leaves only in such a game
    int is_comp, is_ranked;
    int is_official; // init [14] isOfficial (:219331): "end" tables carry the "reward" column (:219864)
    // the ENT's companions our client sends right before it (NET_PACKET_STREAK_SLOTS / PC_STAT) and the
    // comp lobby's team pick ("cd" "t"): the next "en" carries them ([26..28], [13], [8])
    int8_t streak_slots[3];
    int streak_slots_set;
    int pc_stat;                    // -1 none
    int team_pick;                  // toggleTeam's iïíiîiì (default 1)
    // the end screen's match vote: endData.mts as the server wrote each option ("mode,map[,name,id]"),
    // echoed verbatim by "maVote"; the parsed ids are what the client's cards send back
    char vote_mts[NET_MATCH_PICKS][96];
    int vote_mode[NET_MATCH_PICKS], vote_map[NET_MATCH_PICKS];
    int vote_count;
    // counters (tests, logs, the "[krunker] sync" trace)
    // our NET_PACKET_VOICE_V7 chunks of the clip being sent (one clip at a time, in order): a whole
    // clip goes out as one "vc"; krunker_link_reset frees it
    SocialAssembly voice_out;
    unsigned voice_in, voice_sent; // "vc" clips received / sent
    unsigned frames_in, frames_out, cc_answered, cc_failed, captchas_solved, unknown_in;
    unsigned syncs, sync_failed;    // "l" SYNC_PLAYER applied, "f" SYNC_FAILED received
    // the input stream (KRUNKNATIVE_KRUNKER_TRACE=sync, krunker_net.c): "q" windows and the input frames
    // in them, the last isn sent, the server's last ack ("l" inputSN, -1 none yet) and where it had us
    unsigned q_sent, q_frames;
    int last_isn_sent, last_ack;
    float sync_pos[3];
} KrunkerLink;

// Whether our client builds input frames now. v10's gameLoop builds, predicts and sends none while
// window.locked ("lock") or while it has no player of its own (`you && you.active && !locked`,
// game.deob.js:223145): no isn is used up, the "q" window does not fill, nothing is predicted.
int krunker_link_inputs_paused(const KrunkerLink *link);

void krunker_link_init(KrunkerLink *link, const KrunkerLinkIO *io, int64_t ahkey);
// A new WebSocket: ahNum 0, every per-connection state cleared (the io stays).
void krunker_link_reset(KrunkerLink *link);
// One received binary frame. 0 = malformed (logged and dropped; the session goes on).
int krunker_link_on_frame(KrunkerLink *link, const uint8_t *data, size_t len);
// One of our outbound packets: translated and sent, or dropped. 1 = something was sent.
int krunker_link_send_ours(KrunkerLink *link, NetPacketType type, const void *payload, uint16_t length);
// One of our input frames as the official client's 15 input fields (game.deob.js:221748):
// [isn, dt ms, pitch*1000, yaw*1000, movDirs index, shoot, ads, jump, reload, crouch, scroll,
//  wSwap, 1 - speedLmt, reset, interact]. What the "q" carries and the server's procInputs reads.
void krunker_link_input_fields(const NetInputPacket *in, double v[15]);
// Encode and send one Krunker packet (wire name + args). 1 = sent.
int krunker_link_send(KrunkerLink *link, const char *wire, const KrunkerValue *args, size_t argc);
