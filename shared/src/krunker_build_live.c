// The official Krunker client's per-build values at runtime (shared/include/krunker_build_live.h).
#include "krunker_build_live.h"
#include "auth_join.h" // kn_sha256_bytes / kn_sha256_hex

#include <cJSON.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if KRUNKER_BUILD_SCHEMA != 1
#error "krunker_build.h: unknown schema (tools/krunker/extract_build.py)"
#endif

// ---------------------------------------------------------------- the compiled build

#define KRB_WIRE_(ident, wire, name) wire,
#define KRB_KEY_(ident, wire, name) name,
#define KRB_OFF_(sfx, off, name) off,
#define KRB_NAME_(sfx, off, name) name,

static const KrunkerBuild COMPILED = {
    .live = 0,
    .client_version = KRUNKER_CLIENT_VERSION,
    .build_version = KRUNKER_BUILD_VERSION,
    .source_sha256 = KRUNKER_BUILD_SOURCE_SHA256,
    .ahkey = (int64_t) KRUNKER_AHKEY,
    .ahnum_exempt_wire = KRUNKER_AHNUM_EXEMPT_WIRE,
    .c2s_wire = {KRUNKER_C2S_PACKETS(KRB_WIRE_)},
    .s2c_wire = {KRUNKER_S2C_PACKETS(KRB_WIRE_)},
    .add_stride = KRUNKER_ADD_STRIDE,
    .add = {KRUNKER_ADD_FIELDS(KRB_OFF_)},
    .k_stride = KRUNKER_K_STRIDE,
    .k = {KRUNKER_K_FIELDS(KRB_OFF_)},
    .sync_send_vals = KRUNKER_SYNC_SEND_VALS,
    .sync = {KRUNKER_SYNC_FIELDS(KRB_OFF_)},
    .kill = {KRUNKER_KILL_ARG_VICTIM, KRUNKER_KILL_ARG_DEATHS, KRUNKER_KILL_ARG_KILLER, KRUNKER_KILL_ARG_STATS,
             KRUNKER_KILL_ARG_INFO, KRUNKER_KILL_ARG_HIDE_FEED},
    .damage = {KRUNKER_DAMAGE_ARG_TARGET, KRUNKER_DAMAGE_ARG_DAMAGE, KRUNKER_DAMAGE_ARG_CRIT, KRUNKER_DAMAGE_ARG_KILL,
               KRUNKER_DAMAGE_ARG_AI, KRUNKER_DAMAGE_ARG_SILENT},
    .health = {KRUNKER_HEALTH_ARG_HEALTH, KRUNKER_HEALTH_ARG_SID, KRUNKER_HEALTH_ARG_SRC_X, KRUNKER_HEALTH_ARG_SRC_Z,
               KRUNKER_HEALTH_ARG_NO_HIT_MARKER, KRUNKER_HEALTH_ARG_RESET_MAX},
    .score = {KRUNKER_SCORE_ARG_DELTA, KRUNKER_SCORE_ARG_RELAY, KRUNKER_SCORE_ARG_ABSOLUTE, KRUNKER_SCORE_ARG_CLASS_INDEX},
    .bomb = {KRUNKER_BOMB_ARG_BOMBS},
    .cheat_salt = KRUNKER_CHEAT_SALT,
    .cheat_salt_len = KRUNKER_CHEAT_SALT_LEN,
    .cheat_fnv_offset = KRUNKER_CHEAT_FNV_OFFSET,
    .cheat_fnv_prime = KRUNKER_CHEAT_FNV_PRIME,
    .cheat_fnv_double = KRUNKER_CHEAT_FNV_DOUBLE_MUL,
    .matchmaker_url = KRUNKER_MATCHMAKER_URL,
    .turnstile_site_key = KRUNKER_TURNSTILE_SITE_KEY,
    .region_count = KRUNKER_REGION_COUNT,
    .regions = {KRUNKER_REGIONS},
    .strings_ = NULL,
};

// bundle keys and Krunker field names, in id / slot order
static const char *const C2S_KEY[KRUNKER_C2S_COUNT] = {KRUNKER_C2S_PACKETS(KRB_KEY_)};
static const char *const S2C_KEY[KRUNKER_S2C_COUNT] = {KRUNKER_S2C_PACKETS(KRB_KEY_)};
static const char *const ADD_NAME[KRB_ADD_COUNT] = {KRUNKER_ADD_FIELDS(KRB_NAME_)};
static const char *const K_NAME[KRB_K_COUNT] = {KRUNKER_K_FIELDS(KRB_NAME_)};
static const char *const SYNC_NAME[KRB_SYNC_COUNT] = {KRUNKER_SYNC_FIELDS(KRB_NAME_)};
static const char *const KILL_NAME[KRB_KILL_COUNT] = {"victim", "deaths", "killer", "stats", "info", "hideFeed"};
static const char *const DAMAGE_NAME[KRB_DAMAGE_COUNT] = {"target", "damage", "crit", "kill", "ai", "silent"};
static const char *const HEALTH_NAME[KRB_HEALTH_COUNT] = {"health", "sid", "srcX", "srcZ", "noHitMarker", "resetMax"};
static const char *const SCORE_NAME[KRB_SCORE_COUNT] = {"delta", "relay", "absolute", "classIndex"};
static const char *const BOMB_NAME[KRB_BOMB_COUNT] = {"bombs"};

#undef KRB_WIRE_
#undef KRB_KEY_
#undef KRB_OFF_
#undef KRB_NAME_

