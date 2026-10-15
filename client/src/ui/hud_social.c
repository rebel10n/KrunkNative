// v7.1.4 in-match social features (client/include/hud_social.h): mute lists, the v7 chat line,
// quick chat, prop-hunt taunts and the spray keys. Krunker v7.1.4 src/index.js:803-810 (mute lists),
// :11085-11107 (muteUser), :16065-16098 (io_addChat), :16222-16244 (filterCM / quick chat),
// :16450-16490 and :16573-16580 (pressButton / releaseButton: spray, spray wheel, taunts).
#include <hud_social.h>
#include <sprays.h>
#include <backend_ui.h> // account_get: premiumT
#include <console.h>    // ui_console_on_chat: the dev console mirrors chat
#include <player_accounts.h> // player_accounts_is_bot: a bot's chat line is never hidden
#include <html_dom.h>
#include <ui_logic.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_server_caps; // NET_PACKET_SOCIAL_V7 version of the current session, 0 none

void social_reset_server(void) {
    g_server_caps = 0;
}

void social_on_server_caps(const int version) {
    g_server_caps = version;
}

// The offline sandbox is this build's own server; elsewhere the server said so after its INIT.
int social_server_v7(const Client *client) {
    return client && (client->offline || (client->net_server_minor >= NET_PROTOCOL_MINOR_V7 && g_server_caps >= 1));
}

// ---------------------------------------------------------------- mute lists

#define SOCIAL_MUTE_MAX 512

typedef struct {
    const char *key;
    int loaded;
    int count;
    int ids[SOCIAL_MUTE_MAX];
} MuteList;

static MuteList g_mute[2] = {{.key = "mute_chat"}, {.key = "mute_voice"}};

// getSavedVal(key) parsed as JSON: [accountId, ...] (numbers; strings of digits are taken too)
static MuteList *mute_list(const int voice) {
    MuteList *list = &g_mute[voice ? 1 : 0];
    if (list->loaded) return list;
    list->loaded = 1;
    list->count = 0;
    char buf[SOCIAL_MUTE_MAX * 12];
    if (html_storage_get(list->key, buf, sizeof(buf)) < 0) return list;
    cJSON *json = cJSON_Parse(buf);
    const cJSON *item;
    if (!cJSON_IsArray(json)) {
        cJSON_Delete(json);
        return list;
    }
    cJSON_ArrayForEach(item, json) {
        int id = 0;
        if (cJSON_IsNumber(item)) id = item->valueint;
        else if (cJSON_IsString(item)) id = atoi(item->valuestring);
        if (id > 0 && list->count < SOCIAL_MUTE_MAX) list->ids[list->count++] = id;
    }
    cJSON_Delete(json);
    return list;
}

static int mute_index(const MuteList *list, const int account_id) {
    for (int i = 0; i < list->count; i++) {
        if (list->ids[i] == account_id) return i;
    }
    return -1;
}

int social_chat_muted(const int account_id) {
    return account_id > 0 && mute_index(mute_list(0), account_id) >= 0;
}

int social_voice_muted(const int account_id) {
    return account_id > 0 && mute_index(mute_list(1), account_id) >= 0;
}

// muteUser(elm, id, type): the account in or out of its list, saved (saveVal: JSON). v7 saves the
// voice list under "mute_chat", overwriting the chat list; KrunkNative saves it under "mute_voice".
int social_toggle_mute(const int account_id, const int voice) {
    if (account_id <= 0) return 0;
    MuteList *list = mute_list(voice);
    const int at = mute_index(list, account_id);
    if (at >= 0) list->ids[at] = list->ids[--list->count];
    else if (list->count < SOCIAL_MUTE_MAX) list->ids[list->count++] = account_id;
    cJSON *json = cJSON_CreateIntArray(list->ids, list->count);
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    if (text) html_storage_set(list->key, text);
    free(text);
    cJSON_Delete(json);
    return at < 0;
}

void social_mute_reload(void) {
    g_mute[0].loaded = g_mute[1].loaded = 0;
}

// ---------------------------------------------------------------- chat

void social_on_chat_v7(Client *client, const NetChatV7Packet *packet) {
    (void) client;
    if (!packet || social_chat_muted(packet->account_id)) return; // mutedChatAccountIds.includes(accountId)
    char name[PLAYER_NAME_SIZE];
    char text[NET_CHAT_TEXT_SIZE];
    memcpy(name, packet->chat.name, sizeof(name));
    memcpy(text, packet->chat.text, sizeof(text));
    name[sizeof(name) - 1] = 0;
    text[sizeof(text) - 1] = 0;
    ui_console_on_chat(name[0] ? name : "unknown", text, NET_CHAT_PLAYER);
    hud_ui_add_chat_channel(name[0] ? name : "unknown", text, NULL, packet->channel, !player_accounts_is_bot(packet->chat.uid));
}

