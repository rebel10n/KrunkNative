// Official Krunker session: the S2C packets of v10.0.3 that doc/v10/s2c-coverage.md classifies and no other
// krunker_link*.c file translates (client/src/net/krunker_link_internal.h). Pure translation, like krunker_link.c:
// each packet becomes the NetPacket / EXT our own server sends for the same thing, or is ignored on purpose (the
// coverage doc gives each one's reason). Line numbers: deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js); the
// handler table is the iíÍíiïï object at :261708.
//   c      PLAYER_ANIM          [sid, clipIndex, reps]   NET_EXT_PLAYER_ANIM (iíîiiìï :257769 -> players.playClip :193806)
//   uai    UPDATE_ASSET_ID      [sid, assetTxt]          NET_EXT_PLAYER_ASSET_ID (iíiïiiï :257784 -> players.updateAssetID :197964)
//   ready  READY                [ready]                  "sb" ["welc", username] back (iiîîíìí :261397, the send :261464);
//                                                        ready falsy: the live client closes the socket (KRUNKER_LINK_EV_CLOSE)
//   sb     SHOW_SPEECH_BUBBLE   [html, ms, sigh]         NET_EXT_SPEECH_BUBBLE (iíììiîì :261181)
//   cd     SYNC_P_CON_DATA      [lobbyPlayers]           NET_PACKET_COMP_LOBBY CLEAR + one SET per entry (iìîïïïî :256533)
//   cda    SYNC_P_CON_OBJ       [key, entry | null]      NET_PACKET_COMP_LOBBY SET / REMOVE (iííiíiî :256526)
//   uid    CON_UID              [key]                    our lobbyPlayers key (iììîiíí :261633; the renderer's own entry :256554)
//   sdc    SKIN_DEFINITION_CHANGED [{skinId, skin}]      NET_EXT_SKIN_DEFINITION_CHANGED, JSON [skinId, skin] (:261708-261714)
//   lrmr   LATEST_RANKED_MMR_CHANGE [mmrChange, rank]    NET_EXT_LATEST_RANKED_MMR_CHANGE (iìíìïiì :255609)
//   gameAnnouncement [{message, actions, ...}]           NET_EXT_GAME_ANNOUNCEMENT (iîííiîí :244564)
//   jnk    FOUND_JUNK           [balance, amount]        NET_PACKET_CHAT NET_CHAT_FOUND_JUNK "You found [x.xx] Junk" (iïíííîì :242285)
//   ksL    SCRIPT_LOG           [isError, args]          NET_PACKET_SCRIPT_LOG (iíiïïiï :261492): the map script's console line
//   sd     CUSTOM_NETWORK_MESSAGE [id, data]             NET_PACKET_SCRIPT_MSG (iïíiiií :241493 -> mainScript.onNetworkMessage):
//                                                        the server script's GAME.NETWORK.send / broadcast, data's "V_" keys
//                                                        stripped (ks_net_body_from_official, shared/src/ks/ks_net_official.c)
//   p / mp INTER_PROG / INTER_M_PROG [prog]              NET_EXT_PLAYER_INTER_PROG: the spectated player's bars (:259394 / :259398)
//   unb    UNBOX_MESSAGE        [name, itemIndex, kr]    NET_EXT_UNBOX_MESSAGE NAMED (iíïîiìí :254321)
//   unbS   UNBOX_MESSSAGE_S     [playerId, skinIndex]    NET_EXT_UNBOX_MESSAGE PLAYER (iíiíïíi :254331)
//   lotWin LOTTERY_PRIZE_WON    [lotId, entryId, prize, playerId] NET_EXT_LOTTERY_PRIZE_WON (iíiìïïi :255154)
//   spinW  SPIN_WHEEL_RESPONSE  [socketId, uid, err, randNum, skin, funds] NET_EXT_SPIN_WHEEL_RESPONSE (iiîïiiì :254991)
//   chreq  CHARGE_CONSENT_REQUEST [id, amount, modal]    NET_EXT_CHARGE_CONSENT_REQUEST (iîiîiïi :257673); its answer
//                                                        NET_EXT_CHARGE_CONSENT_RESPONSE goes out as "chrsp" [id(, 1)] (:257700)
//   warsClan WARS_CLAN          [id, name, logo, color]  NET_EXT_WARS_CLAN (iíìïïíì :255551); init's syncData.warsClan too (:256999)
//   edMp   EDIT_MAP             [mapJson]                NET_EXT_KN_OFFICIAL_EDIT_MAP slices: the editor opens it (iiíïìiì :259803)
#include "krunker_link_internal.h"
#include <ks_net.h>
#include <net_ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const KrunkerValue *arg(const KrunkerFrame *f, const uint32_t i) { return krunker_frame_arg(f, i); }

static int nil(const KrunkerValue *v) { return !v || v->type == KRUNKER_NIL; }

// JS truthiness of a value
static int truthy(const KrunkerValue *v) {
    if (nil(v)) return 0;
    if (v->type == KRUNKER_BOOL) return v->u.boolean;
    if (v->type == KRUNKER_NUM) return v->u.num != 0 && !isnan(v->u.num);
    if (v->type == KRUNKER_STR) return v->len > 0;
    return 1;
}

// JS String(v) of a string / number key ("abc", 12 -> "12"); "" otherwise
static void key_string(const KrunkerValue *v, char *out, const size_t n) {
    out[0] = 0;
    if (krunker_is(v, KRUNKER_STR)) snprintf(out, n, "%s", krunker_str(v));
    else if (krunker_is(v, KRUNKER_NUM)) snprintf(out, n, "%.17g", v->u.num);
}

static void emit_ext(KrunkerLink *l, const uint16_t subtype, const void *body, const uint16_t length) {
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n) krunker_link_emit(l, NET_PACKET_EXT, buf, n);
}

