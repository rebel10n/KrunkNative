// Map editor multiplayer: Krunker v10.0.7's KE.multiplayer (editor-10.0.7.js:8982-10294): rooms of
// editors co-editing one map over the editor's socket, ported line by line.
//
//   ObjectSync (:8982-9230)    the room's object numbers (sid): temp ids until the server confirms,
//                              the apply* of every remote change, getMapExportWithSIDs
//   Player / PlayerManager     remote editors as translucent 1 x 2 x 1 boxes in their colour with a
//   (:9231-9443)               name label, and a pulsing outline on the object each one has selected
//   ChatUI (:9474-9590)        #mp-chat over the 3D view: system lines, "name: text", @obj
//   windows (:9591-9690)       16 "Multiplayer" (the host form, or the room with Copy Invite Link /
//                              Player List / Disconnect) and 17 "Players" (TP / SEL / KICK / BAN)
//   Multiplayer (:9696-10294)  the editor events it sends on, the 28 socket handlers, host / join /
//                              password check / invite links / script lock / camera every 200 ms
//
// The server side is kn_brain's room relay (services/brain/editor_rooms.c) on the same social socket
// (Krunker's own is editor.krunker.io/ws). Hooks: editor_objects.c (objectAdded / objectRemoved /
// objectSelected / groupRemoved / objectPropertyChanged), editor_render.c (transformEnd and the
// drawing), editor_io.c (mapCleared, mapImportStart / mapImportEnded), editor_script.c (the lock),
// editor.c (the page, Toggle Player List / Focus Chat keys), editor_panels.c (windows 16 / 17).
//
// Intentional differences from v10.0.7:
// - Inline handlers v10 writes as JavaScript statements (the Max Players slider's
//   "document.getElementById('mpMaxPlayersVal').textContent=this.value;KE.multiplayer.updateRoomSetting(
//   'maxPlayers',parseInt(this.value))", Player List's showWindow(KE.multiplayer.playerListWindowIndex))
//   are calls the native page can dispatch: KE.multiplayer.maxPlayersInput(this.value[, 1]), showWindow(17).
// - alert() is a line in the editor's #chatList (editor_message), prompt() / confirm() the editor's
//   in-page dialog (editor_prompt.c), as everywhere in the native editor.
// - The invite link is <origin>/editor.html?room=<id> with the public site's origin natively (the
//   window has no address bar: history.replaceState is web only); the room id the URL would keep is
//   kept here, so a reconnect rejoins as v10's does from its URL.
// - Remote changes apply to the project that joined the room (KrunkNative edits several maps in one
//   page); events of the other projects are not sent.
#include "editor_internal.h"
#include <backend.h>
#include <canvas2d.h>
#include <html_dom.h>
#include <html_ui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP_MAX_PLAYERS 24

typedef struct {
    int id;
    char name[48];
    char color[16];
    int role;           // 1 = host
    int local;
    float pos[3], rot[3];
    int has_sel;        // selectedSID !== null
    int sel;            // -1 = none
} MpPlayer;

typedef struct { int sid; uint32_t uuid; } MpSid;
typedef struct { char tmp[24]; uint32_t uuid; } MpTmp;

static struct {
    int listening, installed;
    // room
    int in_room;
    char room_id[96];
    int max_players;
    MpPlayer players[MP_MAX_PLAYERS];
    int nplayers;
    int me;                 // currentPlayer id (0 none)
    EditorProject *proj;    // the project in the room
    int pending_sync;       // pendingSync
    int importing;          // _isImporting
    int mute;               // removeObject(..., true): no dispatch
    // ObjectSync
    MpSid *sids; size_t nsids, capsids;
    MpTmp *tmps; size_t ntmps, captmps;
    int temp_counter;
    // scripts
    int lock_id;            // scriptLockedBy.id (0 none)
    char lock_name[48];
    int lock_pending;
    int force_popup;        // scriptPopup(true)
    // camera (_startMoveInterval)
    double move_next;
    int have_last;
    float last_pos[3], last_rot[3];
    // password check (checkRoom) and the URL's room
    char check_room[96];
    char url_room[96];
    int was_open;
    char script_pw[128];
    int has_script_pw;
    int sel_sent;           // the last mpPlayerSelect (-2 none yet)
    // chat
    HtmlElem chat, chat_list, chat_input;
    int chat_visible;
} g_mp;

static int me_is_host(void);

