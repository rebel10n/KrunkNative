/*
 * services/brain/editor_rooms.c - Krunker v10's multiplayer map editing rooms on the social socket.
 *
 * v10.0.7's editor (KE.multiplayer, editor-10.0.7.js:9696-10294) co-edits a map with other editors
 * over its own socket: it sends the mp* frames below on the same connection as its uploads and
 * answers them with the handlers of getSocketHandlers (:9850-9880). Krunker's room server
 * (editor.krunker.io/ws) is not public, so kn_brain relays the same frames between the editor
 * sockets of a room, with the semantics the client relies on:
 *
 *   mpHost(settingsJSON)        -> mpRoomJoined(roomId, playersJSON, settingsJSON, yourId)
 *   mpCheckRoom(roomId)         -> mpRoomInfo(roomId, {hasPassword, players, maxPlayers} | {error})
 *   mpJoin(roomId, password)    -> mpRoomJoined to the joiner, mpPlayerJoined(playerJSON) to the others,
 *                                  mpRequestMap(joinerId) to the host (it answers mpSyncMap(map, joinerId)),
 *                                  mpScriptLocked(id, name) to the joiner while scripts are locked;
 *                                  refused with mpError("Room not found" | "Incorrect password" |
 *                                  "Room is full" | "You are banned from this room")
 *   mpLeave / the socket closing -> mpPlayerLeft(id); the host's room closes (mpKicked to the others)
 *                                  when it was made with closeOnHostDisconnect, else the next player
 *                                  becomes the host (mpHostChanged(id) to everyone)
 *   mpKick(id) / mpBan(id)      -> host only: mpKicked / mpBanned(reason) to the player; a ban keeps
 *                                  the account (or a guest's address) out of the room
 *   mpRoomSettings(json)        -> host only: {maxPlayers 2..20} to everyone as mpRoomSettings(json)
 *   mpChat(text)                -> mpChat(id, text, name, color) to everyone (200 characters at most)
 *   mpPlayerMove(pos, rot)      -> mpPlayerMove(id, pos, rot) to the others
 *   mpPlayerSelect(sid)         -> mpPlayerSelect(id, sid) to the others
 *   mpObjectAdded(tempId, obj)  -> mpObjectConfirmed(tempId, sid) to the sender, mpObjectAdded(sid, obj,
 *                                  senderId) to the others (the room numbers objects: sid 1, 2, ...)
 *   mpObjectRemoved(sid...), mpObjectUpdated(sid, changes), mpObjectsMoved(json), mpGroupRemoved(sid...)
 *                               -> the same frame to the others
 *   mpClearMap                  -> host only: to the others
 *   mpSyncMap(map[, targetId])  -> objects without "_sid" get one; mpSyncMapConfirmed([sid per object])
 *                                  to the sender; the map (with every "_sid") to the target (the host's
 *                                  answer to mpRequestMap), else to every other player
 *   mpScriptLock / mpScriptUnlock / mpScriptUpdate(json)
 *                               -> mpScriptLockAcquired to the sender and mpScriptLocked(id, name) to the
 *                                  others, or mpScriptLockDenied(holder name); mpScriptUnlocked(id) to the
 *                                  others; the holder's scripts to the others
 *
 * Players are numbered per room from 1; a player's name is the account's (a guest is "Guest"); the
 * colours are a fixed palette by number. Rooms live in memory only (as v10's: a room ends with its
 * players). Owner: svc-accounts.
 */
#include "acct_internal.h"
#include "editor_rooms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/crypto.h"
#include "knsvc/json.h"

#define MP_MAX_PLAYERS 20 /* the Max Players range, editor-10.0.7.js:9647 */
#define MP_MAX_BANS 64
#define MP_CHAT_MAX 200   /* #mp-chat-input maxLength, editor-10.0.7.js:9500 */

typedef struct MpPlayer {
    SocialSession *s;
    int id;
    int role; /* 1 = host */
    char name[40];
    const char *color;
} MpPlayer;

typedef struct MpBan {
    int64_t account_id;
    char ip[64];
} MpBan;

typedef struct MpRoom {
    char id[40];
    char password[65];
    int max_players;
    int close_on_host;
    MpPlayer players[MP_MAX_PLAYERS];
    int n;
    int next_player;
    int next_sid;
    MpBan bans[MP_MAX_BANS];
    int nbans;
    int lock_id; /* the player editing scripts, 0 = none */
    struct MpRoom *next;
} MpRoom;

static MpRoom *s_rooms;