// One JSON record (`v` printed as JSON) as NetExtJsonChunk slices of `subtype`, in order (net_ext.h; the entity
// records of krunker_link_script.c travel the same way)
static void emit_json(KrunkerLink *l, const uint16_t subtype, const KrunkerValue *v) {
    KrunkerBuf buf = {0};
    if (!v || krunker_value_to_json(v, &buf) != KRUNKER_PROTO_OK || !buf.data || !buf.len) {
        krunker_buf_free(&buf);
        return;
    }
    const size_t parts = (buf.len + NET_EXT_JSON_CHUNK_DATA - 1) / NET_EXT_JSON_CHUNK_DATA;
    uint8_t body[NET_EXT_BODY_MAX];
    for (size_t p = 0; parts <= 255 && p < parts; p++) { // 255 slices: ~130 KB, never a store entry
        const size_t off = p * NET_EXT_JSON_CHUNK_DATA;
        const size_t n = buf.len - off < (size_t) NET_EXT_JSON_CHUNK_DATA ? buf.len - off : (size_t) NET_EXT_JSON_CHUNK_DATA;
        const NetExtJsonChunk head = {(uint8_t) p, (uint8_t) parts};
        memcpy(body, &head, sizeof(head));
        memcpy(body + sizeof(head), buf.data + off, n);
        emit_ext(l, subtype, body, (uint16_t) (sizeof(head) + n));
    }
    krunker_buf_free(&buf);
}

// ---------------------------------------------------------------- player clips

// "c" [sid, clipIndex, reps]: playClip(findBySid(sid), animList[clipIndex], reps || 1). playClip itself does nothing
// for a clip index past the list (animList[i] undefined) or a player without an animated class model (the client's
// consumer checks that: client/src/net/s2c_events.c).
static void on_player_anim(KrunkerLink *l, const KrunkerFrame *f) {
    const double clip = krunker_num(arg(f, 1), -1);
    if (!(clip >= 0 && clip < NET_PLAYER_CLIP_COUNT) || clip != floor(clip)) return;
    const double reps = krunker_num(arg(f, 2), 0);
    const NetPlayerAnimPacket p = {.sid = (int32_t) krunker_num(arg(f, 0), -1), .clip = (uint8_t) clip,
                                   .reps = (float) (reps != 0 && !isnan(reps) ? reps : 1.0)}; // `|| 1`
    emit_ext(l, NET_EXT_PLAYER_ANIM, &p, sizeof(p));
}

// "uai" [sid, assetTxt]: updateAssetID(findBySid(sid), assetTxt) does `assetTxt += ""` and needs config.mdl and a
// non-empty text (the client's consumer checks the class, client/src/net/s2c_events.c)
static void on_update_asset_id(KrunkerLink *l, const KrunkerFrame *f) {
    NetPlayerAssetIdPacket p = {.sid = (int32_t) krunker_num(arg(f, 0), -1)};
    key_string(arg(f, 1), p.asset, sizeof(p.asset)); // a string or a number as JS String() spells it
    if (!p.asset[0]) return;
    emit_ext(l, NET_EXT_PLAYER_ASSET_ID, &p, sizeof(p));
}

// ---------------------------------------------------------------- the menu's speech bubble

// "ready" [ready] (iiîîíìí :261397): ready -> the live client fills the news with featured maps, maybe the register
// gift popup, then (a setTimeout 0) io.send("sb", "welc", getSavedVal("krunker_username")) (:261464), which the
// server answers with an "sb". Our saved username is the account the socket signed in ("" none: JS null).
// Not ready: iîiìiîi.socket.close() (:261468).
static void on_ready(KrunkerLink *l, const KrunkerFrame *f) {
    if (!truthy(arg(f, 0))) {
        if (l->io.event) l->io.event(l->io.user, KRUNKER_LINK_EV_CLOSE, "ready 0: the server is not ready for us");
        return;
    }
    const KrunkerValue args[] = {KRUNKER_V_STR("welc"), l->account_name[0] ? KRUNKER_V_STR(l->account_name) : KRUNKER_V_NIL};
    krunker_link_send(l, krunker_c2s_wire(KRUNKER_C2S_SPEECH_BUBBLE), args, 2);
}

// "sb" [html, ms, sigh]: `if (html)`, then the bubble for ms || 2500 (a sigh first when sigh)
static void on_speech_bubble(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *text = arg(f, 0);
    if (!truthy(text)) return;
    NetSpeechBubblePacket p;
    memset(&p, 0, sizeof(p));
    if (krunker_is(text, KRUNKER_STR)) snprintf(p.text, sizeof(p.text), "%s", krunker_str(text));
    else key_string(text, p.text, sizeof(p.text)); // innerHTML = a number: its String()
    const double ms = krunker_num(arg(f, 1), 0);
    p.ms = ms > 0 && ms < 600000 ? (uint32_t) ms : 0; // `|| 2500` (the consumer's default)
    p.sigh = (uint8_t) truthy(arg(f, 2));
    emit_ext(l, NET_EXT_SPEECH_BUBBLE, &p, sizeof(p));
}

// ---------------------------------------------------------------- the comp lobby table

#define LOBBY_UID_BASE 100000 // a key no player of ours matches: past every official sid

// The uid a lobbyPlayers key stands for in our comp menu: ours (CON_UID) is the INIT's client_id 0 (comp_ui.c's own
// entry); another is the sid of the player with the entry's account id, else LOBBY_UID_BASE + its slot.
static int32_t lobby_uid(const KrunkerLink *l, const char *key, const int slot, const int32_t account_id) {
    if (l->con_uid[0] && !strcmp(key, l->con_uid)) return 0;
    for (int i = 0; account_id > 0 && i < l->player_count; i++) {
        if (l->players[i].info.account_id == account_id && l->players[i].sid > 0) return l->players[i].sid;
    }
    return LOBBY_UID_BASE + slot;
}