static int proj_alive(void) { // the room's map is still open (KrunkNative closes maps in one page)
    for (size_t i = 0; i < g_ed.project_count; i++)
        if (g_ed.projects[i] == g_mp.proj) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// mp_settings (localStorage "mp_settings", editor_panels.c ed_mp_settings)

static int setting_bool(const char *key, const int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(ed_mp_settings(), key);
    return v ? cJSON_IsTrue(v) : def;
}

static void setting_set_str(const char *key, const char *value) {
    cJSON *s = ed_mp_settings();
    cJSON_DeleteItemFromObjectCaseSensitive(s, key);
    cJSON_AddItemToObject(s, key, value ? cJSON_CreateString(value) : cJSON_CreateNull());
    ed_mp_settings_save();
}

static const char *setting_str(const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(ed_mp_settings(), key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// ---------------------------------------------------------------------------
// socket

static void on_frame(void *user, const cJSON *frame);

static void mp_send(cJSON *f) {
    if (!g_mp.listening) g_mp.listening = backend_social_listen(on_frame, NULL);
    backend_social_open();
    backend_social_send(f);
    cJSON_Delete(f);
}

static cJSON *frame(const char *label) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    return f;
}

static void send0(const char *label) { mp_send(frame(label)); }

static void send_num(const char *label, const double v) {
    cJSON *f = frame(label);
    cJSON_AddItemToArray(f, cJSON_CreateNumber(v));
    mp_send(f);
}

static void send_str(const char *label, const char *s) {
    cJSON *f = frame(label);
    cJSON_AddItemToArray(f, cJSON_CreateString(s ? s : ""));
    mp_send(f);
}

static void send_json(const char *label, cJSON *v) { // JSON.stringify(v); consumes v
    char *t = cJSON_PrintUnformatted(v);
    cJSON_Delete(v);
    send_str(label, t ? t : "{}");
    free(t);
}

static cJSON *parse_arg(const cJSON *a) { // typeof x == "string" ? JSON.parse(x) : x (owned)
    if (cJSON_IsString(a)) return cJSON_Parse(a->valuestring);
    return a ? cJSON_Duplicate(a, 1) : NULL;
}

// ---------------------------------------------------------------------------
// ObjectSync

static int sid_of(const EditorObject *o) { // getSid: null = -1
    if (!o) return -1;
    for (size_t i = 0; i < g_mp.nsids; i++)
        if (g_mp.sids[i].uuid == o->uuid) return g_mp.sids[i].sid;
    return -1;
}

static EditorObject *object_of(const int sid) {
    for (size_t i = 0; i < g_mp.nsids; i++)
        if (g_mp.sids[i].sid == sid) return ed_obj_find(g_mp.sids[i].uuid);
    return NULL;
}

static void sid_drop(const int sid) { // removeSid
    for (size_t i = 0; i < g_mp.nsids; i++)
        if (g_mp.sids[i].sid == sid) {
            g_mp.sids[i] = g_mp.sids[--g_mp.nsids];
            return;
        }
}

static void sid_register(const int sid, const EditorObject *o) { // registerObject
    if (!o) return;
    for (size_t i = 0; i < g_mp.nsids; i++)
        if (g_mp.sids[i].sid == sid || g_mp.sids[i].uuid == o->uuid) {
            g_mp.sids[i] = g_mp.sids[--g_mp.nsids];
            i--;
        }
    if (g_mp.nsids == g_mp.capsids) {
        const size_t cap = g_mp.capsids ? g_mp.capsids * 2 : 64;
        MpSid *n = realloc(g_mp.sids, cap * sizeof(*n));
        if (!n) return;
        g_mp.sids = n;
        g_mp.capsids = cap;
    }
    g_mp.sids[g_mp.nsids++] = (MpSid) {sid, o->uuid};
}

static void sync_clear(void) { // clear() / the sidMap, objectToSid, tempIdMap resets
    g_mp.nsids = 0;
    g_mp.ntmps = 0;
}

static const char *temp_id(const EditorObject *o) { // assignTempId
    if (g_mp.ntmps == g_mp.captmps) {
        const size_t cap = g_mp.captmps ? g_mp.captmps * 2 : 32;
        MpTmp *n = realloc(g_mp.tmps, cap * sizeof(*n));
        if (!n) return "tmp_0";
        g_mp.tmps = n;
        g_mp.captmps = cap;
    }
    MpTmp *t = &g_mp.tmps[g_mp.ntmps++];
    snprintf(t->tmp, sizeof(t->tmp), "tmp_%d", ++g_mp.temp_counter);
    t->uuid = o->uuid;
    return t->tmp;
}

static void confirm_sid(const char *tmp, const int sid) {
    for (size_t i = 0; i < g_mp.ntmps; i++)
        if (!strcmp(g_mp.tmps[i].tmp, tmp)) {
            const uint32_t uuid = g_mp.tmps[i].uuid;
            g_mp.tmps[i] = g_mp.tmps[--g_mp.ntmps];
            EditorProject *prev = NULL;
            ed_project_enter(g_mp.proj, &prev);
            const EditorObject *o = ed_obj_find(uuid);
            if (o) sid_register(sid, o);
            ed_project_leave(prev);
            return;
        }
}

// getMapExportWithSIDs
static char *map_export_with_sids(void) {
    const EditorProject *p = g_ed.proj;
    cJSON *doc = cJSON_CreateObject();
    cJSON *objs = cJSON_AddArrayToObject(doc, "objects");
    for (size_t i = 0; i < p->count; i++) {
        cJSON *o = ed_obj_serialize(p->objects[i]);
        const int sid = sid_of(p->objects[i]);
        if (sid >= 0) cJSON_AddNumberToObject(o, "_sid", sid);
        cJSON_AddItemToArray(objs, o);
    }
    cJSON *ar = ed_archetypes_snapshot();
    cJSON_AddItemToObject(doc, "archetypes", cJSON_IsArray(ar) ? ar : (cJSON_Delete(ar), cJSON_CreateArray()));
    cJSON_AddItemToObject(doc, "config", p->map_config ? cJSON_Duplicate(p->map_config, 1) : cJSON_CreateObject());
    cJSON_AddItemToObject(doc, "serverConfig", p->server_config ? cJSON_Duplicate(p->server_config, 1) : cJSON_CreateObject());
    cJSON *scripts = cJSON_AddObjectToObject(doc, "scripts");
    const cJSON *s;
    cJSON_ArrayForEach(s, p->scripts) cJSON_AddStringToObject(scripts, s->string, cJSON_IsString(s) ? s->valuestring : "");
    char *t = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return t;
}

static void assign_into(cJSON *dst, const cJSON *src) { // Object.assign(dst, src)
    if (!cJSON_IsObject(dst) || !cJSON_IsObject(src)) return;
    const cJSON *it;
    cJSON_ArrayForEach(it, src) {
        cJSON_DeleteItemFromObjectCaseSensitive(dst, it->string);
        cJSON_AddItemToObject(dst, it->string, cJSON_Duplicate(it, 1));
    }
}

static void regen_all(void) { // regenAll + the panels
    editor_apply_lighting();
    ed_terrain_regen();
    ed_zone_regen();
    ed_panel_refresh_all();
    g_ed.dirty_counts = 1;
}

// KE.scripts from the room: each side it sends (the script window follows the map's revision)
static void scripts_assign(const cJSON *src) {
    if (!cJSON_IsObject(src) || !g_ed.proj->scripts) return;
    const cJSON *s;
    cJSON_ArrayForEach(s, src) {
        if (!cJSON_IsString(s)) continue;
        cJSON *v = cJSON_CreateString(s->valuestring);
        if (cJSON_GetObjectItemCaseSensitive(g_ed.proj->scripts, s->string)) cJSON_ReplaceItemInObjectCaseSensitive(g_ed.proj->scripts, s->string, v);
        else cJSON_AddItemToObject(g_ed.proj->scripts, s->string, v);
    }
    ed_mark_changed();
}

static void apply_object_added(const int sid, const cJSON *json) {
    cJSON *d = parse_arg(json);
    g_mp.pending_sync = 1;
    if (cJSON_IsObject(d)) {
        EditorObject *o = ed_obj_new(d, 0);
        ed_obj_add(o, 1, 0);
        sid_register(sid, o);
    }
    g_mp.pending_sync = 0;
    cJSON_Delete(d);
}

static void apply_object_removed(const cJSON *f) {
    g_mp.pending_sync = 1;
    for (int i = 1; i < cJSON_GetArraySize(f); i++) {
        const int sid = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(f, i));
        EditorObject *o = object_of(sid);
        if (!o) continue;
        if (g_ed.proj->selected == o) ed_obj_select(NULL); // hideTransform
        ed_obj_remove(o, 1, 0);
        sid_drop(sid);
    }
    g_mp.pending_sync = 0;
}

static void apply_object_updated(const int sid, const cJSON *changes_arg) {
    cJSON *changes = parse_arg(changes_arg);
    EditorObject *o = object_of(sid);
    if (o && cJSON_IsObject(changes)) {
        g_mp.pending_sync = 1;
        cJSON *data = ed_obj_serialize(o);
        const cJSON *c;
        cJSON_ArrayForEach(c, changes) {
            cJSON_DeleteItemFromObjectCaseSensitive(data, c->string);
            if (!cJSON_IsNull(c)) cJSON_AddItemToObject(data, c->string, cJSON_Duplicate(c, 1));
        }
        const int selected = g_ed.proj->selected == o;
        if (selected) ed_obj_select(NULL);
        ed_obj_remove(o, 1, 0);
        sid_drop(sid);
        EditorObject *n = ed_obj_new(data, 0);
        ed_obj_add(n, 1, 0);
        sid_register(sid, n);
        if (selected) ed_obj_select(n); // attachTransform + updateObjConfigGUI
        cJSON_Delete(data);
        g_mp.pending_sync = 0;
    }
    cJSON_Delete(changes);
}

static void set_vec_from(EditorObject *o, const char *key, const cJSON *a) {
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 3) return;
    ed_obj_set_vec(o, key, (vec3) {(float) cJSON_GetNumberValue(cJSON_GetArrayItem(a, 0)), (float) cJSON_GetNumberValue(cJSON_GetArrayItem(a, 1)),
                                   (float) cJSON_GetNumberValue(cJSON_GetArrayItem(a, 2))});
}

static void apply_objects_moved(const cJSON *arg) {
    cJSON *list = parse_arg(arg);
    g_mp.pending_sync = 1;
    const cJSON *m;
    cJSON_ArrayForEach(m, list) {
        EditorObject *o = object_of((int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "sid")));
        if (!o) continue;
        set_vec_from(o, "p", cJSON_GetObjectItemCaseSensitive(m, "p"));
        set_vec_from(o, "r", cJSON_GetObjectItemCaseSensitive(m, "r"));
        set_vec_from(o, "s", cJSON_GetObjectItemCaseSensitive(m, "s"));
        ed_obj_rebuild(o);
        ed_mark_changed();
    }
    g_mp.pending_sync = 0;
    cJSON_Delete(list);
}

static void apply_clear_map(void) {
    g_mp.pending_sync = 1;
    ed_map_clear();
    sync_clear();
    g_mp.pending_sync = 0;
}

static void apply_full_map_sync(const cJSON *arg) {
    cJSON *doc = parse_arg(arg);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        return;
    }
    g_mp.pending_sync = 1;
    ed_map_clear();
    sync_clear();
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(doc, "objects")) {
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(o, "_sid");
        const int has = cJSON_IsNumber(sid);
        const int v = has ? (int) sid->valuedouble : 0;
        cJSON_DeleteItemFromObjectCaseSensitive(o, "_sid");
        EditorObject *n = ed_obj_new(o, 0);
        ed_obj_add(n, 1, 0);
        if (has) sid_register(v, n);
    }
    const cJSON *ar = cJSON_GetObjectItemCaseSensitive(doc, "archetypes");
    if (cJSON_IsArray(ar)) ed_archetypes_restore(ar);
    assign_into(g_ed.proj->map_config, cJSON_GetObjectItemCaseSensitive(doc, "config"));
    assign_into(g_ed.proj->server_config, cJSON_GetObjectItemCaseSensitive(doc, "serverConfig"));
    scripts_assign(cJSON_GetObjectItemCaseSensitive(doc, "scripts"));
    ed_obj_select(NULL); // hideTransform
    regen_all();
    g_mp.pending_sync = 0;
    cJSON_Delete(doc);
}