static const char *const k_colors[] = {"#ff4d4d", "#4da6ff", "#4cff59", "#ffd24d", "#d24dff", "#4dffe1", "#ff8c4d",
                                       "#ff4dc4", "#a6ff4d", "#4d6bff", "#ffa64d", "#4dff9a"};

/* ------------------------------------------------------------------ rooms and players */

static MpRoom *room_by_id(const char *id) {
    if (!id) return NULL;
    for (MpRoom *r = s_rooms; r; r = r->next)
        if (!strcmp(r->id, id)) return r;
    return NULL;
}

static MpRoom *room_of(const SocialSession *s, MpPlayer **out) {
    for (MpRoom *r = s_rooms; r; r = r->next)
        for (int i = 0; i < r->n; i++)
            if (r->players[i].s == s) {
                if (out) *out = &r->players[i];
                return r;
            }
    return NULL;
}

static MpPlayer *player_by_id(MpRoom *r, const int id) {
    for (int i = 0; i < r->n; i++)
        if (r->players[i].id == id) return &r->players[i];
    return NULL;
}

static cJSON *player_json(const MpPlayer *p) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddStringToObject(o, "color", p->color);
    cJSON_AddNumberToObject(o, "role", p->role);
    return o;
}

static cJSON *json_string_of(cJSON *v) { /* the client JSON.parses these arguments: send them as text */
    char *text = kn_json_print(v);
    cJSON_Delete(v);
    cJSON *s = cJSON_CreateString(text ? text : "null");
    free(text);
    return s;
}

static cJSON *settings_json(const MpRoom *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "maxPlayers", r->max_players);
    cJSON_AddBoolToObject(o, "hasPassword", r->password[0] != 0);
    cJSON_AddBoolToObject(o, "closeOnHostDisconnect", r->close_on_host != 0);
    return o;
}

/* sends a copy of frame to every player but `except` (NULL: everyone); consumes frame */
static void room_send(MpRoom *r, const SocialSession *except, cJSON *frame) {
    for (int i = 0; i < r->n; i++)
        if (r->players[i].s != except) social_send(r->players[i].s, cJSON_Duplicate(frame, 1));
    cJSON_Delete(frame);
}

/* a copy of the client's frame under the same label, its arguments from index `from` on, with `lead`
 * (may be NULL) put first */
static cJSON *relay_frame(const char *label, const cJSON *f, const int from, cJSON *lead) {
    cJSON *out = social_frame(label);
    if (lead) cJSON_AddItemToArray(out, lead);
    for (int i = from; i < cJSON_GetArraySize(f); i++) cJSON_AddItemToArray(out, cJSON_Duplicate(cJSON_GetArrayItem(f, i), 1));
    return out;
}

static void room_close(MpRoom *r);

static void room_free(MpRoom *r) {
    for (MpRoom **pp = &s_rooms; *pp; pp = &(*pp)->next)
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    for (int i = 0; i < r->n; i++) social_release(r->players[i].s);
    free(r);
}

static MpPlayer *player_add(MpRoom *r, SocialSession *s, const int role) {
    if (r->n >= MP_MAX_PLAYERS) return NULL;
    MpPlayer *p = &r->players[r->n++];
    memset(p, 0, sizeof *p);
    p->s = social_retain(s);
    p->id = ++r->next_player;
    p->role = role;
    snprintf(p->name, sizeof p->name, "%s", s->account_id > 0 && s->username[0] ? s->username : "Guest");
    p->color = k_colors[(p->id - 1) % (int) (sizeof k_colors / sizeof k_colors[0])];
    return p;
}

static void send_joined(MpRoom *r, const MpPlayer *me) {
    cJSON *players = cJSON_CreateArray();
    for (int i = 0; i < r->n; i++) cJSON_AddItemToArray(players, player_json(&r->players[i]));
    social_send_n(me->s, "mpRoomJoined", 4, cJSON_CreateString(r->id), json_string_of(players), json_string_of(settings_json(r)),
                  cJSON_CreateNumber(me->id));
}