// The slot of a key (added the first time it shows, its uid fixed then), -1 when the table is full
static int lobby_slot(KrunkerLink *l, const char *key, const int32_t account_id) {
    for (int i = 0; i < l->lobby_count; i++) {
        if (!strcmp(l->lobby_key[i], key)) return i;
    }
    if (l->lobby_count >= KRUNKER_LINK_MAX_PLAYERS) return -1;
    const int i = l->lobby_count++;
    snprintf(l->lobby_key[i], sizeof(l->lobby_key[i]), "%s", key);
    memset(&l->lobby_entry[i], 0, sizeof(l->lobby_entry[i]));
    l->lobby_entry[i].uid = lobby_uid(l, key, i, account_id);
    l->lobby_live[i] = 0;
    return i;
}

static void emit_lobby(KrunkerLink *l, const uint8_t op, const NetCompLobbyEntry *entry) {
    NetCompLobbyPacket p;
    memset(&p, 0, sizeof(p));
    p.op = op;
    if (entry) p.entry = *entry;
    krunker_link_emit(l, NET_PACKET_COMP_LOBBY, &p, sizeof(p));
}

// One lobbyPlayers entry (syncLobbyP :215966-216018: a, rej, t, c, i, n, v, cl, l, f; n / cl are 0 when unset, t / c
// deleted when unset) into the key's slot
static void lobby_entry(KrunkerLink *l, const int slot, const KrunkerValue *e) {
    NetCompLobbyEntry *out = &l->lobby_entry[slot];
    const int32_t uid = out->uid;
    memset(out, 0, sizeof(*out));
    out->uid = uid;
    out->active = (uint8_t) truthy(krunker_get(e, "a"));
    out->rejected = (uint8_t) truthy(krunker_get(e, "rej"));
    out->team = (int8_t) krunker_num(krunker_get(e, "t"), 0);
    const KrunkerValue *c = krunker_get(e, "c");
    out->class_index = (int8_t) (krunker_is(c, KRUNKER_NUM) ? c->u.num : -1); // `c != null` (:256608)
    out->account_id = (int32_t) krunker_num(krunker_get(e, "i"), 0);
    out->level = (int32_t) krunker_num(krunker_get(e, "l"), 0);
    out->verified = (uint8_t) truthy(krunker_get(e, "v"));
    const KrunkerValue *flag = krunker_get(e, "f");
    out->flag = (int16_t) (krunker_is(flag, KRUNKER_NUM) ? flag->u.num : -1);
    const char *name = krunker_str(krunker_get(e, "n")), *clan = krunker_str(krunker_get(e, "cl"));
    if (name) snprintf(out->name, sizeof(out->name), "%s", name);
    if (clan) snprintf(out->clan, sizeof(out->clan), "%s", clan);
    l->lobby_live[slot] = 1;
}

// "cd" [lobbyPlayers] (iìîïïïî: iiiïìïï = data, re-render): the whole table, every entry (active or not) kept
static void on_lobby_data(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *table = arg(f, 0);
    emit_lobby(l, NET_COMP_LOBBY_CLEAR, NULL);
    for (int i = 0; i < l->lobby_count; i++) l->lobby_live[i] = 0;
    if (!krunker_is(table, KRUNKER_MAP)) return;
    for (uint32_t i = 0; i < table->len; i++) {
        const KrunkerValue *e = &table->u.items[2 * i + 1];
        if (!krunker_is(e, KRUNKER_MAP)) continue;
        char key[32];
        key_string(&table->u.items[2 * i], key, sizeof(key));
        const int slot = lobby_slot(l, key, (int32_t) krunker_num(krunker_get(e, "i"), 0));
        if (slot < 0) continue;
        lobby_entry(l, slot, e);
        emit_lobby(l, NET_COMP_LOBBY_SET, &l->lobby_entry[slot]);
    }
}

// "cda" [key, entry | null] (iííiíiî: iiiïìïï[key] = entry, deleted when falsy)
static void on_lobby_entry(KrunkerLink *l, const KrunkerFrame *f) {
    char key[32];
    key_string(arg(f, 0), key, sizeof(key));
    const KrunkerValue *e = arg(f, 1);
    if (!key[0]) return;
    if (!truthy(e)) {
        for (int i = 0; i < l->lobby_count; i++) {
            if (strcmp(l->lobby_key[i], key) || !l->lobby_live[i]) continue;
            l->lobby_live[i] = 0;
            emit_lobby(l, NET_COMP_LOBBY_REMOVE, &l->lobby_entry[i]);
        }
        return;
    }
    if (!krunker_is(e, KRUNKER_MAP)) return;
    const int slot = lobby_slot(l, key, (int32_t) krunker_num(krunker_get(e, "i"), 0));
    if (slot < 0) return;
    lobby_entry(l, slot, e);
    emit_lobby(l, NET_COMP_LOBBY_SET, &l->lobby_entry[slot]);
}

// "uid" [key] (iììîiíí: iîìïiiï = key, saveVal("conUID_", key) for a reconnect's clientUID): our entry is the one
// under this key. Keys seen before it came took other uids: the table goes out again with ours under uid 0.
static void on_con_uid(KrunkerLink *l, const KrunkerFrame *f) {
    char key[32];
    key_string(arg(f, 0), key, sizeof(key));
    if (!strcmp(key, l->con_uid)) return;
    snprintf(l->con_uid, sizeof(l->con_uid), "%s", key);
    int changed = 0;
    for (int i = 0; i < l->lobby_count; i++) {
        const int32_t uid = lobby_uid(l, l->lobby_key[i], i, l->lobby_entry[i].account_id);
        if ((uid == 0) == (l->lobby_entry[i].uid == 0)) continue; // ours stays ours, the others keep theirs
        l->lobby_entry[i].uid = uid;
        changed = 1;
    }
    if (!changed) return;
    emit_lobby(l, NET_COMP_LOBBY_CLEAR, NULL);
    for (int i = 0; i < l->lobby_count; i++) {
        if (l->lobby_live[i]) emit_lobby(l, NET_COMP_LOBBY_SET, &l->lobby_entry[i]);
    }
}