// ---------------------------------------------------------------------------
// players

static MpPlayer *player(const int id) {
    for (int i = 0; i < g_mp.nplayers; i++)
        if (g_mp.players[i].id == id) return &g_mp.players[i];
    return NULL;
}

static MpPlayer *current(void) { return g_mp.me ? player(g_mp.me) : NULL; }

static int me_is_host(void) {
    const MpPlayer *p = current();
    return p && p->role == 1;
}

static void add_player(const cJSON *j, const int local) { // addPlayer: the Map keeps one per id
    const int id = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "id"));
    MpPlayer *p = player(id);
    if (!p) {
        if (g_mp.nplayers >= MP_MAX_PLAYERS) return;
        p = &g_mp.players[g_mp.nplayers++];
    }
    memset(p, 0, sizeof(*p));
    p->id = id;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(j, "name"), *color = cJSON_GetObjectItemCaseSensitive(j, "color");
    snprintf(p->name, sizeof(p->name), "%s", cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : "Player");
    snprintf(p->color, sizeof(p->color), "%s", cJSON_IsString(color) && color->valuestring[0] ? color->valuestring : "#ffffff");
    p->role = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "role"));
    p->local = local;
    p->sel = -1;
}

static void remove_player(const int id) {
    for (int i = 0; i < g_mp.nplayers; i++)
        if (g_mp.players[i].id == id) {
            memmove(&g_mp.players[i], &g_mp.players[i + 1], (size_t) (g_mp.nplayers - i - 1) * sizeof(MpPlayer));
            g_mp.nplayers--;
            if (g_mp.me == id) g_mp.me = 0;
            return;
        }
}

static int mp_hex_color(const char *c) {
    if (!c) return 0xffffff;
    if (*c == '#') c++;
    const size_t n = strlen(c);
    int v = (int) strtol(c, NULL, 16);
    if (n == 3) v = ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17;
    return v;
}

// ---------------------------------------------------------------------------
// chat (ChatUI)

static void chat_keydown_bind(void);

static void chat_build(void) {
    if (g_mp.chat) return;
    const HtmlElem center = html_dom_by_id("center");
    if (!center) return;
    html_dom_insert_html(center, HTML_INSERT_BEFORE_END,
                         "<div id=\"mp-chat\" style=\"position:absolute;bottom:80px;left:20px;width:280px;z-index:1000;display:none;flex-direction:column;pointer-events:auto;\">"
                         "<div id=\"mp-chat-list\" style=\"max-height:200px;overflow-y:auto;background:rgba(0,0,0,0.4);border-radius:4px 4px 0 0;padding:6px;font-size:12px;color:#fff;font-family:sans-serif;\"></div>"
                         "<input id=\"mp-chat-input\" type=\"text\" maxlength=\"200\" placeholder=\"Type a message...\" style=\"width:100%;box-sizing:border-box;padding:6px 8px;border:none;background:rgba(0,0,0,0.6);color:#fff;font-size:12px;border-radius:0 0 4px 4px;outline:none;font-family:sans-serif;\"></div>");
    g_mp.chat = html_dom_by_id("mp-chat");
    g_mp.chat_list = html_dom_by_id("mp-chat-list");
    g_mp.chat_input = html_dom_by_id("mp-chat-input");
    chat_keydown_bind();
}

static void chat_show(const int on) {
    chat_build();
    g_mp.chat_visible = on;
    if (g_mp.chat) html_dom_set_display(g_mp.chat, on ? "flex" : "none");
}

static void esc_into(EdBuf *b, const char *s) { edb_add_escaped(b, s ? s : ""); }

typedef struct { const char *text, *color; int bold, italic; } ChatPart;

static void chat_add(const ChatPart *parts, const int n) { // addMessage
    chat_build();
    if (!g_mp.chat_list) return;
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div style=\"margin-bottom:3px;line-height:1.4;word-wrap:break-word;\">");
    for (int i = 0; i < n; i++) {
        edb_add(&b, "<span style=\"");
        if (parts[i].color) edb_addf(&b, "color:%s;", parts[i].color);
        if (parts[i].bold) edb_add(&b, "font-weight:bold;");
        if (parts[i].italic) edb_add(&b, "font-style:italic;");
        edb_add(&b, "\">");
        esc_into(&b, parts[i].text);
        edb_add(&b, "</span>");
    }
    edb_add(&b, "</div>");
    html_dom_insert_html(g_mp.chat_list, HTML_INSERT_BEFORE_END, b.buf ? b.buf : "");
    edb_free(&b);
    if (setting_bool("autoChatScroll", 1)) html_dom_scroll_to_bottom(g_mp.chat_list);
}

static void system_message(const char *text) { // addSystemMessage
    const ChatPart p = {text, "#4cff59", 0, 1};
    chat_add(&p, 1);
}

static void system_messagef(const char *fmt, const char *a) {
    char t[256];
    snprintf(t, sizeof(t), fmt, a);
    system_message(t);
}

static void chat_message(const char *name, const char *color, const char *text) { // addChatMessage
    char who[64];
    snprintf(who, sizeof(who), "%s: ", name ? name : "");
    const ChatPart p[2] = {{who, color, 1, 0}, {text, "#ddd", 0, 0}};
    chat_add(p, 2);
}