// What this client reads, so what a build must still have (anything else may come and go). Every
// KRUNKER_C2S_* / KRUNKER_S2C_* id the client code names is in REQUIRED_* or OPTIONAL_*:
// tools/krunker/test_extract.py fails when one is in neither. REQUIRED: a build without it is refused
// (gameplay would desync). OPTIONAL: cosmetic, a build may drop it (its wire is then NULL: lookups never
// return it, send_c2s sends nothing, the effect is lost). The fields: the ones client/src/net/krunker_link.c reads.
static const int REQUIRED_C2S[] = {
    KRUNKER_C2S_CHAT_MESSAGE, KRUNKER_C2S_CHEAT_CHECK_RESPONSE, KRUNKER_C2S_cptR, KRUNKER_C2S_ENTER_GAME, KRUNKER_C2S_INPUT,
    KRUNKER_C2S_KPD, KRUNKER_C2S_LOAD, KRUNKER_C2S_PING, KRUNKER_C2S_PONG, KRUNKER_C2S_RATE_KPD, KRUNKER_C2S_VOTE_KICK,
    // the player's actions (official-ingame-actions): interact, drop, call a streak (slots 0-2: krunker_link.c sends
    // KILL_STREAK_0 + slot), map triggers, the connection data
    KRUNKER_C2S_INTERACT, KRUNKER_C2S_SECONDARY_INTERACT, KRUNKER_C2S_DROP_WEAPON, KRUNKER_C2S_KILL_STREAK_0,
    KRUNKER_C2S_KILL_STREAK_1, KRUNKER_C2S_KILL_STREAK_2, KRUNKER_C2S_CUSTOM_TRIGGER_INTERACTION, KRUNKER_C2S_SYNC_CONNECTION_DATA,
    // a private lobby's password (official-ingame-lobby): without it a coded lobby cannot be joined
    KRUNKER_C2S_CODE,
};
static const int REQUIRED_S2C[] = {
    KRUNKER_S2C_ACCOUNT_RESPONSE, KRUNKER_S2C_ADD_CHAT, KRUNKER_S2C_ADD_PLAYERS, KRUNKER_S2C_CHANGE_HEALTH,
    KRUNKER_S2C_CHANGE_POSITION, KRUNKER_S2C_CHEAT_CHECK, KRUNKER_S2C_cpt, KRUNKER_S2C_cptR, KRUNKER_S2C_DISCONNECT,
    KRUNKER_S2C_DO_DAMAGE, KRUNKER_S2C_ENTER_GAME, KRUNKER_S2C_ERROR, KRUNKER_S2C_GAME_STATE_CHANGED, KRUNKER_S2C_INIT_GAME,
    KRUNKER_S2C_INIT_LOAD, KRUNKER_S2C_IO_INIT, KRUNKER_S2C_KILL_PLAYER, KRUNKER_S2C_KPD_RESET, KRUNKER_S2C_KPD_RESPONSE,
    KRUNKER_S2C_LOCK_MOVE, KRUNKER_S2C_LOCK_PLAYER, KRUNKER_S2C_NEW_TOKEN, KRUNKER_S2C_REFILL_KNIFE, KRUNKER_S2C_REGEN_PLAYER,
    KRUNKER_S2C_REMOVE_PLAYER, KRUNKER_S2C_SET_BUSY, KRUNKER_S2C_SET_FROZEN, KRUNKER_S2C_SET_JUGGERNAUT,
    KRUNKER_S2C_SET_NOCLIP, KRUNKER_S2C_SET_PLAYER_CHARGED, KRUNKER_S2C_SET_PLAYER_KEY, KRUNKER_S2C_SET_SHARING_IS_CARING,
    KRUNKER_S2C_SET_SPEED_BOOST, KRUNKER_S2C_SET_UNLIMITED, KRUNKER_S2C_SET_WEAPON, KRUNKER_S2C_SOCKET_PING,
    KRUNKER_S2C_START_GAME, KRUNKER_S2C_SYNC_FAILED, KRUNKER_S2C_SYNC_PLAYER, KRUNKER_S2C_UNLOCK_MOVE,
    KRUNKER_S2C_UPDATE_NAMES, KRUNKER_S2C_UPDATE_PLAYERS, KRUNKER_S2C_UPDATE_TEAM, KRUNKER_S2C_UPDATE_TIMER,
    KRUNKER_S2C_VOTE_KICK,
    // official HUD packets that carry game state (official-cd): the match's end, ammo, callable streaks,
    // lives, the zone, objectives and flags
    KRUNKER_S2C_END_GAME, KRUNKER_S2C_UPDATE_AMMO, KRUNKER_S2C_KILL_STREAK, KRUNKER_S2C_UPDATE_LIVES, KRUNKER_S2C_UPDATE_ZONE,
    KRUNKER_S2C_SET_OBJECTIVE, KRUNKER_S2C_UPDATE_FLAG,
    // the world the player moves and plays in (official-ingame-world): map objects and gates, spawnables, airdrops,
    // AI, interactions, mode state (Domination, Parkour checkpoints, Zombies machines / perks / rounds, banks),
    // map-logic values, power-ups, forced hiding
    KRUNKER_S2C_BUILD_OBJECT, KRUNKER_S2C_DESTROY_GAMEOBJECT, KRUNKER_S2C_RESPAWN_OBJ, KRUNKER_S2C_UPDATE_GATE,
    KRUNKER_S2C_TELEPORT_TO_NODE, KRUNKER_S2C_TELEPORT_FROM_NODE, KRUNKER_S2C_CREATE_SPAWNABLE, KRUNKER_S2C_DISPOSE_SPAWNABLE,
    KRUNKER_S2C_SYNC_AIRDROP, KRUNKER_S2C_REMOVE_AIRDROP_BY_SID, KRUNKER_S2C_REMOVE_BY_SID, KRUNKER_S2C_SYNC_ALL_AI,
    KRUNKER_S2C_C_SPAWN, KRUNKER_S2C_PLAYER_INTERACTIONS, KRUNKER_S2C_ALL_PLAYER_INTERACTIONS, KRUNKER_S2C_CONFIRM_INTERACTION,
    KRUNKER_S2C_CAPTURE_CONTROL, KRUNKER_S2C_UPDATE_DOM_ZONE, KRUNKER_S2C_CHECK_POINT_SET, KRUNKER_S2C_UPDATE_MYSTERY_BOX,
    KRUNKER_S2C_SWITCH_MYSTERY_BOX, KRUNKER_S2C_UPDATE_KRUM_MACHINE, KRUNKER_S2C_UPDATE_ZOMBIE_PERKS,
    KRUNKER_S2C_UPDATE_ZOMBIE_ROUND, KRUNKER_S2C_UPDATE_BANK, KRUNKER_S2C_CUSTOM_VAL, KRUNKER_S2C_UPDATE_GLOBAL_VALUE,
    KRUNKER_S2C_UPDATE_POWER_UP, KRUNKER_S2C_SET_HIDDEN,
    // whether we can enter again after a refused "en" (official-ingame-social)
    KRUNKER_S2C_FAILED_TO_ENDER,
    // a private lobby asks for its password and lets us in (official-ingame-lobby)
    KRUNKER_S2C_ENTER_CODE, KRUNKER_S2C_CODE_PASSED,
    // KrunkScript bodies the player collides with or is carried by, and its shooting / melee locks
    // (official-ingame-script): entities, runtime blocks, moved dyn objects
    KRUNKER_S2C_ENTITY_SPAWN, KRUNKER_S2C_ENTITY_REMOVE, KRUNKER_S2C_ENTITY_STATE, KRUNKER_S2C_ENTITY_ATTACH,
    KRUNKER_S2C_ENTITY_DETACH, KRUNKER_S2C_SYNC_NEW_OBJECTS, KRUNKER_S2C_SYNC_LIVE_OBJECTS, KRUNKER_S2C_OBJECT_MOVED,
    KRUNKER_S2C_PLAYER_LOCKS,
};
// (-1 ends each list; the parser never needs them, they document the choice and satisfy the check)
static const int OPTIONAL_C2S[] = {
    // social / cosmetic actions (official-ingame-actions): pings, sprays, taunts, the next-map vote, the spectate target
    KRUNKER_C2S_MARK_POSITION, KRUNKER_C2S_SPRAY, KRUNKER_C2S_PROP_HUNT_TAUNT, KRUNKER_C2S_MATCH_VOTE, KRUNKER_C2S_fpSp,
    // the stalker's taunt keys (shared/include/stalker.h)
    KRUNKER_C2S_STALKER_TAUNT,
    // Prop Hunt picks and rotation (official-ingame-prophunt)
    KRUNKER_C2S_PICK_PROP, KRUNKER_C2S_PROP_ROTATION,
    // voice chat clips (official-ingame-voice)
    KRUNKER_C2S_VOICE_CHAT,
    // the krunker.io account in the game (account-unify): the socket login after a sign-in, the ranked
    // join ("joinedRank"), and the account's RPC (v10 _33978.rpc). A build without them only loses the account
    KRUNKER_C2S_LOGIN, KRUNKER_C2S_RPC, KRUNKER_C2S_JOINED_RANK,
    // the host's Kick / Ban on an official game (the player list's staff buttons, krunker_link_is_host): v10 sends
    // ADMIN_ACTIONS only from them, and the server checks the sender's right. A build without it: those buttons do nothing
    KRUNKER_C2S_ADMIN_ACTIONS,
    // hosting a custom game on krunker.io's servers (the Host Game window's krunker.io hosting): a build without it sends
    // no request ("the request was not sent") and every other packet works as before
    KRUNKER_C2S_CUSTOM_SERVER,
    // the Junkyard's recipe table, asked only by the KRUNKNATIVE_DUMP_RECPS capture (krunker_net.c; not a player feature).
    // A build without it: the capture asks nothing ("the request was not sent"), the game is unchanged
    KRUNKER_C2S_CRAFT_DATA_REQUEST,
    -1,
};
static const int OPTIONAL_S2C[] = {
    // official HUD feedback (official-cd): feeds, medals, scores, leader boards, tags, pings
    KRUNKER_S2C_ACTION_FEED, KRUNKER_S2C_ADD_CHATI18N, KRUNKER_S2C_ADD_MEDAL, KRUNKER_S2C_GET_ASSIST, KRUNKER_S2C_GET_KILL,
    KRUNKER_S2C_GET_SCORE, KRUNKER_S2C_KILL_STREAK_M, KRUNKER_S2C_TEAM_SCORE, KRUNKER_S2C_UPDATE_FLAG_INFO,
    KRUNKER_S2C_UPDATE_FLAG_PING, KRUNKER_S2C_UPDATE_KR_TAGS, KRUNKER_S2C_UPDATE_LEADERS, KRUNKER_S2C_UPDATE_LEADER_BOARD,
    KRUNKER_S2C_UPDATE_OBJECTIVE, KRUNKER_S2C_UPDATE_ROUNDS_DIS, KRUNKER_S2C_UPDATE_WEAPON_STREAK,
    // sights and sounds (official-ingame-fx / -world): tracers, projectiles, explosions, nukes, shakes, sounds, sprays,
    // pings, popups and messages, animations, Big Head, ambience, the host and vote announcements
    KRUNKER_S2C_SHOW_TRACER, KRUNKER_S2C_SHOW_PROJECTILE, KRUNKER_S2C_SHOW_BOMBARDMENT, KRUNKER_S2C_END_PROJECTILE,
    KRUNKER_S2C_EXPLOSION,
    KRUNKER_S2C_NUKE_UPDATE, KRUNKER_S2C_BOMBARDMENT_UPDATE, KRUNKER_S2C_SHAKE, KRUNKER_S2C_EFFECT, KRUNKER_S2C_PLAY_SOUND,
    KRUNKER_S2C_STOP_SOUND, KRUNKER_S2C_PLAYER_SOUND, KRUNKER_S2C_ADD_PLAYER_SOUND, KRUNKER_S2C_REMOVE_PLAYER_SOUND,
    KRUNKER_S2C_TRIGGER_IMG_SOUND, KRUNKER_S2C_JUMP_SCARE, KRUNKER_S2C_ADD_SPRAY, KRUNKER_S2C_MARK_POSITION,
    KRUNKER_S2C_TEXT_POPUP, KRUNKER_S2C_TOGGLE_ELEMENT, KRUNKER_S2C_UPDATE_GAME_MESSAGE, KRUNKER_S2C_UPDATE_LOCKOUT_TIM,
    KRUNKER_S2C_LOG_TIME, KRUNKER_S2C_KR_PACKAGE, KRUNKER_S2C_SERV_ANIM, KRUNKER_S2C_UPDATE_HEAD_SIZE,
    KRUNKER_S2C_CHANGE_AMBIENT, KRUNKER_S2C_UPDATE_HOST, KRUNKER_S2C_UPD_MATCH_VOTE,
    // match social lines (official-ingame-social): the forfeit vote, reconnects, skins found
    KRUNKER_S2C_VOTE_FORFEIT, KRUNKER_S2C_PLAYER_RECONNECTED, KRUNKER_S2C_SKIN_REWARD, KRUNKER_S2C_SKIN_FOUND_MESSAGE,
    // voice chat clips (official-ingame-voice)
    KRUNKER_S2C_VOICE_CHAT,
    // the krunker.io account's numbers in the game (account-unify, krunker_link.c): name / level / rankedPoints
    // ("ua"), balances (KR, junk), challenge rewards ("chlR"). Without them the account shows what it had
    KRUNKER_S2C_UPDATE_ACCOUNT, KRUNKER_S2C_UPDATE_BALANCES, KRUNKER_S2C_UPDATE_CHALLENGE_REWARDS,
    // the Stalker banner (official-ingame-popups)
    KRUNKER_S2C_STALKER_PICKED,
    // the password popup's "Submit - Ns" countdown (official-ingame-lobby)
    KRUNKER_S2C_UPDATE_CODE_TIMER,
    // entity events: only the map's client script reads them (official-ingame-script)
    KRUNKER_S2C_ENTITY_EVENT,
    // Prop Hunt (official-ingame-prophunt): a prop's size and the prop model's rotation; only the Prop Hunt mode sends them
    KRUNKER_S2C_UPDATE_PROP, KRUNKER_S2C_UPDATE_PROP_ROT,
    // the answer to CUSTOM_SERVER (its game id, or the refusal): only read after a custom request. A build without it
    // loses that reply, so the hosting attempt times out ("Please try again.")
    KRUNKER_S2C_CUSTOM_RESPONSE,
    // the answer to CRAFT_DATA_REQUEST (the same capture): the recipe rows and the junk balance
    KRUNKER_S2C_CRAFT_DATA_RESPONSE,
    -1,
};
static const int REQUIRED_ADD[] = {
    KRB_ADD_ID, KRB_ADD_SID, KRB_ADD_X, KRB_ADD_Y, KRB_ADD_Z, KRB_ADD_NAME, KRB_ADD_CLASS_INDEX, KRB_ADD_HEALTH,
    KRB_ADD_MAX_HEALTH, KRB_ADD_TEAM, KRB_ADD_X_DIRE, KRB_ADD_SEC_INDEX, KRB_ADD_ATTACH_INDEX,
};
static const int REQUIRED_K[] = {
    KRB_K_SID, KRB_K_X, KRB_K_Y, KRB_K_Z, KRB_K_X_DIRE, KRB_K_Y_DIRE, KRB_K_WEAPON, KRB_K_AIM_VAL, KRB_K_CROUCH_VAL,
};
static const int REQUIRED_SYNC[] = {
    KRB_SYNC_INPUT_SN, KRB_SYNC_PING, KRB_SYNC_X, KRB_SYNC_Y, KRB_SYNC_Z, KRB_SYNC_VEL_Y, KRB_SYNC_VEL_X, KRB_SYNC_VEL_Z,
    KRB_SYNC_X_DIRE, KRB_SYNC_ON_GROUND, KRB_SYNC_DID_JUMP, KRB_SYNC_DID_JUMP_W, KRB_SYNC_WALL_JUMP_C, KRB_SYNC_ON_WALL,
    KRB_SYNC_ON_LADDER, KRB_SYNC_AIM_VAL, KRB_SYNC_CROUCH_VAL, KRB_SYNC_WEAPON, KRB_SYNC_SLIDE_TIMER, KRB_SYNC_CAN_SLIDE,
    KRB_SYNC_ON_TERRAIN, KRB_SYNC_X_VEL_C, KRB_SYNC_Z_VEL_C, KRB_SYNC_GRAPPLE, KRB_SYNC_CAN_SLIDE_T,
};
static const int REQUIRED_KILL[] = {KRB_KILL_VICTIM, KRB_KILL_DEATHS, KRB_KILL_KILLER, KRB_KILL_INFO};
static const int REQUIRED_DAMAGE[] = {KRB_DAMAGE_TARGET, KRB_DAMAGE_DAMAGE, KRB_DAMAGE_CRIT, KRB_DAMAGE_KILL, KRB_DAMAGE_AI};
static const int REQUIRED_HEALTH[] = {KRB_HEALTH_HEALTH, KRB_HEALTH_SID, KRB_HEALTH_SRC_X, KRB_HEALTH_SRC_Z, KRB_HEALTH_RESET_MAX};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const KrunkerBuild *g_current = &COMPILED;