// ---------------------------------------------------------------- store / live service

// "sks" SKIN_STAT [weapon, stat, amount] (deob-10.0.4.js:184311): the server counted a server-only stat (candy) on your
// counter skin; the client's countSkinStat(you, weapon, stat, amount) (iiîíiíi, :221919-221923), as NET_EXT_KN_SKIN_STAT.
static void on_skin_stat(KrunkerLink *l, const KrunkerFrame *f) {
    char stat[24];
    key_string(arg(f, 1), stat, sizeof(stat));
    const int index = skin_stat_index(stat);
    const double weapon = krunker_num(arg(f, 0), -1), amount = krunker_num(arg(f, 2), 1);
    if (index < 0 || weapon < 0 || weapon > 1e6) return;
    const NetSkinStatPacket body = {.weapon = (int32_t) weapon, .amount = amount >= 1 && amount <= 1e6 ? (int32_t) amount : 1, .stat = (uint8_t) index};
    emit_ext(l, NET_EXT_KN_SKIN_STAT, &body, sizeof(body));
}

// "sdc" [{skinId, skin}] (:261708-261714): `if (skin && typeof skinId == "number")` store.skins[skinId] = skin, then
// the store's own setup of an entry (iîíììïî :116530). The record goes to the client whole, as JSON [skinId, skin].
static void on_skin_definition(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *d = arg(f, 0);
    const KrunkerValue *id = krunker_get(d, "skinId"), *skin = krunker_get(d, "skin");
    if (!krunker_is(id, KRUNKER_NUM) || !truthy(skin)) return;
    const KrunkerValue record[] = {*id, *skin};
    const KrunkerValue all = KRUNKER_V_ARRAY(record, 2);
    emit_json(l, NET_EXT_SKIN_DEFINITION_CHANGED, &all);
}

// "lrmr" [mmrChange, latestRank] (iìíìïiì: `if (mmrChange)` $latestMMRChange / $latestRank, the end screen's rank
// card). rp_change is KrunkNative's own (our server's; the official balance comes as "ub", krunker_link.c on_balances).
static void on_ranked_mmr(KrunkerLink *l, const KrunkerFrame *f) {
    if (!truthy(arg(f, 0))) return;
    const NetLatestRankedMmrPacket p = {.mmr_change = (int32_t) krunker_num(arg(f, 0), 0),
                                        .latest_rank = (int32_t) krunker_num(arg(f, 1), 0), .rp_change = 0};
    emit_ext(l, NET_EXT_LATEST_RANKED_MMR_CHANGE, &p, sizeof(p));
}

// "gameAnnouncement" [{message, target, trackId, bundleId, displayIds, actions}] (iîííiîí :244564): the banner's
// message and, of the actions, v10's "ranked-queue" with its label (the card's buttons :133490; the action map
// :133340). Not carried (doc/v10/s2c-coverage.md): target, trackId, bundleId, displayIds, the other actions.
static void on_game_announcement(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *a = arg(f, 0);
    NetGameAnnouncementPacket p;
    memset(&p, 0, sizeof(p));
    const char *message = krunker_str(krunker_get(a, "message"));
    if (message) snprintf(p.message, sizeof(p.message), "%s", message);
    const KrunkerValue *actions = krunker_get(a, "actions");
    for (uint32_t i = 0; krunker_is(actions, KRUNKER_ARRAY) && i < actions->len; i++) {
        const KrunkerValue *act = krunker_at(actions, i);
        const char *name = krunker_str(krunker_get(act, "action"));
        if (!name || strcmp(name, "ranked-queue")) continue;
        const char *label = krunker_str(krunker_get(act, "label"));
        p.ranked_queue = 1;
        if (label) snprintf(p.ranked_label, sizeof(p.ranked_label), "%s", label);
        break;
    }
    emit_ext(l, NET_EXT_GAME_ANNOUNCEMENT, &p, sizeof(p));
}

// "jnk" [balance, amount] (iïíííîì, sent :221143): addChat(-1, null, "You found [amount.toFixed(2)] Junk", 1), as our
// server's game_found_junk (shared/src/triggers.c). The balance (account.junk) and the end screen's junk tally are not
// kept: this client's official account shows its wallet from the account frames.
static void on_found_junk(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *v = arg(f, 1);
    double amount = krunker_num(v, NAN);
    if (krunker_is(v, KRUNKER_STR)) amount = atof(krunker_str(v)); // parseFloat
    if (isnan(amount)) return;
    NetChatPacket chat;
    memset(&chat, 0, sizeof(chat));
    chat.uid = -1;
    chat.kind = NET_CHAT_FOUND_JUNK;
    snprintf(chat.text, sizeof(chat.text), "You found [%.2f] Junk", amount);
    krunker_link_emit(l, NET_PACKET_CHAT, &chat, sizeof(chat));
}

// "ksL" SCRIPT_LOG [isError, args] (iíiïïiï :261492): console.log("[KRUNKSCRIPT] " + args.join(" ")), red on an error:
// the server script's GAME.log as our server sends it (NET_PACKET_SCRIPT_LOG: uint8 is_error + the text, at most
// KS_NET_LOG_TEXT_MAX (480) bytes, a longer line cut to end with "...", shared/include/ks_net.h; the client prints it
// with the prefix, client/src/ks/ks_client_host.c).
static void js_join_item(const KrunkerValue *v, char *out, const size_t n) {
    out[0] = 0;
    if (nil(v)) return; // Array.join: null / undefined are ""
    if (v->type == KRUNKER_STR) snprintf(out, n, "%s", krunker_str(v));
    else if (v->type == KRUNKER_BOOL) snprintf(out, n, "%s", v->u.boolean ? "true" : "false");
    else if (v->type == KRUNKER_NUM) {
        if (v->u.num == floor(v->u.num) && fabs(v->u.num) < 1e15) snprintf(out, n, "%.0f", v->u.num);
        else snprintf(out, n, "%.17g", v->u.num);
    } else if (v->type == KRUNKER_ARRAY) { // String(array) = its items joined by ","
        size_t at = 0;
        for (uint32_t i = 0; i < v->len && at + 1 < n; i++) {
            char item[64];
            js_join_item(&v->u.items[i], item, sizeof(item));
            at += (size_t) snprintf(out + at, n - at, "%s%s", i ? "," : "", item);
        }
    } else snprintf(out, n, "[object Object]");
}