static void chat_send(void) { // sendMessage
    char raw[1024] = "";
    html_dom_get_value(g_mp.chat_input, raw, sizeof(raw));
    char *t = raw;
    while (*t == ' ' || *t == '\t' || *t == '\n') t++;
    for (size_t n = strlen(t); n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n'); n--) t[n - 1] = 0;
    if (!*t) return;
    char msg[2048];
    snprintf(msg, sizeof(msg), "%s", t);
    const EditorObject *sel = g_ed.proj ? g_ed.proj->selected : NULL;
    if (strstr(msg, "@obj") && sel) {
        const int sid = sid_of(sel);
        char rep[96], out[2048] = "";
        if (sid >= 0) snprintf(rep, sizeof(rep), "%s::%d", editor_prefabs[sel->prefab].name, sid);
        else snprintf(rep, sizeof(rep), "%s::?", editor_prefabs[sel->prefab].name);
        const char *p = msg;
        size_t o = 0;
        while (*p && o + 1 < sizeof(out)) {
            if (!strncmp(p, "@obj", 4)) {
                o += (size_t) snprintf(out + o, sizeof(out) - o, "%s", rep);
                if (o >= sizeof(out)) o = sizeof(out) - 1;
                p += 4;
            } else {
                out[o++] = *p++;
                out[o] = 0;
            }
        }
        snprintf(msg, sizeof(msg), "%s", out);
    }
    if (g_mp.in_room) send_str("mpChat", msg); // sendChatMessage
    html_dom_set_value(g_mp.chat_input, "");
}

static int l_chat_keydown(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    html_ui_stop_propagation();
    if (html_key_is_enter(ev->key)) {
        chat_send();
        html_dom_blur(g_mp.chat_input);
    } else if (html_key_is_escape(ev->key)) {
        html_dom_blur(g_mp.chat_input);
    }
    return 1;
}

static void chat_keydown_bind(void) { html_ui_set_listener("mp-chat-input", "keydown", l_chat_keydown); }

// ---------------------------------------------------------------------------
// windows 16 / 17

static void url_update(const char *room) { // updateURL
    if (!room) return;
    const int streamer = setting_bool("streamerMode", 0);
    if (streamer) setting_set_str("_lastRoomId", room);
    snprintf(g_mp.url_room, sizeof(g_mp.url_room), "%s", room);
    char href[256];
    snprintf(href, sizeof(href), "?room=%s", streamer ? "***" : room);
    html_page_set_url(href, 0);
}

static void url_remove(void) { // removeURLRoom
    g_mp.url_room[0] = 0;
    html_page_set_url("?", 0);
    setting_set_str("_lastRoomId", NULL);
}

static void toolbar_state(void) { // _updateToolbarState
    const HtmlElem icon = html_dom_by_id("multiplayerIcon");
    if (icon) html_dom_set_style(icon, "color", g_mp.in_room ? "#0f0" : "");
}

void ed_mp_window_gen(const int index, EdBuf *h) {
    if (index == 17) { // the player list (iìíiiìì)
        if (!g_mp.in_room) {
            edb_add(h, "<div style=\"padding:10px;color:#aaa\">Not in a room</div>");
            return;
        }
        edb_addf(h, "<div class=\"windowHeader\"><div>Players [%d/", g_mp.nplayers);
        if (g_mp.max_players) edb_addf(h, "%d", g_mp.max_players);
        else edb_add(h, "?");
        edb_add(h, "]</div></div><div style=\"padding:10px\">");
        const MpPlayer *cur = current();
        const int host = cur && cur->role == 1;
        for (int i = 0; i < g_mp.nplayers; i++) {
            const MpPlayer *p = &g_mp.players[i];
            const int you = cur && p->id == cur->id;
            edb_add(h, "<div style=\"display:flex;align-items:center;padding:6px;margin-bottom:4px;\n\t\t\tbackground:rgba(255,255,255,0.05);border-radius:4px;border-left:3px solid ");
            esc_into(h, p->color);
            edb_add(h, "\"><span style=\"flex:1;color:");
            esc_into(h, p->color);
            edb_add(h, ";font-weight:bold\">");
            esc_into(h, p->name);
            if (p->role == 1) edb_add(h, " <span style=\"color:#ffd700;font-size:10px\">[HOST]</span>");
            if (you) edb_add(h, " <span style=\"color:#888;font-size:10px\">(you)</span>");
            edb_add(h, "</span>");
            if (!you) {
                edb_addf(h, "<button class=\"mpBtn\" onclick=\"KE.multiplayer.playerManager.teleportToPlayer(%d)\" title=\"Teleport\">TP</button>", p->id);
                edb_addf(h, "<button class=\"mpBtn\" onclick=\"KE.multiplayer.playerManager.selectPlayerObject(%d)\" title=\"Select Object\">SEL</button>", p->id);
                if (host) {
                    edb_addf(h, "<button class=\"mpBtn mpBtnDanger\" onclick=\"KE.multiplayer.kick(%d)\" title=\"Kick\">KICK</button>", p->id);
                    edb_addf(h, "<button class=\"mpBtn mpBtnDanger\" onclick=\"KE.multiplayer.ban(%d)\" title=\"Ban\">BAN</button>", p->id);
                }
            }
            edb_add(h, "</div>");
        }
        edb_add(h, "</div>");
        return;
    }
    // the Multiplayer window (iìîìîïì)
    edb_add(h, "<div style=\"padding:10px\">");
    if (!g_mp.in_room) {
        edb_add(h, "\n\t\t\t<div style=\"margin-bottom:15px;padding:10px;background:rgba(255,200,0,0.1);border:1px solid rgba(255,200,0,0.3);border-radius:4px;color:#ffd700;font-size:12px\">\n\t\t\t\tOnly invite people you trust - they have full control over the map\n\t\t\t</div>"
                   "\n\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t<label style=\"display:block;margin-bottom:4px;color:#aaa;font-size:12px\">Max Players</label>\n\t\t\t\t<input type=\"range\" id=\"mpMaxPlayers\" min=\"2\" max=\"20\" value=\"5\" style=\"width:100%\"\n\t\t\t\t\toninput=\"KE.multiplayer.maxPlayersInput(this.value)\" />\n\t\t\t\t<span id=\"mpMaxPlayersVal\" style=\"color:#fff\">5</span>\n\t\t\t</div>"
                   "\n\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t<label style=\"display:block;margin-bottom:4px;color:#aaa;font-size:12px\">Room Password (optional)</label>\n\t\t\t\t<input type=\"text\" id=\"mpPassword\" placeholder=\"Leave empty for no password\"\n\t\t\t\t\tstyle=\"width:100%;box-sizing:border-box;padding:6px;background:#333;color:#fff;border:1px solid #555;border-radius:3px\" />\n\t\t\t</div>"
                   "\n\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t<label style=\"color:#aaa;font-size:12px\">\n\t\t\t\t\t<input type=\"checkbox\" id=\"mpCloseOnDisconnect\" checked /> Close room when host disconnects\n\t\t\t\t</label>\n\t\t\t</div>"
                   "\n\t\t\t<div style=\"display:flex;gap:8px\">\n\t\t\t\t<button class=\"mpBtnPrimary\" onclick=\"KE.multiplayer.hostFromUI()\" style=\"flex:1\">Host Room</button>\n\t\t\t\t<button class=\"mpBtnPrimary\" onclick=\"KE.multiplayer.joinPrompt()\" style=\"flex:1\">Join Room</button>\n\t\t\t</div>\n\t\t");
    } else {
        edb_add(h, "\n\t\t\t<div style=\"margin-bottom:10px;color:#aaa;font-size:12px\">\n\t\t\t\tRoom: <span style=\"color:#fff;font-family:monospace\">");
        if (setting_bool("streamerMode", 0)) edb_add(h, "***...***");
        else esc_into(h, g_mp.room_id);
        edb_add(h, "</span>\n\t\t\t</div>\n\t\t");
        if (me_is_host()) {
            const int mp = g_mp.max_players ? g_mp.max_players : 5;
            edb_addf(h, "\n\t\t\t\t<div style=\"margin-bottom:10px\">\n\t\t\t\t\t<label style=\"display:block;margin-bottom:4px;color:#aaa;font-size:12px\">Max Players</label>\n\t\t\t\t\t<input type=\"range\" id=\"mpMaxPlayers\" min=\"2\" max=\"20\" value=\"%d\" style=\"width:100%%\"\n\t\t\t\t\t\toninput=\"KE.multiplayer.maxPlayersInput(this.value,1)\" />\n\t\t\t\t\t<span id=\"mpMaxPlayersVal\" style=\"color:#fff\">%d</span>\n\t\t\t\t</div>\n\t\t\t", mp, mp);
        }
        edb_add(h, "\n\t\t\t<div style=\"display:flex;gap:8px;flex-wrap:wrap\">\n\t\t\t\t<button class=\"mpBtnPrimary\" onclick=\"KE.multiplayer.copyRoomURL()\">Copy Invite Link</button>\n\t\t\t\t<button class=\"mpBtnPrimary\" onclick=\"showWindow(17)\">Player List</button>\n\t\t\t\t<button class=\"mpBtnDanger\" onclick=\"KE.multiplayer.disconnect()\" style=\"margin-left:auto\">Disconnect</button>\n\t\t\t</div>\n\t\t");
    }
    edb_add(h, "</div>");
}

// ---------------------------------------------------------------------------
// room lifecycle

static void stop_moves(void) { g_mp.move_next = 0; g_mp.have_last = 0; }

static void release_script_lock(void) { // releaseScriptLock
    const MpPlayer *cur = current();
    if (g_mp.in_room && g_mp.lock_id && cur && g_mp.lock_id == cur->id) {
        // sendScriptUpdate: the scripts as they are now
        cJSON *s = cJSON_CreateObject();
        const cJSON *it;
        if (g_mp.proj && proj_alive()) cJSON_ArrayForEach(it, g_mp.proj->scripts) cJSON_AddStringToObject(s, it->string, cJSON_IsString(it) ? it->valuestring : "");
        send_json("mpScriptUpdate", s);
        send0("mpScriptUnlock");
        g_mp.lock_id = 0;
    }
}

static void cleanup(void) { // _cleanup
    stop_moves();
    release_script_lock();
    g_mp.lock_id = 0;
    g_mp.lock_pending = 0;
    g_mp.nplayers = 0; // playerManager.clearAll
    sync_clear();
    g_mp.temp_counter = 0;
    chat_show(0);
    if (g_mp.chat_list) html_dom_set_html(g_mp.chat_list, "");
    g_mp.in_room = 0;
    g_mp.room_id[0] = 0;
    g_mp.me = 0;
    g_mp.proj = NULL;
    url_remove();
    toolbar_state();
}

static void join(const char *room, const char *pw) {
    cJSON *f = frame("mpJoin");
    cJSON_AddItemToArray(f, cJSON_CreateString(room));
    cJSON_AddItemToArray(f, cJSON_CreateString(pw ? pw : ""));
    mp_send(f);
}

static void password_cb(void *user, const char *value) {
    (void) user;
    join(g_mp.check_room, value ? value : "");
}

static void join_with_password_check(const char *room) { // joinWithPasswordCheck: checkRoom first
    snprintf(g_mp.check_room, sizeof(g_mp.check_room), "%s", room);
    send_str("mpCheckRoom", room);
}

static int room_from_input(const char *in, char *out, const size_t n) { // the invite parser (iïìiïiì)
    out[0] = 0;
    if (!in) return 0;
    while (*in == ' ' || *in == '\t' || *in == '\n') in++;
    char t[512];
    snprintf(t, sizeof(t), "%s", in);
    for (size_t l = strlen(t); l && (t[l - 1] == ' ' || t[l - 1] == '\t' || t[l - 1] == '\n'); l--) t[l - 1] = 0;
    // new URL(s).searchParams.get("room"): only an absolute URL parses
    const char *scheme = strstr(t, "://");
    const char *q = scheme ? strchr(scheme, '?') : NULL;
    while (q) {
        q++;
        if (!strncmp(q, "room=", 5)) {
            size_t i = 0;
            for (q += 5; *q && *q != '&' && *q != '#' && i + 1 < n; q++) out[i++] = *q;
            out[i] = 0;
            if (out[0]) return 1;
            break;
        }
        q = strchr(q, '&');
    }
    if ((!strncmp(t, "room_", 5) || !strncmp(t, "room:", 5)) && strlen(t) > 5) {
        snprintf(out, n, "%s", t);
        return 1;
    }
    return 0;
}

static void join_prompt_cb(void *user, const char *value) {
    (void) user;
    if (!value || !*value) return;
    char room[96];
    if (!room_from_input(value, room, sizeof(room))) {
        editor_message("Invalid room ID or URL");
        return;
    }
    join_with_password_check(room);
}

// ---------------------------------------------------------------------------
// the socket handlers (getSocketHandlers)

static double num_at(const cJSON *f, const int i) { return cJSON_GetNumberValue(cJSON_GetArrayItem(f, i)); }

static const char *str_at(const cJSON *f, const int i) {
    const cJSON *v = cJSON_GetArrayItem(f, i);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void on_room_joined(const cJSON *f) {
    cJSON *players = parse_arg(cJSON_GetArrayItem(f, 2));
    cJSON *settings = parse_arg(cJSON_GetArrayItem(f, 3));
    const int me = (int) num_at(f, 4);
    if (g_mp.in_room && g_mp.proj && g_mp.proj != g_ed.proj) g_mp.nplayers = 0;
    g_mp.in_room = 1;
    g_mp.proj = g_ed.proj;
    snprintf(g_mp.room_id, sizeof(g_mp.room_id), "%s", str_at(f, 1) ? str_at(f, 1) : "");
    g_mp.max_players = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(settings, "maxPlayers"));
    const cJSON *p;
    cJSON_ArrayForEach(p, players) {
        const int id = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(p, "id"));
        add_player(p, id == me);
        if (id == me) g_mp.me = id;
    }
    chat_show(1);
    system_message("Connected to room");
    g_mp.sel_sent = -2;
    g_mp.move_next = g_ed.now; // _startMoveInterval
    g_mp.have_last = 0;
    url_update(g_mp.room_id);
    toolbar_state();
    if (me_is_host() && g_ed.proj && g_ed.proj->count) { // _registerExistingObjects
        char *t = map_export_with_sids();
        send_str("mpSyncMap", t);
        free(t);
    }
    editor_show_window(16);
    cJSON_Delete(players);
    cJSON_Delete(settings);
}