// 1 when a missing packet id of a parsed build is one the client can do without (OPTIONAL_*)
static int optional_packet(const int *list, const int id) {
    for (int i = 0; list[i] >= 0; i++) {
        if (list[i] == id) return 1;
    }
    return 0;
}

const KrunkerBuild *krunker_build_compiled(void) { return &COMPILED; }
const KrunkerBuild *krunker_build(void) { return g_current; }
void krunker_build_use(const KrunkerBuild *b) { g_current = b ? b : &COMPILED; }

void krunker_build_free(KrunkerBuild *b) {
    if (!b || b == &COMPILED) return;
    if (g_current == b) g_current = &COMPILED;
    free(b->strings_);
    free(b);
}

// ---------------------------------------------------------------- parsing helpers

static int why_set(char *why, const size_t n, const int rc, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int why_set(char *why, const size_t n, const int rc, const char *fmt, ...) {
    if (why && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, n, fmt, ap);
        va_end(ap);
    }
    return rc;
}

static const cJSON *obj(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }

// an integral JSON number in [lo, hi]
static int int_in(const cJSON *v, const double lo, const double hi, long long *out) {
    if (!cJSON_IsNumber(v) || v->valuedouble != (double) (long long) v->valuedouble || v->valuedouble < lo || v->valuedouble > hi)
        return 0;
    *out = (long long) v->valuedouble;
    return 1;
}