#define SCRIPT_LOG_TEXT_MAX 480 // KS_NET_LOG_TEXT_MAX (shared/include/ks_net.h)
static void on_script_log(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *args = arg(f, 1);
    char text[SCRIPT_LOG_TEXT_MAX * 2];
    size_t at = 0;
    text[0] = 0;
    for (uint32_t i = 0; krunker_is(args, KRUNKER_ARRAY) && i < args->len && at + 1 < sizeof(text); i++) {
        char item[SCRIPT_LOG_TEXT_MAX];
        js_join_item(&args->u.items[i], item, sizeof(item));
        at += (size_t) snprintf(text + at, sizeof(text) - at, "%s%s", i ? " " : "", item);
    }
    size_t len = strlen(text);
    if (len > SCRIPT_LOG_TEXT_MAX) {
        memcpy(text + SCRIPT_LOG_TEXT_MAX - 3, "...", 3);
        len = SCRIPT_LOG_TEXT_MAX;
    }
    uint8_t payload[1 + SCRIPT_LOG_TEXT_MAX];
    payload[0] = (uint8_t) truthy(arg(f, 0));
    memcpy(payload + 1, text, len);
    krunker_link_emit(l, NET_PACKET_SCRIPT_LOG, payload, (uint16_t) (1 + len));
}

// ["sd", id, data] -> onNetworkMessage(id, data): one SCRIPT_MSG (KsNetChunkHeader slices) like our server's send_body
static void on_network_message(KrunkerLink *l, const KrunkerFrame *f) {
    static uint8_t body[KS_NET_MAX_BODY];
    static uint16_t seq; // one official session at a time: the link's message counter
    const int len = ks_net_body_from_official(arg(f, 0), arg(f, 1), body, sizeof(body));
    if (len < 0) return;
    const size_t slice = (size_t) KS_NET_CHUNK_DATA, count = ((size_t) len + slice - 1) / slice; // ks_net_make_chunk's slices
    if (count > 255) return;
    const uint16_t msg = seq++;
    for (size_t i = 0; i < count; i++) {
        uint8_t out[NET_MAX_PAYLOAD];
        const size_t at = i * slice, n = (size_t) len - at < slice ? (size_t) len - at : slice;
        const KsNetChunkHeader h = {.msg_seq = msg, .chunk = (uint8_t) i, .chunk_count = (uint8_t) count};
        memcpy(out, &h, sizeof(h));
        memcpy(out + sizeof(h), body + at, n);
        krunker_link_emit(l, NET_PACKET_SCRIPT_MSG, out, (uint16_t) (sizeof(h) + n));
    }
}

// ---------------------------------------------------------------- the spectated player's bars

// "p" [prog] -> iìììîíí (:259394): interactProg.display = prog && prog > 0, .prog = prog || 0; "mp" [prog] -> iiííïïi
// (:259398) the same for interactMProg (the wire has no overCharged flag: display = prog > 0). The server sends them only
// to a connection whose specStream is that player (:222279 / :222288).
static void on_inter_prog(KrunkerLink *l, const KrunkerFrame *f, const uint8_t bar) {
    const double prog = krunker_num(arg(f, 0), 0);
    const NetInterProgPacket p = {.bar = bar, .prog = (float) (isnan(prog) ? 0.0 : prog)};
    emit_ext(l, NET_EXT_PLAYER_INTER_PROG, &p, sizeof(p));
}

// ---------------------------------------------------------------- unbox / raffle chat lines

// players.findById(id) (:198775): the player whose id (its "0" socket id) === id; its sid, -1 none
static int sid_by_id(const KrunkerLink *l, const KrunkerValue *v) {
    const char *id = krunker_str(v);
    for (int i = 0; id && id[0] && i < l->player_count; i++) {
        if (!strcmp(l->players[i].id, id)) return l->players[i].sid;
    }
    return -1;
}

// A store index as JS indexes store.skins with it: a whole number >= 0, or a string of one ("812"); -1 otherwise
static int32_t store_index(const KrunkerValue *v) {
    double d = krunker_num(v, -1);
    if (krunker_is(v, KRUNKER_STR)) {
        char *end = NULL;
        d = strtod(krunker_str(v), &end);
        if (!end || *end || end == krunker_str(v)) return -1;
    }
    return d >= 0 && d < INT32_MAX && d == floor(d) ? (int32_t) d : -1;
}

// "unb" [name, itemIndex, kr] (iíïîiìí): the unboxer by name; a truthy kr: the Free KR item store.freeKR[itemIndex]
static void on_unbox(KrunkerLink *l, const KrunkerFrame *f) {
    NetUnboxMessagePacket p;
    memset(&p, 0, sizeof(p));
    p.uid = -1;
    p.kind = NET_UNBOX_NAMED;
    key_string(arg(f, 0), p.name, sizeof(p.name));
    p.item = store_index(arg(f, 1));
    const double kr = krunker_num(arg(f, 2), 0);
    p.kr = truthy(arg(f, 2)) && kr > 0 && kr < INT32_MAX ? (int32_t) kr : 0;
    if (p.item < 0) return; // store.skins[undefined] / freeKR[undefined]: nothing shows
    emit_ext(l, NET_EXT_UNBOX_MESSAGE, &p, sizeof(p));
}