int social_quick_chat(Client *client, const char *text) {
    static double last;
    if (!client || !text || !text[0]) return 0;
    if (!ui_controls_chat_text_allowed(text)) return 0; // showMessages off: quick chat is player chat too
    const double now = glfwGetTime();
    if (last > 0.0 && now - last <= QUICK_CHAT_COOLDOWN) return 0; // Date.now() - lastMsgTime <= 1500
    client_chat_send(client, text);                                    // f1909
    last = now;
    return 1;
}

// ---------------------------------------------------------------- taunts

int social_taunt(Client *client, const int index) {
    if (!client || !client->me || !client->me->active || index < 0 || index >= TAUNT_COUNT) return 0;
    if (!social_server_v7(client) || !game_player_is_prop(&client->game, client->me)) return 0; // teamOptions[team - 1] == "Prop"
    const NetTauntPacket taunt = {.index = (uint8_t) index};
    client_net_send(client, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    return 1;
}

// ---------------------------------------------------------------- sprays

// Game.isSandbox || account && account.premiumT > 0; the wheel's "spry" index needs a v7 server.
int social_spray_wheel_allowed(const Client *client) {
    if (!client || !social_server_v7(client)) return 0;
    const AccountInfo *account = account_get();
    return client->offline || (account && account->premium);
}

static void spray_send(Client *client, const int index) {
    if (social_server_v7(client)) {
        const NetSprayRequestPacket spray = {.index = index};
        client_net_send(client, NET_PACKET_SPRAY_V7, &spray, sizeof(spray)); // io.send("spry"[, index])
    } else {
        const NetInteractPacket spray = {.action = 0}; // v3: io.send("int", 0)
        client_net_send(client, NET_PACKET_INTERACT, &spray, sizeof(spray));
    }
}

// pressButton, with an active player: the wheel key opens the wheel when it is available,
// otherwise the spray key sprays (both are F by default: one or the other).
void social_spray_press(Client *client, const int spray_key, const int wheel_key) {
    if (!client || !client->me || !client->me->active) return;
    if (wheel_key && social_spray_wheel_allowed(client)) {
        sprays_wheel_open(); // overlay.propWheelClose(); overlay.sprayWheelOpen()
    } else if (spray_key) {
        spray_send(client, -1);
    }
}

// releaseButton: the wheel key sprays the chosen item and closes the wheel (also a quick tap,
// before the wheel showed: the equipped spray).
void social_spray_release(Client *client, const int wheel_key) {
    if (!wheel_key || !sprays_wheel_is_open()) return;
    const int index = sprays_wheel_selected();
    sprays_wheel_close();
    if (client && social_spray_wheel_allowed(client)) spray_send(client, index);
}

int social_spray_scroll(const double dy) {
    if (!sprays_wheel_opened() || !dy) return 0;
    sprays_wheel_change(dy > 0.0 ? -1 : 1); // wheelDelta > 0 (up) -> -1
    return 1;
}

// ---------------------------------------------------------------- prop hunt HUD

// index.js (per menu update): on the Prop team #propControlHolder replaces #botRightHider;
// updateKeybindUI writes the taunt keys into #taunt0..5. Re-applied whenever hud_ui.c's
// visibility pass (layout_epoch) reset the holders.
void social_prop_hud_tick(Client *client, const int visible, const int layout_epoch) {
    static int shown = -1, epoch = -1, keys[TAUNT_COUNT];
    const int prop = visible && client && client->me && game_player_is_prop(&client->game, client->me);
    if (prop != shown || layout_epoch != epoch) {
        if (prop || shown == 1) {
            const HtmlElem holder = html_dom_by_id("propControlHolder");
            const HtmlElem bot = html_dom_by_id("botRightHider");
            if (holder) html_dom_set_display(holder, prop ? "block" : "none");
            if (bot && visible) html_dom_set_display(bot, prop ? "none" : "block");
        }
        shown = prop;
        epoch = layout_epoch;
        for (int i = 0; i < TAUNT_COUNT; i++) keys[i] = -2; // labels again
    }
    if (!prop) return;
    for (int i = 0; i < TAUNT_COUNT; i++) {
        const int code = ui_controls_v7_val("taunt", i);
        if (code == keys[i]) continue;
        keys[i] = code;
        char id[16], name[32], text[40];
        snprintf(id, sizeof(id), "taunt%d", i);
        snprintf(text, sizeof(text), "[%s]", ui_controls_key_name(code, name, sizeof(name))); // "[" + UTILS.getKeyName(val) + "]"
        const HtmlElem el = html_dom_by_id(id);
        if (el) html_dom_set_text(el, text);
    }
}