// a JSON string of 1..n-1 bytes, every byte in allowed (NULL: printable ASCII)
static int str_copy(const cJSON *v, char *out, const size_t n, const char *allowed) {
    const char *s = cJSON_GetStringValue(v);
    if (!s || !s[0] || strlen(s) >= n) return 0;
    for (const char *p = s; *p; p++) {
        if (allowed ? !strchr(allowed, *p) && !isalnum((unsigned char) *p) : (*p < 0x20 || *p > 0x7e)) return 0;
    }
    snprintf(out, n, "%s", s);
    return 1;
}

// "0x811c9dc5" / 2166136261 -> 32-bit value
static int u32_of(const cJSON *v, uint32_t *out) {
    if (cJSON_IsNumber(v)) {
        long long x;
        if (!int_in(v, 0, 4294967295.0, &x)) return 0;
        *out = (uint32_t) x;
        return 1;
    }
    const char *s = cJSON_GetStringValue(v);
    if (!s || !s[0]) return 0;
    char *end = NULL;
    const unsigned long long x = strtoull(s, &end, 0);
    if (!end || *end || x > 0xffffffffull) return 0;
    *out = (uint32_t) x;
    return 1;
}

// "10.0.3r1": digits.digits.digits, an optional rN
static int version_ok(const char *s) {
    int parts = 0;
    const char *p = s;
    while (parts < 3) {
        if (!isdigit((unsigned char) *p)) return 0;
        while (isdigit((unsigned char) *p)) p++;
        parts++;
        if (parts < 3 && *p++ != '.') return 0;
    }
    if (*p == 'r') {
        p++;
        if (!isdigit((unsigned char) *p)) return 0;
        while (isdigit((unsigned char) *p)) p++;
    }
    return *p == 0;
}