// "unbS" [playerId, skinIndex] (iíiíïíi): `if (skin && player)` (the third argument, false, is not read)
static void on_unbox_player(KrunkerLink *l, const KrunkerFrame *f) {
    const NetUnboxMessagePacket p = {.uid = sid_by_id(l, arg(f, 0)), .item = store_index(arg(f, 1)), .kind = NET_UNBOX_PLAYER};
    if (p.uid < 0 || p.item < 0) return;
    emit_ext(l, NET_EXT_UNBOX_MESSAGE, &p, sizeof(p));
}

// "lotWin" [lotteryId, entryId, prize {type, ref, quantity}, playerId] (iíiìïïi): a winner in this game
// (findById(playerId)); the prize as the line names it. A quantity that is no number reads 0 here.
static void on_lottery_won(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *prize = arg(f, 2);
    NetLotteryPrizePacket p;
    memset(&p, 0, sizeof(p));
    p.uid = sid_by_id(l, arg(f, 3));
    if (p.uid < 0 || !krunker_is(prize, KRUNKER_MAP)) return; // no winner here / prize.type of a non-object throws
    const char *type = krunker_str(krunker_get(prize, "type"));
    p.type = !type ? NET_PRIZE_OTHER : !strcmp(type, "skin") ? NET_PRIZE_SKIN : !strcmp(type, "currency") ? NET_PRIZE_CURRENCY
           : !strcmp(type, "subscription") ? NET_PRIZE_SUBSCRIPTION : NET_PRIZE_OTHER;
    js_join_item(krunker_get(prize, "ref"), p.ref, sizeof(p.ref)); // `${ref}`
    const double q = krunker_num(krunker_get(prize, "quantity"), 0);
    p.quantity = isnan(q) ? 0 : q;
    emit_ext(l, NET_EXT_LOTTERY_PRIZE_WON, &p, sizeof(p));
}

// ---------------------------------------------------------------- map prize wheels

// "spinW" [socketId, uid, error, randNum, skinIndex, funds] (iiîïiiì): `if (!error)`; ours (socketId == our socket's
// id) gives the account the skin and its KR (the client checks it has an account); every client spins the wheel.
static void on_spin_wheel(KrunkerLink *l, const KrunkerFrame *f) {
    if (truthy(arg(f, 2))) return;
    char socket[64];
    key_string(arg(f, 0), socket, sizeof(socket));
    NetSpinWheelPacket p;
    memset(&p, 0, sizeof(p));
    const double rand_num = krunker_num(arg(f, 3), 0), funds = krunker_num(arg(f, 5), 0);
    p.uid = (int32_t) krunker_num(arg(f, 1), -1);
    p.rand = (float) (isnan(rand_num) ? 0.0 : rand_num);
    p.skin = store_index(arg(f, 4));
    p.mine = (uint8_t) (socket[0] && l->socket_id[0] && !strcmp(socket, l->socket_id));
    p.funds = isnan(funds) ? 0 : funds;
    if (p.uid < 0) return; // spinners[i].uid == uid: no spinner has a negative uid
    emit_ext(l, NET_EXT_SPIN_WHEEL_RESPONSE, &p, sizeof(p));
}

// ---------------------------------------------------------------- KR charge consent

// "chreq" [id, amount, modal] (iîiîiïi): `if (account)`, the prompt (the client: #chargeModal or #chargeConfirm, closed
// unanswered after 15 s). Our account on this socket is the one "a" ACCOUNT_RESPONSE named (account_name).
static void on_charge_request(KrunkerLink *l, const KrunkerFrame *f) {
    if (!l->account_name[0]) return;
    const double amount = krunker_num(arg(f, 1), 0);
    const NetChargeConsentRequestPacket p = {.amount = isnan(amount) ? 0 : amount, .id = (int32_t) krunker_num(arg(f, 0), 0),
                                             .modal = (uint8_t) truthy(arg(f, 2))};
    emit_ext(l, NET_EXT_CHARGE_CONSENT_REQUEST, &p, sizeof(p));
}

int krunker_link_s2c_send(KrunkerLink *l, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (subtype != NET_EXT_CHARGE_CONSENT_RESPONSE) return -1;
    if (length < sizeof(NetChargeConsentResponsePacket)) return 0;
    NetChargeConsentResponsePacket r;
    memcpy(&r, body, sizeof(r));
    // iïííìíì (:257700): accept ["chrsp", id, 1], decline ["chrsp", id]
    const KrunkerValue args[] = {KRUNKER_V_NUM(r.id), KRUNKER_V_NUM(1)};
    return krunker_link_send(l, krunker_c2s_wire(KRUNKER_C2S_CHARGE_CONSENT_RESPONSE), args, r.accept ? 2 : 1);
}

// ---------------------------------------------------------------- Turf Wars

static void emit_wars_clan(KrunkerLink *l, const int has, const KrunkerValue *id, const KrunkerValue *name, const KrunkerValue *logo,
                           const KrunkerValue *color) {
    NetWarsClanPacket p;
    memset(&p, 0, sizeof(p));
    p.has = (uint8_t) has;
    if (has) {
        p.clan_id = (int32_t) krunker_num(id, 0);
        js_join_item(name, p.clan_name, sizeof(p.clan_name));
        js_join_item(logo, p.clan_logo, sizeof(p.clan_logo));
        js_join_item(color, p.banner_color, sizeof(p.banner_color));
    }
    emit_ext(l, NET_EXT_WARS_CLAN, &p, sizeof(p));
}

// "warsClan" [clan_id, clan_name, clan_logo, clan_banner_color] (iíìïïíì): clan_id != null ? the clan : null
static void on_wars_clan(KrunkerLink *l, const KrunkerFrame *f) {
    emit_wars_clan(l, !nil(arg(f, 0)), arg(f, 0), arg(f, 1), arg(f, 2), arg(f, 3));
}