static void on_room_info(const cJSON *f) {
    const char *id = str_at(f, 1);
    if (!id || !g_mp.check_room[0] || strcmp(id, g_mp.check_room)) return;
    cJSON *info = parse_arg(cJSON_GetArrayItem(f, 2));
    if (cJSON_GetObjectItemCaseSensitive(info, "error")) {
        editor_message("Room not found");
    } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(info, "hasPassword"))) {
        if (g_mp.has_script_pw) password_cb(NULL, g_mp.script_pw); // mp:join:<room>:<password>
        else ed_prompt("Enter room password:", "", password_cb, NULL);
    } else {
        join(g_mp.check_room, "");
    }
    cJSON_Delete(info);
}

static void on_frame(void *user, const cJSON *f) {
    (void) user;
    const char *l = str_at(f, 0);
    if (!g_ed.active || !l || l[0] != 'm' || l[1] != 'p') return;
    if (!strcmp(l, "mpRoomJoined")) return on_room_joined(f);
    if (!strcmp(l, "mpRoomInfo")) return on_room_info(f);
    if (!strcmp(l, "mpError")) { // onError
        const char *e = str_at(f, 1);
        char t[300];
        snprintf(t, sizeof(t), g_mp.chat_visible ? "Error: %s" : "Multiplayer error: %s", e ? e : "undefined");
        if (g_mp.chat_visible) system_message(t);
        else editor_message(t);
        return;
    }
    if (!g_mp.in_room || !g_mp.proj) return;
    if (!proj_alive()) { // KN: the room's map was closed: out of the room
        send0("mpLeave");
        cleanup();
        return;
    }
    EditorProject *prev = NULL;
    ed_project_enter(g_mp.proj, &prev); // the room's map, shown or not
    if (!strcmp(l, "mpPlayerJoined")) {
        cJSON *p = parse_arg(cJSON_GetArrayItem(f, 1));
        add_player(p, 0);
        system_messagef("%s joined", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "name")));
        cJSON_Delete(p);
    } else if (!strcmp(l, "mpPlayerLeft")) {
        const MpPlayer *p = player((int) num_at(f, 1));
        if (p) system_messagef("%s left", p->name);
        remove_player((int) num_at(f, 1));
    } else if (!strcmp(l, "mpObjectAdded")) {
        apply_object_added((int) num_at(f, 1), cJSON_GetArrayItem(f, 2));
    } else if (!strcmp(l, "mpObjectConfirmed")) {
        if (str_at(f, 1)) confirm_sid(str_at(f, 1), (int) num_at(f, 2));
    } else if (!strcmp(l, "mpObjectRemoved") || !strcmp(l, "mpGroupRemoved")) {
        apply_object_removed(f);
    } else if (!strcmp(l, "mpObjectUpdated")) {
        apply_object_updated((int) num_at(f, 1), cJSON_GetArrayItem(f, 2));
    } else if (!strcmp(l, "mpObjectsMoved")) {
        apply_objects_moved(cJSON_GetArrayItem(f, 1));
    } else if (!strcmp(l, "mpClearMap")) {
        apply_clear_map();
        system_message("Map cleared by host");
    } else if (!strcmp(l, "mpSyncMap")) {
        apply_full_map_sync(cJSON_GetArrayItem(f, 1));
        system_message("Map synced");
    } else if (!strcmp(l, "mpSyncMapConfirmed")) {
        cJSON *sids = parse_arg(cJSON_GetArrayItem(f, 1));
        sync_clear(); // sidMap / objectToSid (temp ids are kept)
        for (int i = 0; i < cJSON_GetArraySize(sids) && (size_t) i < g_ed.proj->count; i++)
            sid_register((int) cJSON_GetNumberValue(cJSON_GetArrayItem(sids, i)), g_ed.proj->objects[i]);
        cJSON_Delete(sids);
    } else if (!strcmp(l, "mpSyncConfig")) {
        cJSON *cfg = parse_arg(cJSON_GetArrayItem(f, 1)), *srv = parse_arg(cJSON_GetArrayItem(f, 2));
        assign_into(g_ed.proj->map_config, cfg);
        assign_into(g_ed.proj->server_config, srv);
        cJSON_Delete(cfg);
        cJSON_Delete(srv);
    } else if (!strcmp(l, "mpPlayerMove")) {
        MpPlayer *p = player((int) num_at(f, 1));
        if (p && !p->local) {
            const cJSON *pos = cJSON_GetArrayItem(f, 2), *rot = cJSON_GetArrayItem(f, 3);
            for (int k = 0; k < 3; k++) {
                if (cJSON_IsArray(pos)) p->pos[k] = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(pos, k));
                if (cJSON_IsArray(rot)) p->rot[k] = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(rot, k));
            }
        }
    } else if (!strcmp(l, "mpPlayerSelect")) {
        MpPlayer *p = player((int) num_at(f, 1));
        if (setting_bool("renderOutlines", 1) && p && !p->local) { // onPlayerSelect -> selectObject
            const cJSON *sid = cJSON_GetArrayItem(f, 2);
            p->has_sel = sid && !cJSON_IsNull(sid);
            p->sel = p->has_sel ? (int) cJSON_GetNumberValue(sid) : -1;
        }
    } else if (!strcmp(l, "mpChat")) {
        chat_message(str_at(f, 3), str_at(f, 4), str_at(f, 2));
    } else if (!strcmp(l, "mpKicked") || !strcmp(l, "mpBanned")) {
        const char *why = str_at(f, 1);
        char t[300];
        snprintf(t, sizeof(t), "You have been %s: %s", l[2] == 'K' ? "kicked" : "banned", why && *why ? why : "No reason given");
        cleanup();
        editor_message(t);
    } else if (!strcmp(l, "mpRoomSettings")) {
        cJSON *s = parse_arg(cJSON_GetArrayItem(f, 1));
        const cJSON *mp = cJSON_GetObjectItemCaseSensitive(s, "maxPlayers");
        if (cJSON_IsNumber(mp)) g_mp.max_players = (int) mp->valuedouble;
        cJSON_Delete(s);
    } else if (!strcmp(l, "mpHostChanged")) {
        MpPlayer *p = player((int) num_at(f, 1));
        if (p) {
            p->role = 1;
            system_messagef("%s is now the host", p->name);
        }
    } else if (!strcmp(l, "mpRequestMap")) {
        if (me_is_host()) {
            char *t = map_export_with_sids();
            cJSON *out = frame("mpSyncMap");
            cJSON_AddItemToArray(out, cJSON_CreateString(t ? t : "{}"));
            cJSON_AddItemToArray(out, cJSON_Duplicate(cJSON_GetArrayItem(f, 1), 1));
            mp_send(out);
            free(t);
        }
    } else if (!strcmp(l, "mpScriptLockAcquired")) {
        g_mp.lock_pending = 0;
        const MpPlayer *cur = current();
        g_mp.lock_id = cur ? cur->id : 0;
        snprintf(g_mp.lock_name, sizeof(g_mp.lock_name), "%s", cur ? cur->name : "");
        g_mp.force_popup = 1; // scriptPopup(true)
        editor_script_popup();
        g_mp.force_popup = 0;
    } else if (!strcmp(l, "mpScriptLockDenied")) {
        g_mp.lock_pending = 0;
        system_messagef("%s is currently editing scripts", str_at(f, 1) ? str_at(f, 1) : "undefined");
    } else if (!strcmp(l, "mpScriptLocked")) {
        g_mp.lock_id = (int) num_at(f, 1);
        snprintf(g_mp.lock_name, sizeof(g_mp.lock_name), "%s", str_at(f, 2) ? str_at(f, 2) : "undefined");
        system_messagef("%s is editing scripts", g_mp.lock_name);
    } else if (!strcmp(l, "mpScriptUnlocked")) {
        g_mp.lock_id = 0;
        system_message("Scripts unlocked");
    } else if (!strcmp(l, "mpScriptUpdate")) {
        cJSON *s = parse_arg(cJSON_GetArrayItem(f, 1));
        scripts_assign(s); // the open script window reloads on the revision (ed_script_tick)
        cJSON_Delete(s);
    }
    ed_project_leave(prev);
}