// layouts.<name>.fields [{offset, name}, ...] -> offsets by slot (others -1); every required slot
// present, every offset below limit
static int read_fields(const cJSON *layout, const char *label, const char *const *names, const int count, const int *required,
                       const size_t nrequired, const int limit, int16_t *off, char *why, const size_t why_n) {
    for (int i = 0; i < count; i++) off[i] = -1;
    const cJSON *fields = obj(layout, "fields");
    if (!cJSON_IsArray(fields)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s has no field list", label);
    const cJSON *f;
    cJSON_ArrayForEach(f, fields) {
        const char *name = cJSON_GetStringValue(obj(f, "name"));
        long long o;
        if (!name || !int_in(obj(f, "offset"), 0, 32767, &o))
            return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s has a field without a name or offset", label);
        if (o >= limit)
            return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s field \"%s\" at %lld is outside its %d values", label, name, o, limit);
        for (int i = 0; i < count; i++) {
            if (!strcmp(names[i], name) && off[i] < 0) off[i] = (int16_t) o; // the first one wins (the handler's)
        }
    }
    for (size_t r = 0; r < nrequired; r++) {
        if (off[required[r]] < 0)
            return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s no longer has \"%s\"", label, names[required[r]]);
    }
    return KRUNKER_BUILD_OK;
}