// init (:256999): syncData.warsClan ? setClanInfo({its four fields}) : setClanInfo(null), every init. The client's INIT
// already clears the clan (s2c_ui_on_init), so only a clan goes out, right after the INIT.
void krunker_link_s2c_init(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *w = krunker_get(arg(f, 11), "warsClan");
    if (truthy(w))
        emit_wars_clan(l, 1, krunker_get(w, "clan_id"), krunker_get(w, "clan_name"), krunker_get(w, "clan_logo"),
                       krunker_get(w, "clan_banner_color"));
}

// ---------------------------------------------------------------- the editor

// "edMp" [mapJson] (iiíïìiì): `if (map)` the editor opens it (mapToEdit, window.open("/editor.html")). The JSON goes
// to the client as sent, in NetExtBlobChunk slices; the client drops the objects' col / l false and opens its editor.
static void on_edit_map(KrunkerLink *l, const KrunkerFrame *f) {
    const KrunkerValue *map = arg(f, 0);
    if (!truthy(map) || !krunker_is(map, KRUNKER_STR)) return; // a non-string: JSON.parse throws, saveVal("[object ...]")
    const char *text = krunker_str(map);
    const size_t len = map->len, parts = (len + NET_EXT_BLOB_CHUNK_DATA - 1) / NET_EXT_BLOB_CHUNK_DATA;
    if (parts > 65535) return;
    uint8_t body[NET_EXT_BODY_MAX];
    for (size_t p = 0; p < parts; p++) {
        const size_t off = p * NET_EXT_BLOB_CHUNK_DATA;
        const size_t n = len - off < (size_t) NET_EXT_BLOB_CHUNK_DATA ? len - off : (size_t) NET_EXT_BLOB_CHUNK_DATA;
        const NetExtBlobChunk head = {(uint16_t) p, (uint16_t) parts};
        memcpy(body, &head, sizeof(head));
        memcpy(body + sizeof(head), text + off, n);
        emit_ext(l, NET_EXT_KN_OFFICIAL_EDIT_MAP, body, (uint16_t) (sizeof(head) + n));
    }
}

// ---------------------------------------------------------------- dispatch