/* takes the player out of its room: `why` 0 left, 1 kicked, 2 banned; the room closes or gets a new host */
static void player_remove(MpRoom *r, const int id, const int why) {
    int at = -1;
    for (int i = 0; i < r->n; i++)
        if (r->players[i].id == id) at = i;
    if (at < 0) return;
    const MpPlayer gone = r->players[at];
    memmove(&r->players[at], &r->players[at + 1], (size_t) (r->n - at - 1) * sizeof(MpPlayer));
    r->n--;
    if (why == 1) social_send_n(gone.s, "mpKicked", 1, cJSON_CreateString("Kicked by host"));
    else if (why == 2) social_send_n(gone.s, "mpBanned", 1, cJSON_CreateString("Banned by host"));
    if (r->lock_id == gone.id) {
        r->lock_id = 0;
        room_send(r, NULL, relay_frame("mpScriptUnlocked", NULL, 0, cJSON_CreateNumber(gone.id)));
    }
    room_send(r, NULL, relay_frame("mpPlayerLeft", NULL, 0, cJSON_CreateNumber(gone.id)));
    if (gone.role == 1 && r->n > 0 && r->close_on_host) {
        room_send(r, NULL, relay_frame("mpKicked", NULL, 0, cJSON_CreateString("The host closed the room")));
        social_release(gone.s);
        room_close(r);
        return;
    }
    if (gone.role == 1 && r->n > 0) {
        r->players[0].role = 1;
        room_send(r, NULL, relay_frame("mpHostChanged", NULL, 0, cJSON_CreateNumber(r->players[0].id)));
    }
    social_release(gone.s);
    if (r->n == 0) room_free(r);
}

/* every player of a room closed by its host: their references, then the room */
static void room_close(MpRoom *r) {
    for (int i = 0; i < r->n; i++) social_release(r->players[i].s);
    r->n = 0;
    room_free(r);
}

static int banned(const MpRoom *r, const SocialSession *s) {
    for (int i = 0; i < r->nbans; i++) {
        if (r->bans[i].account_id > 0 ? r->bans[i].account_id == s->account_id : !strcmp(r->bans[i].ip, s->ip)) return 1;
    }
    return 0;
}

static void leave_any(SocialSession *s) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (r) player_remove(r, me->id, 0);
}

void editor_rooms_leave(SocialSession *s) { leave_any(s); }

/* ------------------------------------------------------------------ handlers */

static void sh_mp_host(SocialSession *s, const cJSON *f) {
    leave_any(s);
    cJSON *opt = fa_str(f, 1) ? cJSON_Parse(fa_str(f, 1)) : NULL;
    MpRoom *r = calloc(1, sizeof *r);
    if (!r) {
        cJSON_Delete(opt);
        return;
    }
    char hex[24];
    if (kn_random_hex(hex, sizeof hex, 6) != 0) snprintf(hex, sizeof hex, "%p", (void *) r);
    snprintf(r->id, sizeof r->id, "room_%s", hex);
    const cJSON *mp = cJSON_GetObjectItemCaseSensitive(opt, "maxPlayers");
    r->max_players = cJSON_IsNumber(mp) ? (int) mp->valuedouble : 5;
    if (r->max_players < 2) r->max_players = 2;
    if (r->max_players > MP_MAX_PLAYERS) r->max_players = MP_MAX_PLAYERS;
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(opt, "password");
    if (cJSON_IsString(pw)) snprintf(r->password, sizeof r->password, "%s", pw->valuestring);
    const cJSON *close = cJSON_GetObjectItemCaseSensitive(opt, "closeOnHostDisconnect");
    r->close_on_host = !cJSON_IsFalse(close);
    cJSON_Delete(opt);
    r->next = s_rooms;
    s_rooms = r;
    const MpPlayer *me = player_add(r, s, 1);
    send_joined(r, me);
}

static void sh_mp_check(SocialSession *s, const cJSON *f) {
    const char *id = fa_str(f, 1);
    const MpRoom *r = room_by_id(id);
    cJSON *info = cJSON_CreateObject();
    if (!r) cJSON_AddStringToObject(info, "error", "Room not found");
    else {
        cJSON_AddBoolToObject(info, "hasPassword", r->password[0] != 0);
        cJSON_AddNumberToObject(info, "players", r->n);
        cJSON_AddNumberToObject(info, "maxPlayers", r->max_players);
    }
    social_send_n(s, "mpRoomInfo", 2, cJSON_CreateString(id ? id : ""), json_string_of(info));
}

static void mp_error(SocialSession *s, const char *msg) { social_send_n(s, "mpError", 1, cJSON_CreateString(msg)); }

static void sh_mp_join(SocialSession *s, const cJSON *f) {
    MpRoom *r = room_by_id(fa_str(f, 1));
    MpPlayer *cur = NULL;
    if (r && room_of(s, &cur) == r) return; /* already in it */
    if (!r) return mp_error(s, "Room not found");
    if (banned(r, s)) return mp_error(s, "You are banned from this room");
    const char *pw = fa_str(f, 2);
    if (r->password[0] && (!pw || strcmp(pw, r->password))) return mp_error(s, "Incorrect password");
    if (r->n >= r->max_players) return mp_error(s, "Room is full");
    leave_any(s);
    const MpPlayer *me = player_add(r, s, 0);
    if (!me) return mp_error(s, "Room is full");
    send_joined(r, me);
    room_send(r, s, relay_frame("mpPlayerJoined", NULL, 0, json_string_of(player_json(me))));
    const MpPlayer *lock = r->lock_id ? player_by_id(r, r->lock_id) : NULL;
    if (lock) social_send_n(s, "mpScriptLocked", 2, cJSON_CreateNumber(lock->id), cJSON_CreateString(lock->name));
    for (int i = 0; i < r->n; i++)
        if (r->players[i].role == 1) social_send_n(r->players[i].s, "mpRequestMap", 1, cJSON_CreateNumber(me->id));
}