// ---------------------------------------------------------------------------
// editor events (registerEventHandlers)

static int sending(void) { return g_mp.in_room && g_ed.proj && g_ed.proj == g_mp.proj; }

void ed_mp_mute(const int delta) { g_mp.mute += delta; }

void ed_mp_object_added(EditorObject *o) {
    if (g_mp.pending_sync || g_mp.importing || g_mp.mute || !sending() || ed_entity_stage_member(o)) return;
    cJSON *data = ed_obj_serialize(o);
    char *t = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    cJSON *f = frame("mpObjectAdded");
    cJSON_AddItemToArray(f, cJSON_CreateString(temp_id(o)));
    cJSON_AddItemToArray(f, cJSON_CreateString(t ? t : "{}"));
    free(t);
    mp_send(f);
}

void ed_mp_object_removed(EditorObject *o) {
    if (g_mp.pending_sync || g_mp.importing || g_mp.mute || !sending()) return;
    const int sid = sid_of(o);
    if (sid < 0) return;
    sid_drop(sid);
    send_num("mpObjectRemoved", sid);
}

void ed_mp_object_selected(EditorObject *o) { // KN: the same selection again sends nothing
    if (!sending()) return;
    const int sid = o ? sid_of(o) : -1;
    if (sid == g_mp.sel_sent) return;
    g_mp.sel_sent = sid;
    send_num("mpPlayerSelect", sid);
}

void ed_mp_group_removed(const EditorGroup *g) {
    if (g_mp.pending_sync || !sending() || !g) return;
    cJSON *f = frame("mpGroupRemoved");
    for (size_t i = 0; i < g_ed.proj->count; i++) { // objInsts filtered by the group's members (the placeholder is not one yet)
        const EditorObject *o = g_ed.proj->objects[i];
        for (size_t k = 0; k < g->count; k++)
            if (g->objects[k] == o->uuid) {
                const int sid = sid_of(o);
                if (sid >= 0) cJSON_AddItemToArray(f, cJSON_CreateNumber(sid));
            }
    }
    if (cJSON_GetArraySize(f) > 1) mp_send(f);
    else cJSON_Delete(f);
}

void ed_mp_transform_end(EditorObject *o) { // sendObjectMoved
    if (g_mp.pending_sync || !sending() || !o) return;
    const int sid = sid_of(o);
    if (sid < 0) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "sid", sid);
    static const char *const keys[3] = {"p", "r", "s"};
    for (int k = 0; k < 3; k++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o->data, keys[k]);
        if (v) cJSON_AddItemToObject(m, keys[k], cJSON_Duplicate(v, 1));
        else if (k == 1) {
            cJSON *z = cJSON_CreateArray();
            for (int i = 0; i < 3; i++) cJSON_AddItemToArray(z, cJSON_CreateNumber(0));
            cJSON_AddItemToObject(m, "r", z);
        }
    }
    cJSON *list = cJSON_CreateArray();
    cJSON_AddItemToArray(list, m);
    send_json("mpObjectsMoved", list);
}

void ed_mp_property_changed(EditorObject *o, const cJSON *before) {
    if (g_mp.pending_sync || !sending() || !o || !before) return;
    cJSON *changes = cJSON_CreateObject();
    const cJSON *it;
    // the keys of either state but p / r / s whose JSON differs; a key the object lost is null
    for (int pass = 0; pass < 2; pass++) {
        cJSON_ArrayForEach(it, (pass ? o->data : before)) {
            const char *k = it->string;
            if (!strcmp(k, "p") || !strcmp(k, "r") || !strcmp(k, "s") || cJSON_GetObjectItemCaseSensitive(changes, k)) continue;
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(before, k), *b = cJSON_GetObjectItemCaseSensitive(o->data, k);
            if (a && b && cJSON_Compare(a, b, 1)) continue;
            cJSON_AddItemToObject(changes, k, b ? cJSON_Duplicate(b, 1) : cJSON_CreateNull());
        }
    }
    const int sid = sid_of(o);
    if (cJSON_GetArraySize(changes) && sid >= 0) {
        char *t = cJSON_PrintUnformatted(changes);
        cJSON *f = frame("mpObjectUpdated");
        cJSON_AddItemToArray(f, cJSON_CreateNumber(sid));
        cJSON_AddItemToArray(f, cJSON_CreateString(t ? t : "{}"));
        free(t);
        mp_send(f);
    }
    cJSON_Delete(changes);
}

void ed_mp_map_cleared(void) {
    if (!g_mp.pending_sync && !g_mp.importing && sending() && me_is_host()) send0("mpClearMap");
}

void ed_mp_import(const int start) {
    if (start) {
        if (g_mp.in_room) g_mp.importing = 1; // mapImportStart
        return;
    }
    g_mp.importing = 0; // mapImportEnded
    if (g_mp.pending_sync || !sending()) return;
    sync_clear();
    char *t = map_export_with_sids();
    send_str("mpSyncMap", t);
    free(t);
}

