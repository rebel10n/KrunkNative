// Bot chat through kn_brain (server/include/bot_banter.h): the game server never holds the AI
// gateway's key; it asks the brain, whose services/brain/ai.c writes the line (request kind
// `bot-banter`) and answers `bot-banter-reply` {rid, ok, text | err}. The request id of
// shared/src/bot_chat.c is the frame's rid, so the reply routes itself.
#include <bot_banter.h>
#include <bot_chat.h>
#include <brain.h>
#include <server.h>
#include <server_config.h>
#include <server_log.h>

#include <string.h>

// A signed-in player is in the game (not a spectator / menu client): guests-only lobbies get
// templates, so the AI budget goes to accounts (ai.h: the editor's AI needs a sign-in too).
static int banter_account_present(void) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *c = &g_server->clients[i];
        if (c->player && c->acct.account_id > 0) return 1;
    }
    return 0;
}

int bot_banter_request(Game *game, const struct BotChatRequest_t *req) {
    (void) game;
    if (!req || !g_gs_config.bot_chat_ai || !brain_linked() || !banter_account_present()) return 0;
    cJSON *f = cJSON_CreateObject();
    if (!f) return 0;
    cJSON_AddStringToObject(f, "type", "bot-banter");
    cJSON_AddNumberToObject(f, "rid", req->id);
    cJSON_AddStringToObject(f, "name", req->bot_name);
    cJSON_AddStringToObject(f, "personality", req->personality ? req->personality : "chill");
    cJSON_AddStringToObject(f, "event", req->event ? req->event : "win");
    if (req->other[0]) cJSON_AddStringToObject(f, "other", req->other);
    cJSON_AddStringToObject(f, "mode", req->mode ? req->mode : "");
    cJSON_AddStringToObject(f, "map", req->map ? req->map : "");
    cJSON_AddNumberToObject(f, "kills", req->kills);
    cJSON_AddNumberToObject(f, "deaths", req->deaths);
    cJSON_AddNumberToObject(f, "place", req->place);
    cJSON_AddNumberToObject(f, "players", req->players);
    if (brain_post(f, 0) != 0) return 0; // not reliable: a lost line is a template line
    server_log("[bot-chat] asked the brain for a %s line of %s (#%d)", req->event ? req->event : "?", req->bot_name, req->id);
    return 1;
}

int bot_banter_on_brain_frame(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    if (!cJSON_IsNumber(rid)) return 1;
    const int ok = cJSON_IsTrue(cJSON_GetObjectItem(frame, "ok"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "text"));
    if (!ok) {
        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "err"));
        server_log("[bot-chat] AI line %d failed (%s): template", (int) rid->valuedouble, err && err[0] ? err : "no reason");
        text = NULL;
    }
    bot_chat_deliver(&g_server->game, (int) rid->valuedouble, text);
    return 1;
}