// layouts.<name>: {argc, args: {role: position}}
static int read_args(const cJSON *layouts, const char *key, const char *label, const char *const *names, const int count,
                     const int *required, const size_t nrequired, int16_t *pos, char *why, const size_t why_n) {
    for (int i = 0; i < count; i++) pos[i] = -1;
    const cJSON *layout = obj(layouts, key);
    if (!layout && !nrequired) return KRUNKER_BUILD_OK; // an optional packet's layout may be missing
    const cJSON *args = obj(layout, "args");
    long long argc;
    if (!cJSON_IsObject(args) || !int_in(obj(layout, "argc"), 0, 64, &argc))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s has no argument list", label);
    for (int i = 0; i < count; i++) {
        long long p;
        const cJSON *v = obj(args, names[i]);
        if (!v) continue;
        if (!int_in(v, 0, (double) argc - 1, &p))
            return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s argument \"%s\" is outside its %lld arguments", label, names[i], argc);
        pos[i] = (int16_t) p;
    }
    for (size_t r = 0; r < nrequired; r++) {
        if (pos[required[r]] < 0)
            return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "%s no longer has the argument \"%s\"", label, names[required[r]]);
    }
    return KRUNKER_BUILD_OK;
}

// packets_<side>: [{name, wire}] -> wire names by id (into the string block); the required ids present
typedef struct {
    char *block;
    size_t used, cap;
} StrBlock;

static const char *block_add(StrBlock *b, const char *s) {
    const size_t n = strlen(s) + 1;
    if (b->used + n > b->cap) return NULL;
    char *p = b->block + b->used;
    memcpy(p, s, n);
    b->used += n;
    return p;
}

static int wire_ok(const char *w) {
    const size_t n = w ? strlen(w) : 0;
    if (!n || n > 31) return 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char) w[i] <= 0x20 || (unsigned char) w[i] > 0x7e) return 0;
    }
    return 1;
}

static int read_packets(const cJSON *list, const char *side, const char *const *keys, const int count, const int *required,
                        const size_t nrequired, const int *optional, const char **wire, StrBlock *blk, char *why, const size_t why_n) {
    for (int i = 0; i < count; i++) wire[i] = NULL;
    if (!cJSON_IsArray(list)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no %s packet table", side);
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        const char *name = cJSON_GetStringValue(obj(e, "name"));
        const char *w = cJSON_GetStringValue(obj(e, "wire"));
        if (!name || !wire_ok(w)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "a %s packet entry has no name or a bad wire name", side);
        for (int i = 0; i < count; i++) {
            if (wire[i] || strcmp(keys[i], name)) continue;
            if (!(wire[i] = block_add(blk, w))) return why_set(why, why_n, KRUNKER_BUILD_ENOMEM, "out of memory");
        }
    }
    for (size_t r = 0; r < nrequired; r++) {
        if (!wire[required[r]] && !optional_packet(optional, required[r]))
            return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the %s packet %s is gone", side, keys[required[r]]);
    }
    return KRUNKER_BUILD_OK;
}

static size_t packet_bytes(const cJSON *list) {
    size_t n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        const char *w = cJSON_GetStringValue(obj(e, "wire"));
        if (w) n += strlen(w) + 1;
    }
    return n;
}

// ---------------------------------------------------------------- krunker_build_parse