// scriptPopup: in a room the window opens once the lock is ours; 1 = handled here
int ed_mp_script_gate(void) {
    if (g_mp.force_popup || !g_mp.in_room) return 0;
    // requestScriptLock
    const MpPlayer *cur = current();
    if (g_mp.lock_id) {
        if (cur && g_mp.lock_id == cur->id) return 0;
        system_messagef("%s is currently editing scripts", g_mp.lock_name);
        return 1;
    }
    g_mp.lock_pending = 1;
    send0("mpScriptLock");
    return 1;
}

void ed_mp_script_closed(void) { release_script_lock(); }

// ---------------------------------------------------------------------------
// drawing: the players (1 x 2 x 1 at 0.6, their colour), the outlines, the name labels

int ed_mp_active(void) { return g_mp.in_room && g_ed.proj == g_mp.proj; }

static void player_matrix(const MpPlayer *p, float m[16], float rm[9]) {
    ed_euler_matrix((vec3) {p->rot[0], p->rot[1], p->rot[2]}, rm);
    memset(m, 0, 16 * sizeof(float));
    for (int row = 0; row < 3; row++) {
        m[row * 4 + 0] = rm[row * 3 + 0] * 1;
        m[row * 4 + 1] = rm[row * 3 + 1] * 2;
        m[row * 4 + 2] = rm[row * 3 + 2] * 1;
    }
    m[3] = p->pos[0];
    m[7] = p->pos[1];
    m[11] = p->pos[2];
}

void ed_mp_draw(void) {
    if (!ed_mp_active()) return;
    const double t = html_timer_now_ms() * 0.003;
    for (int i = 0; i < g_mp.nplayers; i++) {
        const MpPlayer *p = &g_mp.players[i];
        if (p->local) continue;
        const int hex = mp_hex_color(p->color);
        float m[16], rm[9];
        player_matrix(p, m, rm);
        ed_render_tinted_box(m, hex, 0.6f);
        if (p->has_sel && p->sel != -1) { // the outline: the object's box * 1.05, 0.4 + sin(t) * 0.15
            const EditorObject *o = object_of(p->sel);
            if (o) {
                float b[16];
                ed_obj_world_box(o, b);
                for (int row = 0; row < 3; row++)
                    for (int col = 0; col < 3; col++) b[row * 4 + col] *= 1.05f;
                ed_render_tinted_box(b, hex, (float) (0.4 + sin(t) * 0.15));
            }
        }
    }
}

void ed_mp_draw_labels(Canvas2D *c, const int cw, const int ch) {
    if (!ed_mp_active() || !c) return;
    for (int i = 0; i < g_mp.nplayers; i++) {
        const MpPlayer *p = &g_mp.players[i];
        if (p->local) continue;
        float m[16], rm[9];
        player_matrix(p, m, rm);
        // the label sprite: (0, 2, 0) in the model, 3 x 0.75 units, never hidden by depth
        const vec3 at = {p->pos[0] + rm[1] * 2, p->pos[1] + rm[4] * 2, p->pos[2] + rm[7] * 2};
        float sx, sy, px_per_unit;
        if (!ed_render_project_point(at, cw, ch, &sx, &sy, &px_per_unit)) continue;
        const double h = 0.75 * px_per_unit, k = h / 64.0; // the 256 x 64 label canvas
        if (h < 2) continue;
        c2d_save(c);
        c2d_translate(c, sx - 128 * k, sy - 32 * k);
        c2d_scale(c, k, k);
        c2d_set_fill_color(c, 0, 0, 0, 0.5f);
        c2d_begin_path(c);
        c2d_move_to(c, 12, 4);
        c2d_arc_to(c, 252, 4, 252, 60, 8);
        c2d_arc_to(c, 252, 60, 4, 60, 8);
        c2d_arc_to(c, 4, 60, 4, 4, 8);
        c2d_arc_to(c, 4, 4, 252, 4, 8);
        c2d_close_path(c);
        c2d_fill(c);
        c2d_set_font_css(c, "bold 28px sans-serif");
        c2d_set_text_align(c, C2D_ALIGN_CENTER);
        c2d_set_text_baseline(c, C2D_BASELINE_MIDDLE);
        float rgba[4] = {1, 1, 1, 1};
        c2d_parse_color(p->color, rgba);
        c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
        char name[64];
        snprintf(name, sizeof(name), "%.16s", p->name);
        c2d_fill_text(c, name, 128, 32, 0, 0);
        c2d_restore(c);
    }
}

// ---------------------------------------------------------------------------
// page: tick (camera every 200 ms, the URL's room once the socket opens), keys

void ed_mp_tick(const double now) {
    const int open = backend_social_is_open();
    if (open && !g_mp.was_open) {
        // the socket's connect: getURLRoomId (the URL's room, else the streamer mode's last one)
        const char *last = setting_str("_lastRoomId");
        const char *room = g_mp.url_room[0] && strcmp(g_mp.url_room, "***") ? g_mp.url_room : last;
        if (room && *room) {
            char r[96];
            snprintf(r, sizeof(r), "%s", room);
            join_with_password_check(r);
        }
    }
    g_mp.was_open = open;
    if (!g_mp.in_room || !g_mp.proj || g_mp.move_next <= 0 || now < g_mp.move_next) return;
    if (!proj_alive()) {
        send0("mpLeave");
        cleanup();
        return;
    }
    g_mp.move_next = now + 0.2;
    const Camera *cam = &g_mp.proj->camera;
    const float pos[3] = {cam->position.x, cam->position.y, cam->position.z};
    const float rot[3] = {g_mp.proj->pitch, g_mp.proj->yaw, 0};
    if (g_mp.have_last && !memcmp(pos, g_mp.last_pos, sizeof(pos)) && !memcmp(rot, g_mp.last_rot, sizeof(rot))) return;
    memcpy(g_mp.last_pos, pos, sizeof(pos));
    memcpy(g_mp.last_rot, rot, sizeof(rot));
    g_mp.have_last = 1;
    cJSON *f = frame("mpPlayerMove");
    cJSON *a = cJSON_CreateArray(), *b = cJSON_CreateArray();
    for (int k = 0; k < 3; k++) {
        cJSON_AddItemToArray(a, cJSON_CreateNumber(pos[k]));
        cJSON_AddItemToArray(b, cJSON_CreateNumber(rot[k]));
    }
    cJSON_AddItemToArray(f, a);
    cJSON_AddItemToArray(f, b);
    mp_send(f);
}

// Toggle Player List / Focus Chat (mpPlayerListKey / mpChatKey, only in a room); 1 = handled
int ed_mp_key(const int list_key, const int chat_key) {
    if (!g_mp.in_room) return 0;
    if (list_key) editor_show_window(17);
    if (chat_key && g_mp.chat_visible && g_mp.chat_input) html_dom_focus(g_mp.chat_input);
    return list_key || chat_key;
}

void ed_mp_page_enter(const char *query) {
    memset(&g_mp, 0, sizeof(g_mp));
    g_mp.listening = backend_social_listen(on_frame, NULL);
    const char *r = query ? strstr(query, "room=") : NULL;
    if (r && (r == query || r[-1] == '&')) {
        size_t i = 0;
        for (r += 5; *r && *r != '&' && *r != '#' && i + 1 < sizeof(g_mp.url_room); r++) g_mp.url_room[i++] = *r;
        g_mp.url_room[i] = 0;
    }
    g_mp.was_open = 0; // a socket already open counts as the connect
    chat_build();
}

void ed_mp_page_leave(void) {
    if (g_mp.in_room) send0("mpLeave");
    if (g_mp.listening) backend_social_unlisten(on_frame, NULL);
    free(g_mp.sids);
    free(g_mp.tmps);
    memset(&g_mp, 0, sizeof(g_mp));
}

// ---------------------------------------------------------------------------
// handlers

static int h_host(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { // hostFromUI
    (void) u; (void) ev; (void) a; (void) n;
    char v[32] = "", pw[128] = "";
    html_dom_get_value(html_dom_by_id("mpMaxPlayers"), v, sizeof(v));
    html_dom_get_value(html_dom_by_id("mpPassword"), pw, sizeof(pw));
    const HtmlElem close = html_dom_by_id("mpCloseOnDisconnect");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "maxPlayers", atoi(v) ? atoi(v) : 5);
    cJSON_AddStringToObject(o, "password", pw);
    cJSON_AddBoolToObject(o, "closeOnHostDisconnect", !close || html_dom_get_checked(close));
    send_json("mpHost", o);
    return 1;
}