int krunker_link_s2c_frame(KrunkerLink *l, const KrunkerS2C id, const KrunkerFrame *f) {
    switch (id) {
        case KRUNKER_S2C_PLAYER_ANIM: on_player_anim(l, f); return 1;
        case KRUNKER_S2C_UPDATE_ASSET_ID: on_update_asset_id(l, f); return 1;
        case KRUNKER_S2C_READY: on_ready(l, f); return 1;
        case KRUNKER_S2C_SHOW_SPEECH_BUBBLE: on_speech_bubble(l, f); return 1;
        case KRUNKER_S2C_SYNC_P_CON_DATA: on_lobby_data(l, f); return 1;
        case KRUNKER_S2C_SYNC_P_CON_OBJ: on_lobby_entry(l, f); return 1;
        case KRUNKER_S2C_CON_UID: on_con_uid(l, f); return 1;
        case KRUNKER_S2C_SKIN_DEFINITION_CHANGED: on_skin_definition(l, f); return 1;
        case KRUNKER_S2C_SKIN_STAT: on_skin_stat(l, f); return 1;
        case KRUNKER_S2C_LATEST_RANKED_MMR_CHANGE: on_ranked_mmr(l, f); return 1;
        case KRUNKER_S2C_GAME_ANNOUNCEMENT: on_game_announcement(l, f); return 1;
        case KRUNKER_S2C_FOUND_JUNK: on_found_junk(l, f); return 1;
        case KRUNKER_S2C_SCRIPT_LOG: on_script_log(l, f); return 1;
        case KRUNKER_S2C_CUSTOM_NETWORK_MESSAGE: on_network_message(l, f); return 1;
        case KRUNKER_S2C_INTER_PROG: on_inter_prog(l, f, NET_INTER_BAR_INTERACT); return 1;
        case KRUNKER_S2C_INTER_M_PROG: on_inter_prog(l, f, NET_INTER_BAR_CHARGE); return 1;
        case KRUNKER_S2C_UNBOX_MESSAGE: on_unbox(l, f); return 1;
        case KRUNKER_S2C_UNBOX_MESSSAGE_S: on_unbox_player(l, f); return 1;
        case KRUNKER_S2C_LOTTERY_PRIZE_WON: on_lottery_won(l, f); return 1;
        case KRUNKER_S2C_SPIN_WHEEL_RESPONSE: on_spin_wheel(l, f); return 1;
        case KRUNKER_S2C_CHARGE_CONSENT_REQUEST: on_charge_request(l, f); return 1;
        case KRUNKER_S2C_WARS_CLAN: on_wars_clan(l, f); return 1;
        case KRUNKER_S2C_EDIT_MAP: on_edit_map(l, f); return 1;

        // Taken before the dispatch by name (krunker_link.c account_scoped -> io.account_frame -> the account UI)
        case KRUNKER_S2C_RPC_SUCCESS:
        case KRUNKER_S2C_RPC_ERROR:
        case KRUNKER_S2C_UPDATE_FUNDS:
        case KRUNKER_S2C_CLAN_RESPONSE:
        case KRUNKER_S2C_UPDATE_CHALLENGES:
        case KRUNKER_S2C_UPDATE_CHALLENGES_PROGRESS:
        case KRUNKER_S2C_CHALLENGE_COMPLETED:
        case KRUNKER_S2C_DATA_LOADED:
        case KRUNKER_S2C_SYNC_BP_PROG:
        case KRUNKER_S2C_DAILY_SPIN_CLAIM_RESPONSE:
        // "vr" (REDEEM_VOUCHER_RESPONSE, deob-10.0.4.js:221042) and the real-money purchase's answers (krunker_checkout.h):
        // the main thread's (krunker_net.c account_frames_tick -> kio_store.c / kio_checkout.c)
        case KRUNKER_S2C_REDEEM_VOUCHER_RESPONSE:
        case KRUNKER_S2C_LOAD_XSOLLA:
        case KRUNKER_S2C_PURCHASE_RESPONSE:
        case KRUNKER_S2C_PURCHASE_PRODUCT_RESPONSE:
        case KRUNKER_S2C_PURCHASE_PREMIUM_RESPONSE:
        case KRUNKER_S2C_PURCHASE_BUNDLE_RESPONSE:
        case KRUNKER_S2C_UPDATE_PREMIUM_AMOUNT:
        case KRUNKER_S2C_PURCHASE_RESPONSE_2:
        case KRUNKER_S2C_PURCHASE_LEVEL_RESPONSE:
        case KRUNKER_S2C_PURCHASE_ERROR:
        case KRUNKER_S2C_AVAILABLE_BUNDLE_DATA:
        case KRUNKER_S2C_ALL_BUNDLE_DATA:
            return 1;

        // Ignored on purpose (doc/v10/s2c-coverage.md, "irrelevant" and "KN equivalent" rows): real-money store and
        // payment providers, ads, surveys, Twitch / Discord / MetaMask / NFT linking, the menu's account windows
        // (kn_brain serves them), the developer debug views, and packets whose v10 handler is empty
        case KRUNKER_S2C_FORCE_PING:              // no handler registered in 10.0.3
        case KRUNKER_S2C_GAME_INSTANCE_ID:        // the menu's region label of the game id we joined (the seek knows it)
        case KRUNKER_S2C_ADD_MAIL:
        case KRUNKER_S2C_OPEN_MAIL:
        case KRUNKER_S2C_UPDATE_MAIL:
        case KRUNKER_S2C_CLAIM_ITEM_RESPONSE:
        case KRUNKER_S2C_SURVEY_BUNDLE_RESPONSE:
        case KRUNKER_S2C_POPUP_REGISTER_REWARDS:
        case KRUNKER_S2C_CLAN_LEAVE_RESPONSE:
        case KRUNKER_S2C_UPLOAD_RESPONSE:
        case KRUNKER_S2C_UPLOAD_PROFILE_RESPONSE:
        case KRUNKER_S2C_SPIN_RESPONSE:
        case KRUNKER_S2C_SAVE_SPIN_HISTORY:
        case KRUNKER_S2C_SPIN_KR:
        case KRUNKER_S2C_SPIN_WHEELS_UPDATED:
        case KRUNKER_S2C_SET_DEBUG:
        case KRUNKER_S2C_SET_EXPERIMENTAL:
        case KRUNKER_S2C_NEW_PASS_RESPONSE:
        case KRUNKER_S2C_ADD_DEBUG_POING:
        case KRUNKER_S2C_CRAFT_RESPONSE:
        case KRUNKER_S2C_CONSOLE_LOG:
        case KRUNKER_S2C_SOCKET_PING_RESULT:
        case KRUNKER_S2C_COUNTRY_FLAG_UPDATE:
        case KRUNKER_S2C_BADGE_UPDATE:
        case KRUNKER_S2C_BADGE_CLAIM_RESPONSE:
        case KRUNKER_S2C_BADGE_CLAIM_MULTI_RES:
        case KRUNKER_S2C_HIDE_STATUS:
        case KRUNKER_S2C_PREMIUM_RESPONSE:
        case KRUNKER_S2C_ALIAS_RESPONSE:
        case KRUNKER_S2C_SHOW_POPUP:
        case KRUNKER_S2C_LOADING_POPUP:
        case KRUNKER_S2C_ANNOUNCEMENT:
        case KRUNKER_S2C_TWITCH_DROPS_RESPONSE:
        case KRUNKER_S2C_TWITCH_REMOVE_RESPONSE:
        case KRUNKER_S2C_TWITCH_VERIFY_RESPONSE:
        case KRUNKER_S2C_DISCORD_LINK_RESULT:
        case KRUNKER_S2C_NFT_RESPONSE:
        case KRUNKER_S2C_PROMOS_RESPONSE:
        case KRUNKER_S2C_DEDI_BUY_RESPONSE:
        case KRUNKER_S2C_FRIENDS_LIST_RESPONSE:
        case KRUNKER_S2C_VOTE_MAP:
        case KRUNKER_S2C_UPDATE_SETTINGS:
        case KRUNKER_S2C_ADD_HELPER_C:
        case KRUNKER_S2C_DEBUG_COMPLEX_HITBOX:
        case KRUNKER_S2C_ADD_HITBOX_HELPER:
        case KRUNKER_S2C_DEBUG_RAYCAST:
        case KRUNKER_S2C_FORCE_VIDEO_AD:
        case KRUNKER_S2C_OPEN_METAMASK_AUTH:
        case KRUNKER_S2C_METAMASK_LINK_RESPONSE:
        case KRUNKER_S2C_METAMASK_UNLINK_RESPONSE:
        case KRUNKER_S2C_BOTS_POPUP:
        case KRUNKER_S2C_UPDATE_COUNTRY:
        case KRUNKER_S2C_RECIEVE_DOUBLE_XP:
        case KRUNKER_S2C_GAME_SURVEY:
        case KRUNKER_S2C_GUEST_EARNED_REWARDS:    // the krunker.io guest's FRVR profile, not our kn_brain guest's
        case KRUNKER_S2C_UPDATE_LOTTERY_ENTRIES:
        case KRUNKER_S2C_LOTTERY_PRIZE_WON_PLAYER:
        case KRUNKER_S2C_LOTTERY_STARTED:
        case KRUNKER_S2C_LOTTERY_ENDED:
            return 1;
        default:
            return 0;
    }
}