static void sh_mp_leave(SocialSession *s, const cJSON *f) {
    (void) f;
    leave_any(s);
}

static MpRoom *host_room(SocialSession *s, MpPlayer **me) {
    MpRoom *r = room_of(s, me);
    return r && (*me)->role == 1 ? r : NULL;
}

static void sh_mp_kick(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = host_room(s, &me);
    const int id = (int) fa_int(f, 1, 0);
    if (!r || id == me->id || !player_by_id(r, id)) return;
    player_remove(r, id, 1);
}

static void sh_mp_ban(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = host_room(s, &me);
    const int id = (int) fa_int(f, 1, 0);
    MpPlayer *p = r && id != me->id ? player_by_id(r, id) : NULL;
    if (!p) return;
    if (r->nbans < MP_MAX_BANS) {
        MpBan *b = &r->bans[r->nbans++];
        b->account_id = p->s->account_id;
        snprintf(b->ip, sizeof b->ip, "%s", p->s->ip);
    }
    player_remove(r, id, 2);
}

static void sh_mp_settings(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = host_room(s, &me);
    cJSON *in = r && fa_str(f, 1) ? cJSON_Parse(fa_str(f, 1)) : NULL;
    const cJSON *mp = cJSON_GetObjectItemCaseSensitive(in, "maxPlayers");
    if (cJSON_IsNumber(mp)) {
        int v = (int) mp->valuedouble;
        v = v < 2 ? 2 : v > MP_MAX_PLAYERS ? MP_MAX_PLAYERS : v;
        r->max_players = v;
        cJSON *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "maxPlayers", v);
        room_send(r, NULL, relay_frame("mpRoomSettings", NULL, 0, json_string_of(out)));
    }
    cJSON_Delete(in);
}

static void sh_mp_chat(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    const char *t = fa_str(f, 1);
    if (!r || !t) return;
    while (isspace((unsigned char) *t)) t++;
    char text[MP_CHAT_MAX * 4 + 1];
    snprintf(text, sizeof text, "%s", t);
    /* 200 characters (UTF-16 units in the client; code points here), the trailing spaces trimmed */
    size_t cps = 0, i = 0;
    for (; text[i]; i++) {
        if (((unsigned char) text[i] & 0xC0) != 0x80 && ++cps > MP_CHAT_MAX) break;
    }
    text[i] = 0;
    for (size_t n = strlen(text); n && isspace((unsigned char) text[n - 1]); n--) text[n - 1] = 0;
    if (!text[0]) return;
    cJSON *out = social_frame("mpChat");
    cJSON_AddItemToArray(out, cJSON_CreateNumber(me->id));
    cJSON_AddItemToArray(out, cJSON_CreateString(text));
    cJSON_AddItemToArray(out, cJSON_CreateString(me->name));
    cJSON_AddItemToArray(out, cJSON_CreateString(me->color));
    room_send(r, NULL, out);
}

static int vec3_ok(const cJSON *v) {
    if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) != 3) return 0;
    const cJSON *n;
    cJSON_ArrayForEach(n, v) if (!cJSON_IsNumber(n)) return 0;
    return 1;
}

static void sh_mp_move(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (!r || !vec3_ok(fa(f, 1)) || !vec3_ok(fa(f, 2))) return;
    room_send(r, s, relay_frame("mpPlayerMove", f, 1, cJSON_CreateNumber(me->id)));
}

static void sh_mp_select(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (!r) return;
    room_send(r, s, relay_frame("mpPlayerSelect", f, 1, cJSON_CreateNumber(me->id)));
}

static void sh_mp_object_added(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (!r || !fa_str(f, 1) || !fa_str(f, 2)) return;
    const int sid = ++r->next_sid;
    social_send_n(s, "mpObjectConfirmed", 2, cJSON_CreateString(fa_str(f, 1)), cJSON_CreateNumber(sid));
    cJSON *out = social_frame("mpObjectAdded");
    cJSON_AddItemToArray(out, cJSON_CreateNumber(sid));
    cJSON_AddItemToArray(out, cJSON_CreateString(fa_str(f, 2)));
    cJSON_AddItemToArray(out, cJSON_CreateNumber(me->id));
    room_send(r, s, out);
}