static int parse_doc(const cJSON *d, KrunkerBuild *b, StrBlock *blk, char *why, const size_t why_n) {
    long long v;
    if (!cJSON_IsObject(d)) return why_set(why, why_n, KRUNKER_BUILD_EJSON, "not a JSON object");
    if (!int_in(obj(d, "schema"), 0, 1e9, &v) || v != KRUNKER_BUILD_SCHEMA)
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "build.json schema %s, this client reads %d",
                       cJSON_IsNumber(obj(d, "schema")) ? "changed" : "missing", KRUNKER_BUILD_SCHEMA);
    if (!str_copy(obj(d, "client_version"), b->client_version, sizeof(b->client_version), ".") || !version_ok(b->client_version))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no clientVersion");
    if (!str_copy(obj(d, "build_version"), b->build_version, sizeof(b->build_version), "_-"))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no buildVersion");
    b->source_sha256[0] = 0;
    str_copy(obj(obj(d, "source"), "sha256"), b->source_sha256, sizeof(b->source_sha256), "");
    if (!int_in(obj(d, "ahkey"), 0, 9007199254740991.0, &v)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no AHKEY");
    b->ahkey = v;
    const char *trailer = cJSON_GetStringValue(obj(d, "ahnum_trailer"));
    if (!trailer || strcmp(trailer, "nibbles"))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the ahNum trailer is no longer two nibbles (%s)", trailer ? trailer : "missing");
    const char *exempt = cJSON_GetStringValue(obj(obj(d, "ahnum_exempt"), "wire"));
    if (!wire_ok(exempt)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no ahNum-exempt packet");
    snprintf(b->ahnum_exempt_wire, sizeof(b->ahnum_exempt_wire), "%s", exempt);

    int rc;
    if ((rc = read_packets(obj(d, "packets_c2s"), "client", C2S_KEY, KRUNKER_C2S_COUNT, REQUIRED_C2S,
                           COUNT(REQUIRED_C2S), OPTIONAL_C2S, b->c2s_wire, blk, why, why_n)))
        return rc;
    if ((rc = read_packets(obj(d, "packets_s2c"), "server", S2C_KEY, KRUNKER_S2C_COUNT, REQUIRED_S2C,
                           COUNT(REQUIRED_S2C), OPTIONAL_S2C, b->s2c_wire, blk, why, why_n)))
        return rc;

    // CHEAT_CHECK: the formulas krunker_cheat.c implements, with this build's constants
    const cJSON *cheat = obj(d, "cheat");
    const char *ks = cJSON_GetStringValue(obj(cheat, "keystream_formula"));
    if (!ks || strcmp(ks, "v1"))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the cheat check's keystream changed (%s)", ks ? ks : "missing");
    const char *handler = cJSON_GetStringValue(obj(cheat, "handler"));
    if (handler && strcmp(handler, "v1")) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the cheat check's handler changed (%s)", handler);
    const cJSON *cc = obj(cheat, "cc_args");
    long long nonce_at, ct_at;
    if (cc && (!int_in(obj(cc, "nonce"), 0, 64, &nonce_at) || !int_in(obj(cc, "ciphertext"), 0, 64, &ct_at) || nonce_at != 0 || ct_at != 2))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the cheat check's arguments moved");
    const cJSON *salt = obj(cheat, "salt_bytes");
    b->cheat_salt_len = 0;
    if (cJSON_IsArray(salt)) {
        const cJSON *s;
        cJSON_ArrayForEach(s, salt) {
            if (b->cheat_salt_len >= KRB_MAX_SALT || !int_in(s, 0, 255, &v))
                return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the cheat check's salt is not a byte list");
            b->cheat_salt[b->cheat_salt_len++] = (uint8_t) v;
        }
    } else if (cJSON_GetStringValue(obj(cheat, "salt"))) {
        const char *s = cJSON_GetStringValue(obj(cheat, "salt"));
        b->cheat_salt_len = strlen(s);
        if (b->cheat_salt_len >= KRB_MAX_SALT) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the cheat check's salt is too long");
        memcpy(b->cheat_salt, s, b->cheat_salt_len);
    }
    if (!b->cheat_salt_len) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no cheat check salt");
    if (!u32_of(obj(cheat, "fnv_offset"), &b->cheat_fnv_offset) || !u32_of(obj(cheat, "fnv_prime"), &b->cheat_fnv_prime))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "no cheat check hash constants");
    const char *mul = cJSON_GetStringValue(obj(cheat, "fnv_multiply"));
    if (mul && !strcmp(mul, "double")) b->cheat_fnv_double = 1;
    else if (mul && !strcmp(mul, "imul")) b->cheat_fnv_double = 0;
    else return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the cheat check's hash step changed (%s)", mul ? mul : "missing");

    // the per-player arrays and the per-event arguments
    const cJSON *lay = obj(d, "layouts");
    const cJSON *add = obj(lay, "add_players"), *k = obj(lay, "update_players"), *sync = obj(lay, "sync_player");
    long long stride;
    if (!int_in(obj(add, "stride"), 1, 4096, &stride)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "ADD_PLAYERS has no stride");
    b->add_stride = (int) stride;
    if ((rc = read_fields(add, "ADD_PLAYERS", ADD_NAME, KRB_ADD_COUNT, REQUIRED_ADD, COUNT(REQUIRED_ADD), b->add_stride, b->add, why, why_n)))
        return rc;
    if (!int_in(obj(k, "stride"), 1, 4096, &stride)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "UPDATE_PLAYERS has no stride");
    b->k_stride = (int) stride;
    const cJSON *kargs = obj(k, "args");
    if (kargs && (!int_in(obj(kargs, "list"), 0, 64, &v) || v != 0))
        return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "UPDATE_PLAYERS' player list moved");
    if ((rc = read_fields(k, "UPDATE_PLAYERS", K_NAME, KRB_K_COUNT, REQUIRED_K, COUNT(REQUIRED_K), b->k_stride, b->k, why, why_n)))
        return rc;
    if (!int_in(obj(sync, "send_vals"), 0, 4096, &stride)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "SYNC_PLAYER has no layout");
    b->sync_send_vals = (int) stride;
    if ((rc = read_fields(sync, "SYNC_PLAYER", SYNC_NAME, KRB_SYNC_COUNT, REQUIRED_SYNC, COUNT(REQUIRED_SYNC), 4096, b->sync, why, why_n)))
        return rc;
    if ((rc = read_args(lay, "kill_player", "KILL_PLAYER", KILL_NAME, KRB_KILL_COUNT, REQUIRED_KILL, COUNT(REQUIRED_KILL), b->kill, why, why_n)))
        return rc;
    if ((rc = read_args(lay, "do_damage", "DO_DAMAGE", DAMAGE_NAME, KRB_DAMAGE_COUNT, REQUIRED_DAMAGE, COUNT(REQUIRED_DAMAGE), b->damage, why,
                        why_n)))
        return rc;
    if ((rc = read_args(lay, "change_health", "CHANGE_HEALTH", HEALTH_NAME, KRB_HEALTH_COUNT, REQUIRED_HEALTH, COUNT(REQUIRED_HEALTH),
                        b->health, why, why_n)))
        return rc;
    if ((rc = read_args(lay, "get_score", "GET_SCORE", SCORE_NAME, KRB_SCORE_COUNT, NULL, 0, b->score, why, why_n))) return rc;
    if ((rc = read_args(lay, "show_bombardment", "SHOW_BOMBARDMENT", BOMB_NAME, KRB_BOMB_COUNT, NULL, 0, b->bomb, why, why_n))) return rc;

    // extras: optional (the compiled values stay when one did not extract)
    const cJSON *ex = obj(d, "extras");
    char url[sizeof(b->matchmaker_url)];
    if (str_copy(obj(ex, "matchmaker_url"), url, sizeof(url), NULL)) {
        if (strncmp(url, "https://", 8)) return why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "the matchmaker URL is not https");
        snprintf(b->matchmaker_url, sizeof(b->matchmaker_url), "%s", url);
    }
    str_copy(obj(ex, "turnstile_site_key"), b->turnstile_site_key, sizeof(b->turnstile_site_key), "_-");
    const cJSON *regions = obj(obj(ex, "regions"), "regions");
    if (cJSON_IsArray(regions) && cJSON_GetArraySize(regions) > 0) {
        int n = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, regions) {
            if (n < KRB_MAX_REGIONS && str_copy(r, b->regions[n], sizeof(b->regions[n]), "-")) n++;
        }
        if (n) b->region_count = n;
    }
    return KRUNKER_BUILD_OK;
}