static int h_join_prompt(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    ed_prompt("Enter Room ID or Invite URL:", "", join_prompt_cb, NULL);
    return 1;
}

static int h_disconnect(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    if (g_mp.in_room) {
        send0("mpLeave");
        cleanup();
    }
    return 1;
}

static int h_copy(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { // copyRoomURL
    (void) u; (void) ev; (void) a; (void) n;
    if (!g_mp.in_room) return 1;
    char origin[128], url[384];
    html_page_origin(origin, sizeof(origin));
    snprintf(url, sizeof(url), "%s/editor.html?room=%s", origin, g_mp.room_id);
    html_clipboard_write(url);
    system_message("Invite link copied to clipboard");
    return 1;
}

static void ban_cb(void *user, const char *value) {
    if (value && !strcmp(value, "1")) send_num("mpBan", (double) (intptr_t) user);
}

static int arg_int(const HtmlArg *a, const int n) {
    if (n < 1) return 0;
    return a[0].kind == HTML_ARG_STR ? atoi(a[0].str) : (int) a[0].num;
}

static int h_kick(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    send_num("mpKick", arg_int(a, n));
    return 1;
}

static int h_ban(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    ed_confirm("Are you sure you want to ban this player?", ban_cb, (void *) (intptr_t) arg_int(a, n));
    return 1;
}

static int h_teleport(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { // teleportToPlayer
    (void) u; (void) ev;
    const MpPlayer *p = player(arg_int(a, n));
    if (!p || !g_ed.proj) return 1;
    g_ed.proj->camera.position = (vec3) {p->pos[0] + 5, p->pos[1] + 3, p->pos[2] + 5};
    return 1;
}

static int h_select_object(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { // selectPlayerObject
    (void) u; (void) ev;
    const MpPlayer *p = player(arg_int(a, n));
    if (!p || !p->has_sel || !g_ed.proj) return 1;
    EditorObject *o = object_of(p->sel);
    if (o) ed_obj_select(o);
    return 1;
}

// KN: the Max Players slider's inline statements (the number beside it; in a room, updateRoomSetting)
static int h_max_players(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u;
    const char *v = n >= 1 && a[0].kind == HTML_ARG_STR ? a[0].str : ev->value;
    html_dom_set_text(html_dom_by_id("mpMaxPlayersVal"), v ? v : "");
    if (n >= 2 && a[1].num) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "maxPlayers", v ? atoi(v) : 0); // parseInt: NaN never happens on a range
        send_json("mpRoomSettings", o);
    }
    return 1;
}

void ed_mp_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.multiplayer.hostFromUI", h_host},
        {"KE.multiplayer.joinPrompt", h_join_prompt},
        {"KE.multiplayer.disconnect", h_disconnect},
        {"KE.multiplayer.copyRoomURL", h_copy},
        {"KE.multiplayer.kick", h_kick},
        {"KE.multiplayer.ban", h_ban},
        {"KE.multiplayer.playerManager.teleportToPlayer", h_teleport},
        {"KE.multiplayer.playerManager.selectPlayerObject", h_select_object},
        {"KE.multiplayer.maxPlayersInput", h_max_players},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// ---------------------------------------------------------------------------
// KRUNKNATIVE_EDITOR_SCRIPT "mp:..." (tools/editor/mp_e2e.py):
//   mp:host[:max[:password]]  hostFromUI with these values    mp:join:<room or invite>[:password]
//   mp:chat:<text>            the chat input + Enter           mp:leave        Disconnect
//   mp:script                 scriptPopup (the lock)           mp:scriptclose  the script window closes
//   mp:dump:<file>            the room state as text      mp:setting:<key>:<JSON>  an mp_settings value

static void dump(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "ROOM %s %d\n", g_mp.in_room ? g_mp.room_id : "-", g_mp.max_players);
    for (int i = 0; i < g_mp.nplayers; i++) {
        const MpPlayer *p = &g_mp.players[i];
        fprintf(f, "PLAYER %d %s %s role=%d%s pos=%.2f,%.2f,%.2f sel=%d\n", p->id, p->name, p->color, p->role, p->local ? " local" : "",
                p->pos[0], p->pos[1], p->pos[2], p->has_sel ? p->sel : -1);
    }
    if (g_mp.proj && proj_alive()) {
        EditorProject *prev = NULL;
        ed_project_enter(g_mp.proj, &prev);
        for (size_t i = 0; i < g_ed.proj->count; i++) {
            const EditorObject *o = g_ed.proj->objects[i];
            char *t = cJSON_PrintUnformatted(o->data);
            fprintf(f, "OBJ sid=%d %s\n", sid_of(o), t ? t : "");
            free(t);
        }
        const cJSON *s;
        cJSON_ArrayForEach(s, g_ed.proj->scripts) fprintf(f, "SCRIPT %s %s\n", s->string, cJSON_IsString(s) ? s->valuestring : "");
        ed_project_leave(prev);
    }
    fprintf(f, "LOCK %d %s\n", g_mp.lock_id, g_mp.lock_name);
    fprintf(f, "CHATVISIBLE %d\n", g_mp.chat_visible);
    char *chat = malloc(65536);
    if (chat && g_mp.chat_list) {
        for (HtmlElem line = html_dom_first_child(g_mp.chat_list); line; line = html_dom_next(line)) {
            html_dom_get_text(line, chat, 65536);
            fprintf(f, "CHAT %s\n", chat);
        }
    }
    free(chat);
    char icon[32] = "";
    html_dom_get_style(html_dom_by_id("multiplayerIcon"), "color", icon, sizeof(icon));
    fprintf(f, "ICON %s\n", icon);
    fclose(f);
}

void ed_mp_script(const char *arg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", arg);
    char *rest = strchr(buf, ':');
    if (rest) *rest++ = 0;
    HtmlEvent ev = {.type = "click"};
    if (!strcmp(buf, "host")) {
        char *pw = rest ? strchr(rest, ':') : NULL;
        if (pw) *pw++ = 0;
        editor_show_window(16);
        if (rest && *rest) html_dom_set_value(html_dom_by_id("mpMaxPlayers"), rest);
        if (pw) html_dom_set_value(html_dom_by_id("mpPassword"), pw);
        h_host(NULL, &ev, NULL, 0);
    } else if (!strcmp(buf, "join") && rest) {
        // the room (or invite URL: "https://..." holds ':' too); a password after the last ':' of a room id
        char *pw = NULL;
        if (!strstr(rest, "://")) {
            char *c = strlen(rest) > 5 ? strchr(rest + 5, ':') : NULL;
            if (c) {
                *c = 0;
                pw = c + 1;
            }
        }
        snprintf(g_mp.script_pw, sizeof(g_mp.script_pw), "%s", pw ? pw : "");
        g_mp.has_script_pw = pw != NULL; // answers the password prompt
        join_prompt_cb(NULL, rest);
    } else if (!strcmp(buf, "setting") && rest) { // setting:<key>:<JSON value> (the Settings window's Multiplayer tab)
        char *v = strchr(rest, ':');
        if (v) {
            *v++ = 0;
            cJSON *val = cJSON_Parse(v);
            cJSON *m = ed_mp_settings();
            cJSON_DeleteItemFromObjectCaseSensitive(m, rest);
            cJSON_AddItemToObject(m, rest, val ? val : cJSON_CreateString(v));
            ed_mp_settings_save();
        }
    } else if (!strcmp(buf, "password") && rest) {
        password_cb(NULL, rest);
    } else if (!strcmp(buf, "chat") && rest) {
        chat_build();
        html_dom_set_value(g_mp.chat_input, rest);
        chat_send();
    } else if (!strcmp(buf, "leave")) {
        h_disconnect(NULL, &ev, NULL, 0);
    } else if (!strcmp(buf, "script")) {
        editor_script_popup();
    } else if (!strcmp(buf, "scriptclose")) {
        ed_script_close();
    } else if (!strcmp(buf, "dump") && rest) {
        dump(rest);
    }
}