/* mpObjectRemoved / mpObjectUpdated / mpObjectsMoved / mpGroupRemoved: as they came, to the others */
static void relay_others(SocialSession *s, const cJSON *f) {
    MpRoom *r = room_of(s, NULL);
    if (!r) return;
    room_send(r, s, relay_frame(fa_str(f, 0), f, 1, NULL));
}

static void sh_mp_clear(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = host_room(s, &me);
    if (r) room_send(r, s, relay_frame("mpClearMap", f, 1, NULL));
}

static void sh_mp_sync(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    cJSON *map = r && fa_str(f, 1) ? cJSON_Parse(fa_str(f, 1)) : NULL;
    if (!cJSON_IsObject(map)) {
        cJSON_Delete(map);
        return;
    }
    cJSON *sids = cJSON_CreateArray();
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(map, "objects")) {
        if (!cJSON_IsObject(o)) continue;
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(o, "_sid");
        int v;
        if (cJSON_IsNumber(sid)) {
            v = (int) sid->valuedouble;
            if (v > r->next_sid) r->next_sid = v;
        } else {
            v = ++r->next_sid;
            cJSON_DeleteItemFromObjectCaseSensitive(o, "_sid");
            cJSON_AddNumberToObject(o, "_sid", v);
        }
        cJSON_AddItemToArray(sids, cJSON_CreateNumber(v));
    }
    social_send_n(s, "mpSyncMapConfirmed", 1, json_string_of(sids));
    cJSON *out = social_frame("mpSyncMap");
    cJSON_AddItemToArray(out, json_string_of(map));
    const cJSON *target = fa(f, 2);
    MpPlayer *to = cJSON_IsNumber(target) && me->role == 1 ? player_by_id(r, (int) target->valuedouble) : NULL;
    if (to) social_send(to->s, out);
    else if (cJSON_IsNumber(target)) cJSON_Delete(out);
    else room_send(r, s, out);
}

static void sh_mp_lock(SocialSession *s, const cJSON *f) {
    (void) f;
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (!r) return;
    const MpPlayer *holder = r->lock_id && r->lock_id != me->id ? player_by_id(r, r->lock_id) : NULL;
    if (holder) return social_send_n(s, "mpScriptLockDenied", 1, cJSON_CreateString(holder->name));
    r->lock_id = me->id;
    social_send_n(s, "mpScriptLockAcquired", 0);
    cJSON *out = social_frame("mpScriptLocked");
    cJSON_AddItemToArray(out, cJSON_CreateNumber(me->id));
    cJSON_AddItemToArray(out, cJSON_CreateString(me->name));
    room_send(r, s, out);
}

static void sh_mp_unlock(SocialSession *s, const cJSON *f) {
    (void) f;
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (!r || r->lock_id != me->id) return;
    r->lock_id = 0;
    room_send(r, s, relay_frame("mpScriptUnlocked", NULL, 0, cJSON_CreateNumber(me->id)));
}

static void sh_mp_script(SocialSession *s, const cJSON *f) {
    MpPlayer *me = NULL;
    MpRoom *r = room_of(s, &me);
    if (!r || r->lock_id != me->id || !fa_str(f, 1)) return;
    room_send(r, s, relay_frame("mpScriptUpdate", f, 1, NULL));
}

const EditorRoomRoute k_editor_room_routes[] = {
    {"mpHost", sh_mp_host},
    {"mpCheckRoom", sh_mp_check},
    {"mpJoin", sh_mp_join},
    {"mpLeave", sh_mp_leave},
    {"mpKick", sh_mp_kick},
    {"mpBan", sh_mp_ban},
    {"mpRoomSettings", sh_mp_settings},
    {"mpChat", sh_mp_chat},
    {"mpPlayerMove", sh_mp_move},
    {"mpPlayerSelect", sh_mp_select},
    {"mpObjectAdded", sh_mp_object_added},
    {"mpObjectRemoved", relay_others},
    {"mpObjectUpdated", relay_others},
    {"mpObjectsMoved", relay_others},
    {"mpGroupRemoved", relay_others},
    {"mpClearMap", sh_mp_clear},
    {"mpSyncMap", sh_mp_sync},
    {"mpScriptLock", sh_mp_lock},
    {"mpScriptUnlock", sh_mp_unlock},
    {"mpScriptUpdate", sh_mp_script},
    {NULL, NULL},
};