int krunker_build_parse(const char *json, const size_t len, KrunkerBuild **out, char *why, const size_t why_n) {
    if (out) *out = NULL;
    if (why && why_n) why[0] = 0;
    if (!json || !out) return why_set(why, why_n, KRUNKER_BUILD_EJSON, "no document");
    cJSON *d = cJSON_ParseWithLength(json, len);
    if (!d) return why_set(why, why_n, KRUNKER_BUILD_EJSON, "not JSON");
    KrunkerBuild *b = malloc(sizeof(*b));
    StrBlock blk = {0};
    blk.cap = packet_bytes(obj(d, "packets_c2s")) + packet_bytes(obj(d, "packets_s2c")) + 1;
    blk.block = malloc(blk.cap);
    if (!b || !blk.block) {
        free(b);
        free(blk.block);
        cJSON_Delete(d);
        return why_set(why, why_n, KRUNKER_BUILD_ENOMEM, "out of memory");
    }
    *b = COMPILED; // the extras a document lacks keep the compiled values
    b->live = 1;
    b->strings_ = blk.block;
    const int rc = parse_doc(d, b, &blk, why, why_n);
    cJSON_Delete(d);
    if (rc != KRUNKER_BUILD_OK) {
        free(blk.block);
        free(b);
        return rc;
    }
    *out = b;
    return KRUNKER_BUILD_OK;
}

// ---------------------------------------------------------------- the manifest

static int hex64(const char *s) {
    if (!s || strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        if (!isdigit((unsigned char) s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) return 0;
    }
    return 1;
}

int krunker_build_manifest_parse(const char *json, const size_t len, KrunkerBuildManifest *out, char *why, const size_t why_n) {
    if (!out) return KRUNKER_BUILD_EJSON;
    memset(out, 0, sizeof(*out));
    cJSON *m = json ? cJSON_ParseWithLength(json, len) : NULL;
    long long schema;
    int rc = KRUNKER_BUILD_OK;
    if (!cJSON_IsObject(m)) {
        rc = why_set(why, why_n, KRUNKER_BUILD_EJSON, "the manifest is not JSON");
    } else if (!int_in(obj(m, "schema"), 0, 1e9, &schema) || schema != 1) {
        rc = why_set(why, why_n, KRUNKER_BUILD_ESHAPE, "unknown manifest schema");
    } else if (!str_copy(obj(m, "build_version"), out->build_version, sizeof(out->build_version), "_-") ||
               !str_copy(obj(m, "client_version"), out->client_version, sizeof(out->client_version), ".") ||
               !hex64(cJSON_GetStringValue(obj(m, "sha256"))) ||
               !str_copy(obj(m, "path"), out->path, sizeof(out->path), "._-/")) {
        rc = why_set(why, why_n, KRUNKER_BUILD_EJSON, "the manifest lacks a version, sha256 or path");
    } else {
        snprintf(out->sha256, sizeof(out->sha256), "%s", cJSON_GetStringValue(obj(m, "sha256")));
        if (out->path[0] == '/' || strstr(out->path, "..") || strstr(out->path, "//"))
            rc = why_set(why, why_n, KRUNKER_BUILD_EJSON, "the manifest's path is not a relative one");
    }
    cJSON_Delete(m);
    if (rc) memset(out, 0, sizeof(*out));
    return rc;
}

int krunker_build_accept(const KrunkerBuildManifest *m, const char *doc, const size_t len, KrunkerBuild **out, char *why,
                         const size_t why_n) {
    if (out) *out = NULL;
    if (!m || !doc || !out) return why_set(why, why_n, KRUNKER_BUILD_EJSON, "no document");
    uint8_t digest[32];
    char hex[65];
    kn_sha256_bytes(doc, len, digest);
    kn_sha256_hex(digest, hex);
    if (strcmp(hex, m->sha256))
        return why_set(why, why_n, KRUNKER_BUILD_EJSON, "the document's sha256 %.12s is not the manifest's %.12s", hex, m->sha256);
    KrunkerBuild *b = NULL;
    const int rc = krunker_build_parse(doc, len, &b, why, why_n);
    if (rc) return rc;
    if (strcmp(b->build_version, m->build_version) || strcmp(b->client_version, m->client_version)) {
        krunker_build_free(b);
        return why_set(why, why_n, KRUNKER_BUILD_EJSON, "the document's versions are not the manifest's");
    }
    *out = b;
    return KRUNKER_BUILD_OK;
}

int krunker_build_doc_url(const char *manifest_url, const KrunkerBuildManifest *m, char *out, const size_t out_n) {
    if (!manifest_url || !m || !m->path[0] || !out || !out_n) return 0;
    // the manifest's directory's parent: .../updates/client/krunker-build.json -> .../updates/
    const char *q = strpbrk(manifest_url, "?#");
    size_t end = q ? (size_t) (q - manifest_url) : strlen(manifest_url);
    int slashes = 0;
    while (end > 0) {
        if (manifest_url[end - 1] == '/' && ++slashes == 2) break;
        end--;
    }
    const char *scheme = strstr(manifest_url, "://");
    if (slashes < 2 || !scheme || end <= (size_t) (scheme - manifest_url) + 3) return 0;
    const int w = snprintf(out, out_n, "%.*s%s", (int) end, manifest_url, m->path);
    return w > 0 && (size_t) w < out_n;
}
