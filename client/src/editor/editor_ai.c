// The map editor's AI panel (editor_ai.h): a compact chat docked over the 3D
// view. The interaction model is abulxt's AI chat: model picker, streamed answers rendered as
// markdown, collapsed "Thought for Ns" traces, tool calls as expandable cards, Stop / Retry / Copy,
// local history, a usage line.
//
// Transport: kn_brain's POST /ai/chat (shared/include/ai_proto.h "kn_brain's own stream") with the
// account's session token; the brain injects the provider key, so the client never holds one. AI is
// for signed-in accounts only: a guest sees "Sign in to use AI". A user may instead point the panel
// at an OpenAI-compatible endpoint of their own (URL + key kept in this machine's storage only,
// never sent to kn_brain), an explicit mode with a warning; the same stream parser serves both.
#include "editor_ai.h"
#include <ai_proto.h>
#include <app_mode.h>
#include <backend.h>
#include <backend_ui.h>
#include <http_stream.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AI_RENDER_EVERY_S 0.066  // live message re-render while streaming (~15 Hz)
#define AI_MAX_CONV_BYTES (640u * 1024u) // older turns are dropped from a request beyond this (brain body limit 1 MiB)
#define AI_MAX_TOOL_ROUNDS 25    // automatic model <-> tool rounds after one user message
#define AI_HISTORY_MAX 40        // chats kept in storage (knAiChats)
#define AI_HISTORY_BYTES (1536u * 1024u)

static struct {
    int built, open;
    HtmlElem root, list, live, input, model_sel, banner, usage, send, stop, account;
    // models (GET /ai/models)
    cJSON *models;           // normalised rows
    char model[96];
    int models_state;        // 0 none, 1 loading, 2 loaded, 3 failed
    char models_error[256];
    char health[16];         // "healthy" / "degraded" / "down" / ...
    char health_reason[256];
    double models_at;        // g_ed.now of the last load
    int signed_in_seen;      // account id the panel was drawn for (-1 never)
    // conversation
    cJSON *conv;             // chat-shape messages (no system prompt); assistant "meta" for the UI
    // the turn in flight
    HttpStream *hs;
    int streaming;
    int http_status;
    int sse;
    AiStream st;
    AiBuf errbody;
    double started, think_end, render_at;
    int live_dirty;
    cJSON *done;             // kn.done of the turn (owned)
    cJSON *tool_starts;      // names announced by kn.tool_start
    cJSON *server_tools;     // kn.server_tool events by id (object)
    char live_model[96];
    // usage
    int64_t sess_in, sess_out;
    double sess_cost;
    int req_today;
    int64_t tok_today;
    char banner_text[400];
    int banner_kind;         // 0 none, 1 info, 2 warning, 3 error, 4 sign-in
    // tools
    int tools_on;            // offer the editor tools to the model (knAiSettings.tools, default on)
    int web_on;              // brain web search (knAiSettings.web, default on)
    int allow_all;           // approvals: every tool allowed for this session
    int rounds;              // automatic tool rounds since the user's last message
    int tool_step;           // a tool call waits to run on the next tick
    char effort[16];         // "" = the model's default, low / medium / high
    cJSON *server_tool_names; // the brain's server tools (names)
    // the user's own endpoint (knAiEndpoint: never sent to kn_brain)
    int custom;
    char ep_url[512], ep_key[512], ep_model[96];
    HttpStream *ep_models_hs;
    AiBuf ep_models_body;
    int ep_models_status;
    // history (knAiChats) and popovers
    char chat_id[32];
    int pop;                 // 0 none, 1 history, 2 settings
    HtmlElem pop_el;
} A;

static void render_all(void);
static void render_live(void);
static void set_banner(int kind, const char *text);
static void send_turn(void);
static void update_controls(void);
static void process_tools(void);
static void history_save(void);
static void models_load(void);

// ---------------------------------------------------------------------------
// helpers

static const cJSON *model_row(const char *id) {
    const cJSON *m;
    cJSON_ArrayForEach(m, A.models) {
        if (!strcmp(ed_json_str(m, "id", ""), id)) return m;
    }
    return NULL;
}

static const char *model_label(const char *id) {
    const cJSON *m = model_row(id);
    return m ? ed_json_str(m, "name", id) : id;
}

static void fmt_int(char *out, const size_t n, const int64_t v) {
    // 12345 -> "12,345"
    char raw[32];
    snprintf(raw, sizeof(raw), "%lld", (long long) (v < 0 ? -v : v));
    const size_t len = strlen(raw);
    size_t o = 0;
    if (v < 0 && o + 1 < n) out[o++] = '-';
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        out[o++] = raw[i];
        if ((len - i - 1) % 3 == 0 && i + 1 < len && o + 1 < n) out[o++] = ',';
    }
    out[o] = 0;
}

static void fmt_cost(char *out, const size_t n, const double usd) {
    if (usd < 0) out[0] = 0;
    else if (usd < 0.0001) snprintf(out, n, "<$0.0001");
    else if (usd < 1) snprintf(out, n, "$%.4f", usd);
    else snprintf(out, n, "$%.2f", usd);
}

static int signed_in(void) { return account_get() != NULL && account_session_token()[0]; }

static int brain_ready(void) { return backend_enabled(); }

static char *brain_url(const char *path) {
    const char *base = backend_url();
    const size_t n = strlen(base) + strlen(path) + 1;
    char *u = malloc(n);
    if (u) snprintf(u, n, "%s%s", base, path);
    return u;
}

// ---------------------------------------------------------------------------
// settings (storage: knAiSettings {tools, web, effort, model}; knAiEndpoint {custom, url, key, model}
// stays on this machine: the key is never sent anywhere but to its own URL)

static char *storage_text(const char *key) {
    const int n = html_storage_get(key, NULL, 0);
    if (n <= 0) return NULL;
    char *t = malloc((size_t) n + 1);
    if (t) html_storage_get(key, t, (size_t) n + 1);
    return t;
}

static int json_flag(const cJSON *o, const char *key, const int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return v ? ed_json_truthy(v) : def;
}

static void settings_load(void) {
    char *t = storage_text("knAiSettings");
    cJSON *o = t ? cJSON_Parse(t) : NULL;
    free(t);
    A.tools_on = json_flag(o, "tools", 1);
    A.web_on = json_flag(o, "web", 1);
    snprintf(A.effort, sizeof(A.effort), "%s", ed_json_str(o, "effort", ""));
    if (o && ed_json_str(o, "model", "")[0]) snprintf(A.model, sizeof(A.model), "%s", ed_json_str(o, "model", ""));
    cJSON_Delete(o);
    t = storage_text("knAiEndpoint");
    o = t ? cJSON_Parse(t) : NULL;
    if (t) memset(t, 0, strlen(t));
    free(t);
    A.custom = json_flag(o, "custom", 0);
    snprintf(A.ep_url, sizeof(A.ep_url), "%s", ed_json_str(o, "url", ""));
    snprintf(A.ep_key, sizeof(A.ep_key), "%s", ed_json_str(o, "key", ""));
    snprintf(A.ep_model, sizeof(A.ep_model), "%s", ed_json_str(o, "model", ""));
    cJSON_Delete(o);
    if (A.custom && !A.ep_url[0]) A.custom = 0;
}

static void settings_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "tools", A.tools_on);
    cJSON_AddBoolToObject(o, "web", A.web_on);
    cJSON_AddStringToObject(o, "effort", A.effort);
    if (!A.custom) cJSON_AddStringToObject(o, "model", A.model);
    char *t = cJSON_PrintUnformatted(o);
    if (t) html_storage_set("knAiSettings", t);
    free(t);
    cJSON_Delete(o);
}

static void endpoint_save(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "custom", A.custom);
    cJSON_AddStringToObject(o, "url", A.ep_url);
    cJSON_AddStringToObject(o, "key", A.ep_key);
    cJSON_AddStringToObject(o, "model", A.ep_model);
    char *t = cJSON_PrintUnformatted(o);
    if (t) {
        html_storage_set("knAiEndpoint", t);
        memset(t, 0, strlen(t));
    }
    free(t);
    cJSON_Delete(o);
}

// ---------------------------------------------------------------------------
// panel markup (ui/css/editor-ai.css)

static void build(void) {
    if (A.built && html_dom_by_id("knAi")) return;
    HtmlElem center = html_dom_by_id("center");
    if (!center) return;
    html_dom_insert_html(center, HTML_INSERT_BEFORE_END,
        "<div id='knAi' class='knAi' style='display:none'>"
        "<div class='knAiHead'>"
        "<div class='knAiTitle'><i class='material-icons knAiSpark'>auto_awesome</i>AI</div>"
        "<select id='knAiModel' class='knAiModel' onchange='KE.kn.aiModel(this.value)'></select>"
        "<div class='knAiIcon material-icons' title='New chat' onclick='KE.kn.aiNew()'>add_comment</div>"
        "<div id='knAiHistBtn' class='knAiIcon material-icons' title='History' onclick='KE.kn.aiPop(1)'>history</div>"
        "<div id='knAiSetBtn' class='knAiIcon material-icons' title='Settings' onclick='KE.kn.aiPop(2)'>tune</div>"
        "<div class='knAiIcon material-icons' title='Close' onclick='KE.kn.aiToggle()'>close</div>"
        "</div>"
        "<div id='knAiPop' class='knAiPop' style='display:none'></div>"
        "<div id='knAiBanner' class='knAiBanner' style='display:none'></div>"
        "<div id='knAiList' class='knAiList'></div>"
        "<div class='knAiComposer'>"
        "<textarea id='knAiInput' class='knAiInput' rows='2' spellcheck='false' placeholder='Ask anything about this map'"
        " onkeydown='return KE.kn.aiKey()'></textarea>"
        "<div class='knAiBar'>"
        "<div id='knAiAccount' class='knAiAccount'></div>"
        "<div id='knAiTools' class='knAiChip' title='Let the AI read and edit the open map' onclick='KE.kn.aiChip(0)'>Tools</div>"
        "<div id='knAiWeb' class='knAiChip' title='Let the AI search the web' onclick='KE.kn.aiChip(1)'>Web</div>"
        "<div id='knAiSend' class='knAiSend' onclick='KE.kn.aiSend()'>Ask</div>"
        "<div id='knAiStop' class='knAiStop' style='display:none' onclick='KE.kn.aiStop()'>&#9632; Stop</div>"
        "</div>"
        "<div id='knAiUsage' class='knAiUsage'></div>"
        "</div>"
        "</div>");
    A.root = html_dom_by_id("knAi");
    A.list = html_dom_by_id("knAiList");
    A.input = html_dom_by_id("knAiInput");
    A.model_sel = html_dom_by_id("knAiModel");
    A.banner = html_dom_by_id("knAiBanner");
    A.usage = html_dom_by_id("knAiUsage");
    A.send = html_dom_by_id("knAiSend");
    A.stop = html_dom_by_id("knAiStop");
    A.account = html_dom_by_id("knAiAccount");
    A.pop_el = html_dom_by_id("knAiPop");
    A.built = A.root != 0;
    A.signed_in_seen = -1;
    if (!A.conv) A.conv = cJSON_CreateArray();
    settings_load();
}

static void set_banner(const int kind, const char *text) {
    A.banner_kind = text && text[0] ? kind : 0;
    snprintf(A.banner_text, sizeof(A.banner_text), "%s", text ? text : "");
    if (!A.banner) return;
    if (!A.banner_kind) {
        html_dom_set_display(A.banner, "none");
        return;
    }
    EdBuf b;
    edb_init(&b);
    static const char *const cls[] = {"", "knAiInfo", "knAiWarn", "knAiErr", "knAiSign"};
    edb_addf(&b, "<div class='%s'>", cls[kind]);
    if (kind == 4) {
        edb_add(&b, "<div class='knAiSignTitle'><i class='material-icons'>lock</i>Sign in to use AI</div><div class='knAiSignText'>");
        ai_html_escape(&b, text);
        edb_add(&b, "</div><div class='knAiSignBtn' onclick='KE.kn.aiSignIn()'>Sign in</div>");
    } else {
        ai_html_escape(&b, text);
        if (kind == 3) edb_add(&b, "<span class='knAiDismiss material-icons' onclick='KE.kn.aiDismiss()'>close</span>");
    }
    edb_add(&b, "</div>");
    html_dom_set_html(A.banner, b.buf ? b.buf : "");
    html_dom_set_display(A.banner, "");
    edb_free(&b);
}

static void health_banner(void) {
    if (A.banner_kind == 3 || A.banner_kind == 4) return; // errors and the sign-in card win
    if (A.custom) { // the brain's health says nothing about the user's own endpoint
        if (A.banner_kind == 2) set_banner(0, NULL);
        return;
    }
    if (!strcmp(A.health, "degraded")) {
        char t[400];
        snprintf(t, sizeof(t), "AI service degraded: %s", A.health_reason[0] ? A.health_reason : "answers may ignore your messages");
        set_banner(2, t);
    } else if (!strcmp(A.health, "down")) {
        char t[400];
        snprintf(t, sizeof(t), "AI service unavailable: %s", A.health_reason);
        set_banner(2, t);
    } else if (A.banner_kind == 2) {
        set_banner(0, NULL);
    }
}

static void update_account(void) {
    if (!A.account) return;
    const AccountInfo *acc = account_get();
    EdBuf b;
    edb_init(&b);
    if (acc) {
        edb_add(&b, "<i class='material-icons'>account_circle</i>");
        ai_html_escape(&b, acc->alias[0] ? acc->alias : acc->name);
    } else {
        edb_add(&b, "<i class='material-icons'>person_outline</i>Guest");
    }
    html_dom_set_html(A.account, b.buf ? b.buf : "");
    edb_free(&b);
    A.signed_in_seen = acc ? acc->id : 0;
}

static void update_usage(void) {
    if (!A.usage) return;
    char in[32], out[32], cost[32], tok[32], line[320];
    fmt_int(in, sizeof(in), A.sess_in);
    fmt_int(out, sizeof(out), A.sess_out);
    fmt_cost(cost, sizeof(cost), A.sess_cost);
    fmt_int(tok, sizeof(tok), A.tok_today);
    if (!A.sess_in && !A.sess_out && !A.req_today) {
        html_dom_set_text(A.usage, A.custom ? "Your own endpoint: its provider bills your key" : "Free for signed-in accounts");
        return;
    }
    if (A.custom || !A.req_today)
        snprintf(line, sizeof(line), "This chat: %s in / %s out%s%s%s", in, out, cost[0] ? " \xc2\xb7 " : "", cost, cost[0] ? " (est.)" : "");
    else
        snprintf(line, sizeof(line), "This chat: %s in / %s out%s%s%s \xc2\xb7 today: %d requests, %s tokens", in, out, cost[0] ? " \xc2\xb7 " : "",
                 cost, cost[0] ? " (est.)" : "", A.req_today, tok);
    html_dom_set_text(A.usage, line);
}

static int model_has_tools(void) {
    const cJSON *row = model_row(A.custom ? A.ep_model : A.model);
    const cJSON *t = row ? cJSON_GetObjectItemCaseSensitive(row, "tools") : NULL;
    return !t || ed_json_truthy(t); // unknown: offer them
}

static int web_available(void) { return !A.custom && cJSON_GetArraySize(A.server_tool_names) > 0; }

static void update_controls(void) {
    if (!A.built) return;
    const int can = signed_in() && (A.custom || brain_ready());
    const int busy = A.streaming || A.tool_step; // Stop also ends a run of tool calls
    html_dom_set_display(A.send, busy ? "none" : "");
    html_dom_set_display(A.stop, busy ? "" : "none");
    html_dom_set_class(A.send, "knAiOff", !can);
    if (can) html_dom_remove_attr(A.input, "disabled");
    else html_dom_set_attr(A.input, "disabled", "");
    HtmlElem tools = html_dom_by_id("knAiTools"), web = html_dom_by_id("knAiWeb");
    html_dom_set_display(tools, ai_tools_available() ? "" : "none");
    html_dom_set_class(tools, "knAiOn", A.tools_on && model_has_tools());
    html_dom_set_attr(tools, "title", !model_has_tools() ? "This model cannot use tools" : A.tools_on ? "Editor tools on: the AI can read and edit the open map"
                                                                                                        : "Editor tools off");
    html_dom_set_display(web, web_available() ? "" : "none");
    html_dom_set_class(web, "knAiOn", A.web_on && model_has_tools());
    html_dom_set_class(html_dom_by_id("knAiSetBtn"), "knAiOn", A.custom);
    update_account();
    update_usage();
}

static void fill_models(void) {
    if (!A.model_sel) return;
    EdBuf b;
    edb_init(&b);
    const char *cur = A.custom ? A.ep_model : A.model;
    int found = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, A.models) found |= !strcmp(ed_json_str(m, "id", ""), cur);
    if (!cJSON_GetArraySize(A.models) || (A.custom && cur[0] && !found)) {
        edb_add(&b, "<option value='");
        ai_html_escape(&b, cur);
        edb_add(&b, "'>");
        if (A.custom && cur[0]) ai_html_escape(&b, cur);
        else edb_add(&b, A.models_state == 1 ? "Loading models..." : "No models");
        edb_add(&b, "</option>");
    }
    cJSON_ArrayForEach(m, A.models) {
        const char *id = ed_json_str(m, "id", "");
        edb_add(&b, "<option value='");
        ai_html_escape(&b, id);
        edb_addf(&b, "'%s>", !strcmp(id, cur) ? " selected" : "");
        ai_html_escape(&b, ed_json_str(m, "name", id));
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "reasoning"))) edb_add(&b, " \xc2\xb7 thinks");
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "tools"))) edb_add(&b, " \xc2\xb7 tools");
        const double pin = ed_json_num(m, "price_in", -1), pout = ed_json_num(m, "price_out", -1);
        if (pin >= 0 && pout >= 0) edb_addf(&b, " \xc2\xb7 $%.2f/$%.2f", pin, pout);
        const char *st = ed_json_str(m, "status", "");
        if (!strcmp(st, "unavailable")) edb_add(&b, " \xc2\xb7 unavailable");
        else if (!strcmp(st, "degraded")) edb_add(&b, " \xc2\xb7 degraded");
        edb_add(&b, "</option>");
    }
    html_dom_set_html(A.model_sel, b.buf ? b.buf : "");
    if (cur[0]) html_dom_set_value(A.model_sel, cur);
    edb_free(&b);
}

// The selected model's known trouble (kn_brain's per-model probe / chat results): a banner that
// says why and offers the service's default instead. 1 when shown.
static char g_default_model[96];

static int model_warning(void) {
    if (A.custom || A.banner_kind == 3 || A.banner_kind == 4) return 0;
    const cJSON *row = model_row(A.model);
    if (!row || strcmp(ed_json_str(row, "status", ""), "unavailable")) {
        if (A.banner_kind == 2 && !strncmp(A.banner_text, "This model is unavailable", 25)) set_banner(0, NULL);
        return 0;
    }
    char t[400];
    snprintf(t, sizeof(t), "This model is unavailable: %s", ed_json_str(row, "status_reason", "the provider refuses it"));
    set_banner(2, t);
    if (g_default_model[0] && strcmp(g_default_model, A.model) && A.banner) {
        EdBuf b;
        edb_init(&b);
        edb_add(&b, "<span class='knAiAct knAiSwitch' onclick=\"KE.kn.aiUse('");
        ai_html_escape(&b, g_default_model);
        edb_add(&b, "')\">Use ");
        ai_html_escape(&b, model_label(g_default_model));
        edb_add(&b, "</span>");
        html_dom_insert_html(html_dom_first_child(A.banner), HTML_INSERT_BEFORE_END, b.buf ? b.buf : "");
        edb_free(&b);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// GET /ai/models (kn_brain) or <url>/models (the user's own endpoint)

static void models_apply(cJSON *o, const int status, const char *transport_error) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(o, "models");
    if (!A.custom) {
        const cJSON *health = cJSON_GetObjectItemCaseSensitive(o, "health");
        snprintf(A.health, sizeof(A.health), "%s", ed_json_str(health, "state", ""));
        snprintf(A.health_reason, sizeof(A.health_reason), "%s", ed_json_str(health, "reason", ""));
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(o, "server_tools");
        if (cJSON_IsArray(st)) {
            cJSON_Delete(A.server_tool_names);
            A.server_tool_names = cJSON_Duplicate(st, 1);
        }
        if (ed_json_str(o, "default", "")[0]) snprintf(g_default_model, sizeof(g_default_model), "%s", ed_json_str(o, "default", ""));
    }
    if (cJSON_IsArray(list) && cJSON_GetArraySize(list) > 0) {
        cJSON_Delete(A.models);
        A.models = cJSON_Duplicate(list, 1);
        A.models_state = 2;
        A.models_error[0] = 0;
        char *cur = A.custom ? A.ep_model : A.model;
        const size_t cn = A.custom ? sizeof(A.ep_model) : sizeof(A.model);
        if (!cur[0] || (!A.custom && !model_row(cur))) snprintf(cur, cn, "%s", ed_json_str(o, "default", ""));
        if (!cur[0] || (!A.custom && !model_row(cur))) snprintf(cur, cn, "%s", ed_json_str(cJSON_GetArrayItem(A.models, 0), "id", ""));
    } else {
        A.models_state = 3;
        snprintf(A.models_error, sizeof(A.models_error), "%s",
                 o ? ed_json_str(o, "message", "the AI service is not available") : status ? "the AI service answered with an error" : transport_error);
    }
    if (!A.built) return;
    fill_models();
    if (A.models_state == 3 && A.banner_kind != 4 && !A.custom) {
        char t[400];
        snprintf(t, sizeof(t), "AI unavailable: %s", A.models_error);
        set_banner(2, t);
    } else if (!model_warning()) {
        health_banner();
    }
    update_controls();
}

static void models_done(void *user, const int status, const char *body, const size_t len) {
    (void) user;
    if (A.custom) return; // switched to the own endpoint meanwhile
    cJSON *o = status ? cJSON_ParseWithLength(body, len) : NULL;
    models_apply(o, status, body);
    cJSON_Delete(o);
}

static void ep_models_open(void *user, const int status, const char *ctype) {
    (void) user; (void) ctype;
    A.ep_models_status = status;
}

static void ep_models_data(void *user, const char *data, const size_t len) {
    (void) user;
    if (A.ep_models_body.len < (4u << 20)) ai_buf_add(&A.ep_models_body, data, len);
}

static void ep_models_done(void *user, const int status, const char *error) {
    (void) user;
    A.ep_models_hs = NULL;
    cJSON *list = status == 200 ? ai_models_normalise(A.ep_models_body.data ? A.ep_models_body.data : "", A.ep_models_body.len) : NULL;
    cJSON *o = cJSON_CreateObject();
    if (list) cJSON_AddItemToObject(o, "models", list);
    else {
        char e[240] = "";
        if (status) ai_error_text(A.ep_models_body.data, A.ep_models_body.len, e, sizeof(e));
        char m[320];
        snprintf(m, sizeof(m), "%s: %s", status ? "the endpoint's model list" : "cannot reach the endpoint", status ? e : (error ? error : "network error"));
        cJSON_AddStringToObject(o, "message", m);
    }
    ai_buf_free(&A.ep_models_body);
    if (A.custom) models_apply(o, status, error);
    cJSON_Delete(o);
}

static char *ep_url(const char *path) {
    size_t n = strlen(A.ep_url);
    while (n && A.ep_url[n - 1] == '/') n--;
    const size_t cap = n + strlen(path) + 1;
    char *u = malloc(cap);
    if (u) snprintf(u, cap, "%.*s%s", (int) n, A.ep_url, path);
    return u;
}

static void models_load(void) {
    if (A.models_state == 1) return;
    if (A.custom) {
        if (!A.ep_url[0]) return;
        if (A.ep_models_hs) http_stream_cancel(A.ep_models_hs);
        ai_buf_free(&A.ep_models_body);
        char *u = ep_url("/models");
        char headers[600] = "";
        if (A.ep_key[0]) snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", A.ep_key);
        static const HttpStreamCallbacks cb = {ep_models_open, ep_models_data, ep_models_done};
        A.ep_models_hs = u ? http_stream_start("GET", u, headers[0] ? headers : NULL, NULL, &cb, NULL) : NULL;
        memset(headers, 0, sizeof(headers));
        free(u);
        A.models_state = A.ep_models_hs ? 1 : 3;
        A.models_at = g_ed.now;
        fill_models();
        return;
    }
    if (!brain_ready()) return;
    char *u = brain_url("/ai/models");
    if (!u) return;
    A.models_state = 1;
    A.models_at = g_ed.now;
    if (!backend_http_request("GET", u, NULL, models_done, NULL)) A.models_state = 3;
    free(u);
    fill_models();
}

// ---------------------------------------------------------------------------
// rendering

static const cJSON *jget(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }

static cJSON *meta_of(cJSON *m) {
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(m, "meta");
    if (!cJSON_IsObject(meta)) {
        cJSON_DeleteItemFromObjectCaseSensitive(m, "meta");
        meta = cJSON_AddObjectToObject(m, "meta");
    }
    return meta;
}

// meta.tools[id] of an assistant message (created)
static cJSON *tool_meta(cJSON *m, const char *id) {
    cJSON *meta = meta_of(m);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(meta, "tools");
    if (!cJSON_IsObject(tools)) tools = cJSON_AddObjectToObject(meta, "tools");
    cJSON *tm = cJSON_GetObjectItemCaseSensitive(tools, id);
    if (!cJSON_IsObject(tm)) tm = cJSON_AddObjectToObject(tools, id);
    return tm;
}

// the tool result message answering call `id` (searching after index `from`)
static const cJSON *tool_result(const char *id, const int from) {
    const int n = cJSON_GetArraySize(A.conv);
    for (int i = from + 1; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(A.conv, i);
        if (!strcmp(ed_json_str(m, "role", ""), "tool") && !strcmp(ed_json_str(m, "tool_call_id", ""), id)) return m;
    }
    return NULL;
}

static void render_trace(EdBuf *b, const int idx, const char *reasoning, const int live, const double secs, const int open) {
    if (!reasoning || !reasoning[0]) {
        if (live) edb_add(b, "<div class='knAiTrace knAiPulse'><span class='knAiDot'></span>Thinking\xe2\x80\xa6</div>");
        return;
    }
    edb_addf(b, "<div class='knAiTrace%s' onclick='KE.kn.aiTrace(%d)'><i class='material-icons knAiChev'>%s</i>", live ? " knAiPulse" : "", idx,
             open ? "expand_more" : "chevron_right");
    if (live) edb_add(b, "<span class='knAiDot'></span>Thinking\xe2\x80\xa6");
    else if (secs > 0) edb_addf(b, "<span class='knAiTick'>&#10003;</span>Thought for %.1fs", secs);
    else edb_add(b, "<span class='knAiTick'>&#10003;</span>Thought");
    edb_add(b, "</div>");
    if (open) {
        edb_add(b, "<div class='knAiThink'>");
        ai_md_html(reasoning, b);
        edb_add(b, "</div>");
    }
}

static void add_clipped_pre(EdBuf *b, const char *text, const size_t max) {
    edb_add(b, "<pre class='knAiPre'>");
    const size_t cl = strlen(text);
    if (cl > max) {
        char *cut = malloc(max + 1);
        if (cut) {
            memcpy(cut, text, max);
            cut[max] = 0;
            ai_html_escape(b, cut);
            free(cut);
        }
        edb_addf(b, "\n[... %zu more bytes]", cl - max);
    } else {
        ai_html_escape(b, text);
    }
    edb_add(b, "</pre>");
}

static const char *tool_icon(const char *name) {
    if (strstr(name, "search")) return "search";
    if (strstr(name, "fetch")) return "public";
    if (strstr(name, "render")) return "photo_camera";
    if (strstr(name, "playtest")) return "sports_esports";
    if (strstr(name, "validate") || strstr(name, "compile")) return "rule";
    if (strstr(name, "delete")) return "delete";
    if (strstr(name, "query") || strstr(name, "get") || strstr(name, "list") || strstr(name, "info")) return "travel_explore";
    if (strstr(name, "script")) return "code";
    return "build";
}

static void render_tool_card(EdBuf *b, const int idx, const int k, const cJSON *call, const cJSON *result, const cJSON *meta) {
    const cJSON *fn = jget(call, "function");
    const char *name = ed_json_str(fn, "name", "?");
    const char *id = ed_json_str(call, "id", "");
    const cJSON *tm = jget(jget(meta, "tools"), id);
    const char *status = ed_json_str(tm, "status", result ? "done" : "pending");
    const char *content = result ? ed_json_str(result, "content", "") : ed_json_str(tm, "result", "");
    const int is_err = !strcmp(status, "error") || !strcmp(status, "denied") || !strncmp(content, "Error:", 6);
    const int open = ed_json_truthy(jget(tm, "open"));
    edb_addf(b, "<div class='knAiTool%s'><div class='knAiToolHead' onclick='KE.kn.aiTool(%d,%d)'>", is_err ? " knAiToolErr" : "", idx, k);
    edb_addf(b, "<i class='material-icons'>%s</i><span class='knAiToolName'>", tool_icon(name));
    ai_html_escape(b, name);
    edb_add(b, "</span><span class='knAiToolState'>");
    if (!strcmp(status, "running") || (!result && !strcmp(status, "pending"))) edb_add(b, "<span class='knAiDot'></span>running");
    else if (!strcmp(status, "approval")) edb_add(b, "needs approval");
    else if (!strcmp(status, "denied")) edb_add(b, "&#10007; denied");
    else if (is_err) edb_add(b, "&#10007; failed");
    else edb_add(b, "&#10003; done");
    const double ms = ed_json_num(tm, "ms", -1);
    if (ms >= 0) edb_addf(b, " \xc2\xb7 %.1fs", ms / 1000.0);
    edb_add(b, "</span></div>");
    if (!strcmp(status, "approval")) {
        edb_addf(b, "<div class='knAiApprove'><div class='knAiAllow' onclick='KE.kn.aiApprove(%d,%d,1)'>Allow</div>"
                    "<div onclick='KE.kn.aiApprove(%d,%d,2)'>Allow all this session</div>"
                    "<div onclick='KE.kn.aiApprove(%d,%d,0)'>Deny</div></div>",
                 idx, k, idx, k, idx, k);
    }
    if (open || !strcmp(status, "approval")) {
        edb_add(b, "<div class='knAiToolBody'><div class='knAiToolLbl'>arguments</div>");
        const char *args = ed_json_str(fn, "arguments", "{}");
        cJSON *pa = cJSON_Parse(args);
        char *pretty = pa ? cJSON_Print(pa) : NULL;
        add_clipped_pre(b, pretty ? pretty : args, 4000);
        free(pretty);
        cJSON_Delete(pa);
        if (content[0]) {
            edb_add(b, "<div class='knAiToolLbl'>result</div>");
            add_clipped_pre(b, content, 6000);
        }
        edb_add(b, "</div>");
    }
    const cJSON *img;
    cJSON_ArrayForEach(img, jget(tm, "images")) {
        if (cJSON_IsString(img)) {
            edb_add(b, "<img class='knAiShot' src='");
            ai_html_escape(b, img->valuestring);
            edb_add(b, "'>");
        }
    }
    edb_add(b, "</div>");
}

static void render_footer(EdBuf *b, const int idx, const cJSON *meta) {
    const double tin = ed_json_num(meta, "tokens_in", 0), tout = ed_json_num(meta, "tokens_out", 0);
    const double secs = ed_json_num(meta, "seconds", 0), cost = ed_json_num(meta, "cost", -1);
    char t[32], c[32];
    fmt_int(t, sizeof(t), (int64_t) (tin + tout));
    fmt_cost(c, sizeof(c), cost);
    edb_addf(b, "<div class='knAiFoot'><span>%s tokens \xc2\xb7 %.1fs%s%s</span>", t, secs, c[0] ? " \xc2\xb7 " : "", c);
    edb_addf(b, "<span class='knAiActs'><span class='knAiAct' onclick='KE.kn.aiCopy(%d)'>Copy</span>"
                "<span class='knAiAct' onclick='KE.kn.aiRetry(%d)'>Retry</span></span></div>", idx, idx);
}

// one assistant message (idx = its index in A.conv)
static void render_assistant(EdBuf *b, const int idx, const cJSON *m) {
    const cJSON *meta = jget(m, "meta");
    edb_add(b, "<div class='knAiMsg knAiBot'><div class='knAiWho'><i class='material-icons knAiSpark'>auto_awesome</i>");
    ai_html_escape(b, model_label(ed_json_str(meta, "model", A.custom ? A.ep_model : A.model)));
    edb_add(b, "</div>");
    render_trace(b, idx, ed_json_str(m, "reasoning_content", ""), 0, ed_json_num(meta, "think_s", 0), ed_json_truthy(jget(meta, "open")));
    const char *content = ed_json_str(m, "content", "");
    if (content[0]) {
        edb_add(b, "<div class='knAiBody'>");
        ai_md_html(content, b);
        edb_add(b, "</div>");
    }
    int k = 0;
    const cJSON *call;
    cJSON_ArrayForEach(call, jget(m, "tool_calls")) render_tool_card(b, idx, k++, call, tool_result(ed_json_str(call, "id", ""), idx), meta);
    const char *err = ed_json_str(meta, "error", "");
    if (err[0]) {
        edb_add(b, "<div class='knAiMsgErr'>");
        ai_html_escape(b, err);
        edb_add(b, "</div>");
    }
    if (ed_json_truthy(jget(meta, "last"))) render_footer(b, idx, meta);
    edb_add(b, "</div>");
}

static void render_all(void) {
    if (!A.list) return;
    EdBuf b;
    edb_init(&b);
    const int n = cJSON_GetArraySize(A.conv);
    if (!n && !A.streaming) {
        edb_add(&b, "<div class='knAiEmpty'><div class='knAiEmptyTitle'><i class='material-icons knAiSpark'>auto_awesome</i>Map editor AI</div>"
                    "<div class='knAiEmptyText'>Ask about Krunker mapping, or describe what to build: with Tools on, the AI reads and edits "
                    "the open map (every change is one undo step).</div></div>");
    }
    for (int i = 0; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(A.conv, i);
        const char *role = ed_json_str(m, "role", "");
        if (!strcmp(role, "user")) {
            edb_add(&b, "<div class='knAiMsg knAiUser'><div class='knAiBubble'>");
            ai_html_escape(&b, ed_json_str(m, "content", ""));
            edb_add(&b, "</div></div>");
        } else if (!strcmp(role, "assistant")) {
            render_assistant(&b, i, m);
        }
    }
    edb_add(&b, "<div id='knAiLive'></div>");
    html_dom_set_html(A.list, b.buf ? b.buf : "");
    edb_free(&b);
    A.live = html_dom_by_id("knAiLive");
    render_live();
    html_dom_scroll_to_bottom(A.list);
}

static void render_live(void) {
    if (!A.live) return;
    A.live_dirty = 0;
    A.render_at = g_ed.now;
    if (!A.streaming) {
        html_dom_set_html(A.live, "");
        return;
    }
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div class='knAiMsg knAiBot'><div class='knAiWho'><i class='material-icons knAiSpark'>auto_awesome</i>");
    ai_html_escape(&b, model_label(A.live_model));
    edb_add(&b, "</div>");
    const int thinking = !A.st.text.len && !A.st.ncalls;
    if (A.st.reasoning.len || thinking) render_trace(&b, -1, A.st.reasoning.data, thinking, 0, 0);
    if (A.st.text.len) {
        edb_add(&b, "<div class='knAiBody'>");
        ai_md_html(A.st.text.data, &b);
        edb_add(&b, "</div>");
    }
    const cJSON *ev;
    cJSON_ArrayForEach(ev, A.server_tools) {
        const char *st = ed_json_str(ev, "status", "");
        edb_addf(&b, "<div class='knAiTool'><div class='knAiToolHead'><i class='material-icons'>%s</i><span class='knAiToolName'>",
                 tool_icon(ed_json_str(ev, "name", "")));
        ai_html_escape(&b, ed_json_str(ev, "name", ""));
        edb_add(&b, "</span><span class='knAiToolState'>");
        if (!strcmp(st, "running")) edb_add(&b, "<span class='knAiDot'></span>running");
        else edb_addf(&b, "%s \xc2\xb7 %.1fs", !strcmp(st, "error") ? "&#10007; failed" : "&#10003; done", ed_json_num(ev, "ms", 0) / 1000.0);
        edb_add(&b, "</span></div></div>");
    }
    const cJSON *name;
    cJSON_ArrayForEach(name, A.tool_starts) {
        edb_addf(&b, "<div class='knAiTool'><div class='knAiToolHead'><i class='material-icons'>%s</i><span class='knAiToolName'>", tool_icon(name->valuestring));
        ai_html_escape(&b, name->valuestring);
        edb_add(&b, "</span><span class='knAiToolState'><span class='knAiDot'></span>preparing</span></div></div>");
    }
    edb_add(&b, "</div>");
    html_dom_set_html(A.live, b.buf);
    edb_free(&b);
    html_dom_scroll_to_bottom(A.list);
}

// ---------------------------------------------------------------------------
// the turn: request, stream, result

static void stream_delta(void *user, const int kind, const char *text, const size_t len) {
    (void) user; (void) text; (void) len;
    if (kind == AI_DELTA_TEXT && A.think_end <= 0) A.think_end = g_ed.now;
    if (kind == AI_DELTA_TOOL && A.custom && text && len) { // own endpoint: no kn.tool_start, the parser's tool names
        char nm[128];
        snprintf(nm, sizeof(nm), "%.*s", (int) len, text);
        cJSON_AddItemToArray(A.tool_starts, cJSON_CreateString(nm));
    }
    A.live_dirty = 1;
}

static void stream_event(void *user, const char *event, const cJSON *data) {
    (void) user;
    if (!strcmp(event, "kn.done")) {
        cJSON_Delete(A.done);
        A.done = cJSON_Duplicate(data, 1);
    } else if (!strcmp(event, "kn.meta")) {
        const cJSON *h = jget(data, "health");
        if (h) {
            snprintf(A.health, sizeof(A.health), "%s", ed_json_str(h, "state", ""));
            snprintf(A.health_reason, sizeof(A.health_reason), "%s", ed_json_str(h, "reason", ""));
            health_banner();
        }
    } else if (!strcmp(event, "kn.tool_start")) {
        if (!ed_json_truthy(jget(data, "server"))) cJSON_AddItemToArray(A.tool_starts, cJSON_CreateString(ed_json_str(data, "name", "tool")));
        A.live_dirty = 1;
    } else if (!strcmp(event, "kn.server_tool")) {
        const char *id = ed_json_str(data, "id", "");
        cJSON_DeleteItemFromObjectCaseSensitive(A.server_tools, id);
        cJSON_AddItemToObject(A.server_tools, id, cJSON_Duplicate(data, 1));
        A.live_dirty = 1;
    } else if (!strcmp(event, "kn.round")) {
        // a new round after server tools: the text keeps growing in the same live message
        cJSON_Delete(A.tool_starts);
        A.tool_starts = cJSON_CreateArray();
        A.live_dirty = 1;
    }
}

static void hs_open(void *user, const int status, const char *ctype) {
    (void) user;
    A.http_status = status;
    A.sse = strstr(ctype, "event-stream") != NULL;
}

static void hs_data(void *user, const char *data, const size_t len) {
    (void) user;
    if (A.http_status == 200 && A.sse) ai_stream_feed(&A.st, data, len);
    else if (A.errbody.len < 65536 || (A.http_status == 200 && A.errbody.len < (8u << 20))) ai_buf_add(&A.errbody, data, len);
}

// the finished turn's messages into the conversation
static void turn_commit(const char *error) {
    const double secs = g_ed.now - A.started;
    const double think_s = A.think_end > 0 ? A.think_end - A.started : (A.st.reasoning.len ? secs : 0);
    cJSON *added = NULL;
    const cJSON *dm = jget(A.done, "messages");
    if (cJSON_IsArray(dm) && cJSON_GetArraySize(dm) > 0) added = cJSON_Duplicate(dm, 1);
    else if (A.st.text.len || A.st.reasoning.len || A.st.ncalls) {
        added = cJSON_CreateArray();
        cJSON_AddItemToArray(added, ai_stream_message(&A.st));
    }
    // the footer belongs to the newest reply
    const cJSON *m;
    cJSON_ArrayForEach(m, A.conv) {
        if (!strcmp(ed_json_str(m, "role", ""), "assistant")) cJSON_DeleteItemFromObjectCaseSensitive(meta_of((cJSON *) m), "last");
    }
    cJSON *last_assistant = NULL;
    cJSON *it;
    while (added && (it = cJSON_DetachItemFromArray(added, 0))) {
        if (!strcmp(ed_json_str(it, "role", ""), "assistant")) last_assistant = it;
        // brain-run tool results (web search): their card shows the result, they are not drawn alone
        cJSON_AddItemToArray(A.conv, it);
    }
    cJSON_Delete(added);
    if (!last_assistant && error) {
        last_assistant = cJSON_CreateObject();
        cJSON_AddStringToObject(last_assistant, "role", "assistant");
        cJSON_AddStringToObject(last_assistant, "content", "");
        cJSON_AddItemToObject(meta_of(last_assistant), "failed", cJSON_CreateTrue()); // never sent upstream
        cJSON_AddItemToArray(A.conv, last_assistant);
    }
    // server tool timings into the cards
    const cJSON *ev;
    const int n = cJSON_GetArraySize(A.conv);
    cJSON_ArrayForEach(ev, A.server_tools) {
        const char *id = ed_json_str(ev, "id", "");
        for (int i = n - 1; i >= 0; i--) {
            cJSON *am = cJSON_GetArrayItem(A.conv, i);
            const cJSON *call;
            int hit = 0;
            cJSON_ArrayForEach(call, jget(am, "tool_calls")) hit |= !strcmp(ed_json_str(call, "id", ""), id);
            if (!hit) continue;
            cJSON *tm = tool_meta(am, id);
            ed_json_set_num(tm, "ms", ed_json_num(ev, "ms", 0));
            ed_json_set_str(tm, "status", ed_json_str(ev, "status", "done"));
            break;
        }
    }
    if (last_assistant) {
        cJSON *meta = meta_of(last_assistant);
        const cJSON *u = jget(A.done, "usage");
        const double tin = u ? ed_json_num(u, "prompt_tokens", 0) : (double) A.st.prompt_tokens;
        const double tout = u ? ed_json_num(u, "completion_tokens", 0) : (double) A.st.completion_tokens;
        ed_json_set_str(meta, "model", A.live_model);
        ed_json_set_num(meta, "seconds", secs);
        if (think_s > 0) ed_json_set_num(meta, "think_s", think_s);
        ed_json_set_num(meta, "tokens_in", tin);
        ed_json_set_num(meta, "tokens_out", tout);
        double cost = ed_json_num(A.done, "cost_usd", -1);
        const cJSON *row = model_row(A.live_model);
        if (cost < 0 && row && ed_json_num(row, "price_in", -1) >= 0)
            cost = (ed_json_num(row, "price_in", 0) * tin + ed_json_num(row, "price_out", 0) * tout) / 1e6;
        ed_json_set_num(meta, "cost", cost);
        ed_json_set_bool(meta, "last", 1);
        if (error) ed_json_set_str(meta, "error", error);
        A.sess_in += (int64_t) tin;
        A.sess_out += (int64_t) tout;
        if (cost > 0) A.sess_cost += cost;
    }
    const cJSON *acct = jget(A.done, "account");
    if (acct) {
        A.req_today = (int) ed_json_num(acct, "requests_today", A.req_today);
        A.tok_today = (int64_t) ed_json_num(acct, "tokens_today", (double) A.tok_today);
    }
}

static void turn_reset(void) {
    ai_stream_free(&A.st);
    ai_buf_free(&A.errbody);
    cJSON_Delete(A.done);
    A.done = NULL;
    cJSON_Delete(A.tool_starts);
    A.tool_starts = NULL;
    cJSON_Delete(A.server_tools);
    A.server_tools = NULL;
    A.hs = NULL;
    A.streaming = 0;
}

static void error_from_body(char *out, const size_t n, int *signin) {
    *signin = 0;
    cJSON *o = A.errbody.len ? cJSON_ParseWithLength(A.errbody.data, A.errbody.len) : NULL;
    const char *code = ed_json_str(o, "error", "");
    if (!A.custom && (A.http_status == 401 || !strcmp(code, "SignInRequired"))) *signin = 1;
    if (o && ed_json_str(o, "message", NULL)) snprintf(out, n, "%s", ed_json_str(o, "message", ""));
    else {
        char e[240];
        ai_error_text(A.errbody.data, A.errbody.len, e, sizeof(e));
        snprintf(out, n, "HTTP %d%s%s", A.http_status, e[0] ? ": " : "", e);
    }
    cJSON_Delete(o);
}

static int conv_index(const cJSON *m) {
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, A.conv) {
        if (it == m) return i;
        i++;
    }
    return -1;
}

// the newest assistant message whose editor tool calls still lack results; NULL = none
static cJSON *pending_tool_message(void) {
    const int n = cJSON_GetArraySize(A.conv);
    for (int i = n - 1; i >= 0; i--) {
        cJSON *m = cJSON_GetArrayItem(A.conv, i);
        if (strcmp(ed_json_str(m, "role", ""), "assistant")) continue;
        const cJSON *call;
        cJSON_ArrayForEach(call, jget(m, "tool_calls")) {
            if (!tool_result(ed_json_str(call, "id", ""), i)) return m;
        }
        return NULL; // only the newest assistant message counts
    }
    return NULL;
}

static void hs_done(void *user, const int status, const char *error) {
    (void) user;
    char err[400] = "";
    int signin = 0;
    if (!status) snprintf(err, sizeof(err), "Cannot reach the AI service (%s)", error ? error : "network error");
    else if (A.http_status != 200) error_from_body(err, sizeof(err), &signin);
    else if (!A.sse) { // a whole JSON answer (an endpoint that ignores stream:true)
        if (!ai_parse_response(&A.st, A.errbody.data ? A.errbody.data : "", A.errbody.len)) snprintf(err, sizeof(err), "The answer is not readable");
        else if (A.st.error[0]) snprintf(err, sizeof(err), "%s", A.st.error);
    } else {
        ai_stream_finish(&A.st);
        if (A.st.error[0]) snprintf(err, sizeof(err), "%s", A.st.error);
        else if (!A.st.done && !A.done && !A.st.text.len && !A.st.ncalls) snprintf(err, sizeof(err), "The answer ended early");
    }
    const int model_down = !A.custom && A.done && !strcmp(ed_json_str(A.done, "error", ""), "ModelUnavailable");
    turn_commit(err[0] ? err : NULL);
    turn_reset();
    if (model_down) { // the picker learns it (status), the banner offers the service's default
        A.models_state = 0;
        models_load();
    }
    if (signin) set_banner(4, "Your session expired: sign in again in the Krunker Hub.");
    else if (err[0] && A.http_status == 429) set_banner(2, err);
    if (!err[0] && pending_tool_message()) {
        A.tool_step = 1; // the editor's tools run from the next ticks, one per frame
    } else {
        history_save();
    }
    render_all();
    update_controls();
}

static cJSON *system_message(void) {
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "You are the AI assistant of the Krunker Box Office map editor (Krunker v7.1.4 maps). Answer briefly and concretely; "
                "use markdown. Coordinates are Krunker units with y up; an object's p is its bottom centre, s its size, rotations are radians.\n");
    if (g_ed.proj) {
        const char *name = ed_json_str(g_ed.proj->map_config, "name", "");
        edb_addf(&b, "Open map: \"%s\", %zu objects (%d collidable, %d spawn points).\n", name[0] ? name : "untitled", g_ed.proj->count,
                 g_ed.proj->obj_count[1], g_ed.proj->obj_count[2]);
    }
    if (A.tools_on && ai_tools_available() && model_has_tools())
        edb_add(&b, "You can inspect and change the map open in the editor with the editor tools (they act on the shown map unless you "
                    "pass project). Look before you edit (map_info, objects_query, prefabs); batch edits in one call; every edit is one "
                    "undo step the user can revert; render_view / render_topdown let you see the result, map_validate and playtest "
                    "check it. Destructive tools (deletes, saves, scripts) wait for the user's approval. Say what you changed.\n");
    if (A.web_on && web_available() && model_has_tools())
        edb_add(&b, "web_search and web_fetch look things up on the web.\n");
    const AccountInfo *acc = account_get();
    if (acc) edb_addf(&b, "The user is signed in as %s.\n", acc->name);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", b.buf ? b.buf : "");
    edb_free(&b);
    return m;
}

// The request's messages: the system prompt + the conversation (failed placeholders and UI state
// left out, oldest turns dropped beyond the size budget).
static cJSON *request_messages(void) {
    cJSON *out = cJSON_CreateArray();
    cJSON_AddItemToArray(out, system_message());
    const int n = cJSON_GetArraySize(A.conv);
    int start = 0;
    size_t total = 0;
    for (int i = n - 1; i >= 0; i--) {
        char *t = cJSON_PrintUnformatted(cJSON_GetArrayItem(A.conv, i));
        total += t ? strlen(t) : 0;
        free(t);
        if (total > AI_MAX_CONV_BYTES) {
            start = i + 1;
            break;
        }
    }
    // never start on a tool result or an assistant turn: from the next user message
    while (start > 0 && start < n && strcmp(ed_json_str(cJSON_GetArrayItem(A.conv, start), "role", ""), "user")) start++;
    for (int i = start; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(A.conv, i);
        if (ed_json_truthy(jget(jget(m, "meta"), "failed"))) continue;
        cJSON *c = cJSON_Duplicate(m, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(c, "meta");              // UI state
        cJSON_DeleteItemFromObjectCaseSensitive(c, "reasoning_content"); // never sent back upstream
        cJSON_AddItemToArray(out, c);
    }
    return out;
}

static void send_turn(void) {
    if (A.streaming) return;
    if (!signed_in()) {
        set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
        update_controls();
        return;
    }
    if (!A.custom && !brain_ready()) {
        set_banner(3, "AI needs the account service: this editor runs offline.");
        return;
    }
    if (A.custom && !A.ep_url[0]) {
        set_banner(3, "Set your endpoint's URL in the settings first.");
        return;
    }
    turn_reset();
    ai_stream_init(&A.st, stream_delta, NULL);
    A.st.on_event = stream_event;
    A.tool_starts = cJSON_CreateArray();
    A.server_tools = cJSON_CreateObject();
    const char *model = A.custom ? A.ep_model : A.model;
    snprintf(A.live_model, sizeof(A.live_model), "%s", model);
    cJSON *messages = request_messages();
    const int tools_ok = model_has_tools();
    cJSON *tools = A.tools_on && tools_ok ? ai_tools_defs() : NULL;
    char *json = NULL, *url = NULL;
    char headers[700] = "";
    if (A.custom) {
        json = ai_request_body(AI_API_CHAT, model, messages, tools, A.effort, 1, 0);
        url = ep_url("/chat/completions");
        if (A.ep_key[0]) snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", A.ep_key);
    } else {
        cJSON *body = cJSON_CreateObject();
        if (model[0]) cJSON_AddStringToObject(body, "model", model);
        cJSON_AddItemToObject(body, "messages", messages);
        messages = NULL;
        if (tools) {
            cJSON_AddItemToObject(body, "tools", tools);
            tools = NULL;
        }
        if (A.effort[0]) cJSON_AddStringToObject(body, "effort", A.effort);
        if (A.web_on && web_available() && tools_ok) cJSON_AddBoolToObject(body, "web_search", 1);
        cJSON_AddBoolToObject(body, "stream", 1);
        json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        url = brain_url("/ai/chat");
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", account_session_token());
    }
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    static const HttpStreamCallbacks cb = {hs_open, hs_data, hs_done};
    A.http_status = 0;
    A.sse = 0;
    A.started = g_ed.now;
    A.think_end = 0;
    A.hs = json && url ? http_stream_start("POST", url, headers[0] ? headers : NULL, json, &cb, NULL) : NULL;
    memset(headers, 0, sizeof(headers));
    free(json);
    free(url);
    if (!A.hs) {
        set_banner(3, "Cannot start the request (an https:// URL in a build without TLS?)");
        return;
    }
    A.streaming = 1;
    if (A.banner_kind == 3) set_banner(0, NULL);
    render_all();
    update_controls();
}

static void tool_answer(cJSON *m, const char *id, const char *text);

// the newest reply's unanswered tool calls are answered "not run" (stop, a new message)
static void skip_pending_tools(const char *why) {
    cJSON *m = pending_tool_message();
    if (!m) return;
    const cJSON *calls = jget(m, "tool_calls");
    const int at = conv_index(m);
    const cJSON *call;
    cJSON_ArrayForEach(call, calls) {
        const char *id = ed_json_str(call, "id", "");
        if (tool_result(id, at)) continue;
        tool_answer(m, id, why);
        ed_json_set_str(tool_meta(m, id), "status", "denied");
    }
    A.tool_step = 0;
}

static void stop_turn(void) {
    if (A.tool_step || pending_tool_message()) { // tools waiting: the rest are not run
        skip_pending_tools("Error: stopped by the user before it ran");
        history_save();
        render_all();
        update_controls();
        return;
    }
    if (!A.streaming) return;
    http_stream_cancel(A.hs);
    A.hs = NULL;
    ai_stream_finish(&A.st);
    turn_commit("Stopped");
    turn_reset();
    history_save();
    render_all();
    update_controls();
}

static void skip_pending_tools(const char *why);

static void submit_text(const char *text) {
    if (!text || !text[0] || A.streaming || A.tool_step) return;
    skip_pending_tools("Error: not run: the user sent a new message instead"); // e.g. an approval left open
    if (!signed_in()) {
        set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
        update_controls();
        return;
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(A.conv, m);
    A.rounds = 0;
    send_turn();
    if (!A.streaming) render_all();
}

// ---------------------------------------------------------------------------
// the editor's tools: one call per tick (the card shows "running" first), approvals for the
// destructive ones, then the next request with the results

static void tool_answer(cJSON *m, const char *id, const char *text) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "role", "tool");
    cJSON_AddStringToObject(r, "tool_call_id", id);
    cJSON_AddStringToObject(r, "content", text);
    // keep the results in call order: right after the call's message and the results before it
    const int n = cJSON_GetArraySize(A.conv);
    int at = n;
    for (int i = 0; i < n; i++) {
        if (cJSON_GetArrayItem(A.conv, i) == m) {
            at = i + 1;
            while (at < n && !strcmp(ed_json_str(cJSON_GetArrayItem(A.conv, at), "role", ""), "tool")) at++;
            break;
        }
    }
    if (at >= n) cJSON_AddItemToArray(A.conv, r);
    else cJSON_InsertItemInArray(A.conv, at, r);
}

static void tool_run_one(cJSON *m, const cJSON *call) {
    const cJSON *fn = jget(call, "function");
    const char *id = ed_json_str(call, "id", "");
    const char *name = ed_json_str(fn, "name", "");
    cJSON *tm = tool_meta(m, id);
    const double t0 = g_ed.now;
    const clock_t c0 = clock();
    if (!ai_tool_known(name)) {
        char e[200];
        snprintf(e, sizeof(e), "Error: unknown tool %s%s", name, ai_tools_available() ? "" : " (this build has no editor tools)");
        tool_answer(m, id, e);
        ed_json_set_str(tm, "status", "error");
        return;
    }
    cJSON *res = ai_tool_run(name, ed_json_str(fn, "arguments", "{}"));
    const int is_err = ed_json_truthy(jget(res, "is_error"));
    const char *text = ed_json_str(res, "text", "");
    if (is_err) {
        char *e = malloc(strlen(text) + 8);
        if (e) {
            sprintf(e, "Error: %s", text);
            tool_answer(m, id, e);
            free(e);
        }
    } else {
        tool_answer(m, id, text);
    }
    ed_json_set_str(tm, "status", is_err ? "error" : "done");
    double ms = (g_ed.now - t0) * 1000.0;
    if (ms <= 0) ms = (double) (clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    ed_json_set_num(tm, "ms", ms);
    const cJSON *imgs = jget(res, "images");
    if (cJSON_GetArraySize(imgs)) cJSON_AddItemToObject(tm, "images", cJSON_Duplicate(imgs, 1)); // shown, not stored in the history
    cJSON_Delete(res);
}

// one step: the next pending call runs, waits for approval, or (all answered) the next request goes out
static void process_tools(void) {
    cJSON *m = pending_tool_message();
    if (!m) {
        A.tool_step = 0;
        if (A.rounds >= AI_MAX_TOOL_ROUNDS) {
            set_banner(2, "Stopped after 25 tool rounds: ask again to continue.");
            history_save();
            render_all();
            update_controls();
            return;
        }
        A.rounds++;
        send_turn();
        return;
    }
    const int from = conv_index(m);
    const cJSON *call;
    cJSON_ArrayForEach(call, jget(m, "tool_calls")) {
        const char *id = ed_json_str(call, "id", "");
        if (tool_result(id, from)) continue;
        const char *name = ed_json_str(jget(call, "function"), "name", "");
        cJSON *tm = tool_meta(m, id);
        const char *status = ed_json_str(tm, "status", "");
        if (!strcmp(status, "approval")) {
            A.tool_step = 0; // waits for KE.kn.aiApprove
            return;
        }
        if (ai_tool_needs_approval(name) && !A.allow_all && !ed_json_truthy(jget(tm, "approved"))) {
            ed_json_set_str(tm, "status", "approval");
            A.tool_step = 0;
            render_all();
            update_controls();
            return;
        }
        if (strcmp(status, "running")) { // first pass: show it running, run it on the next tick
            ed_json_set_str(tm, "status", "running");
            render_all();
            return;
        }
        tool_run_one(m, call);
        render_all();
        return;
    }
}

static void approve(const int idx, const int k, const int choice) {
    cJSON *m = cJSON_GetArrayItem(A.conv, idx);
    const cJSON *call = cJSON_GetArrayItem(jget(m, "tool_calls"), k);
    if (!m || !call) return;
    const char *id = ed_json_str(call, "id", "");
    cJSON *tm = tool_meta(m, id);
    if (strcmp(ed_json_str(tm, "status", ""), "approval")) return;
    if (choice == 0) {
        tool_answer(m, id, "Error: the user denied this tool call");
        ed_json_set_str(tm, "status", "denied");
    } else {
        if (choice == 2) A.allow_all = 1;
        ed_json_set_bool(tm, "approved", 1);
        ed_json_set_str(tm, "status", "pending");
    }
    A.tool_step = 1;
    render_all();
    update_controls();
}

// ---------------------------------------------------------------------------
// history (storage knAiChats: newest first [{id, title, model, updated, conv}])

static cJSON *history_load(void) {
    char *t = storage_text("knAiChats");
    cJSON *h = t ? cJSON_Parse(t) : NULL;
    free(t);
    if (!cJSON_IsArray(h)) {
        cJSON_Delete(h);
        h = cJSON_CreateArray();
    }
    return h;
}

static void history_store(cJSON *h) {
    // size budget: drop the oldest chats
    char *t = cJSON_PrintUnformatted(h);
    while (t && (strlen(t) > AI_HISTORY_BYTES || cJSON_GetArraySize(h) > AI_HISTORY_MAX) && cJSON_GetArraySize(h) > 1) {
        free(t);
        cJSON_DeleteItemFromArray(h, cJSON_GetArraySize(h) - 1);
        t = cJSON_PrintUnformatted(h);
    }
    if (t) html_storage_set("knAiChats", t);
    free(t);
}

static void history_save(void) {
    if (!cJSON_GetArraySize(A.conv)) return;
    if (!A.chat_id[0]) snprintf(A.chat_id, sizeof(A.chat_id), "c%lld%03d", (long long) time(NULL), rand() % 1000);
    cJSON *h = history_load();
    for (int i = cJSON_GetArraySize(h) - 1; i >= 0; i--) {
        if (!strcmp(ed_json_str(cJSON_GetArrayItem(h, i), "id", ""), A.chat_id)) cJSON_DeleteItemFromArray(h, i);
    }
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "id", A.chat_id);
    char title[64] = "New chat";
    const cJSON *m;
    cJSON_ArrayForEach(m, A.conv) {
        if (!strcmp(ed_json_str(m, "role", ""), "user")) {
            const char *u = ed_json_str(m, "content", "");
            size_t k = 0;
            for (; u[k] && k < 42; k++) title[k] = u[k] == '\n' ? ' ' : u[k];
            while (k && ((unsigned char) title[k - 1] & 0xC0) == 0x80) k--; // whole UTF-8 characters
            if (k && ((unsigned char) title[k - 1] & 0x80)) k--;
            snprintf(title + k, sizeof(title) - k, "%s", u[k] ? "\xe2\x80\xa6" : "");
            break;
        }
    }
    cJSON_AddStringToObject(c, "title", title);
    cJSON_AddStringToObject(c, "model", A.custom ? A.ep_model : A.model);
    cJSON_AddNumberToObject(c, "updated", (double) time(NULL));
    cJSON *conv = cJSON_Duplicate(A.conv, 1);
    cJSON_ArrayForEach(m, conv) { // renders stay out of the stored history
        cJSON *tools = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "meta"), "tools");
        cJSON *tm;
        cJSON_ArrayForEach(tm, tools) cJSON_DeleteItemFromObjectCaseSensitive(tm, "images");
    }
    cJSON_AddItemToObject(c, "conv", conv);
    cJSON_InsertItemInArray(h, 0, c);
    history_store(h);
    cJSON_Delete(h);
}

static void history_open(const char *id) {
    cJSON *h = history_load();
    const cJSON *c;
    cJSON_ArrayForEach(c, h) {
        if (strcmp(ed_json_str(c, "id", ""), id)) continue;
        cJSON_Delete(A.conv);
        A.conv = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(c, "conv"), 1);
        if (!cJSON_IsArray(A.conv)) {
            cJSON_Delete(A.conv);
            A.conv = cJSON_CreateArray();
        }
        snprintf(A.chat_id, sizeof(A.chat_id), "%s", id);
        A.sess_in = A.sess_out = 0;
        A.sess_cost = 0;
        break;
    }
    cJSON_Delete(h);
}

static void history_delete(const char *id) {
    cJSON *h = history_load();
    for (int i = cJSON_GetArraySize(h) - 1; i >= 0; i--) {
        if (!strcmp(ed_json_str(cJSON_GetArrayItem(h, i), "id", ""), id)) cJSON_DeleteItemFromArray(h, i);
    }
    history_store(h);
    cJSON_Delete(h);
    if (!strcmp(A.chat_id, id)) A.chat_id[0] = 0;
}

// ---------------------------------------------------------------------------
// popovers: history (1), settings (2)

static void render_pop(void) {
    if (!A.pop_el) return;
    if (!A.pop) {
        html_dom_set_display(A.pop_el, "none");
        html_dom_set_html(A.pop_el, "");
        return;
    }
    EdBuf b;
    edb_init(&b);
    if (A.pop == 1) {
        edb_add(&b, "<div class='knAiPopTitle'>Chats on this computer</div>");
        cJSON *h = history_load();
        if (!cJSON_GetArraySize(h)) edb_add(&b, "<div class='knAiNote'>No saved chats yet.</div>");
        const cJSON *c;
        cJSON_ArrayForEach(c, h) {
            const char *id = ed_json_str(c, "id", "");
            edb_add(&b, "<div class='knAiRow' onclick=\"KE.kn.aiHist('");
            ai_html_escape(&b, id);
            edb_add(&b, "')\"><i class='material-icons' style='font-size:15px;margin-right:6px;color:#7e7e7e'>chat_bubble_outline</i><span class='knAiRowTitle'>");
            ai_html_escape(&b, ed_json_str(c, "title", "Chat"));
            edb_add(&b, "</span><i class='material-icons knAiRowDel' title='Delete' onclick=\"KE.kn.aiHistDel('");
            ai_html_escape(&b, id);
            edb_add(&b, "')\">delete_outline</i></div>");
        }
        cJSON_Delete(h);
    } else {
        edb_add(&b, "<div class='knAiPopTitle'>AI settings</div>");
        edb_addf(&b, "<div class='knAiField'><label>Provider</label><select id='knAiSrc' onchange='KE.kn.aiSrc(this.value)'>"
                     "<option value='0'%s>Krunker Box Office AI (free, signed-in accounts)</option>"
                     "<option value='1'%s>Your own OpenAI-compatible endpoint</option></select></div>",
                 A.custom ? "" : " selected", A.custom ? " selected" : "");
        if (A.custom) {
            edb_add(&b, "<div class='knAiNote knAiWarnNote'>Your own endpoint: the URL and key are stored on this computer only and the key is "
                        "sent to that URL only, never to Krunker Box Office. The chat and the map data the tools read go to that provider, "
                        "which bills your key. In the browser the endpoint must allow cross-origin requests (CORS).</div>");
            edb_add(&b, "<div class='knAiField'><label>Base URL (.../v1)</label><input id='knAiEpUrl' type='text' spellcheck='false' value='");
            ai_html_escape(&b, A.ep_url);
            edb_add(&b, "' placeholder='https://api.example.com/v1'></div><div class='knAiField'><label>API key</label><input id='knAiEpKey' type='password' value='");
            ai_html_escape(&b, A.ep_key);
            edb_add(&b, "' placeholder='sk-...'></div><div class='knAiField'><label>Model</label><input id='knAiEpModel' type='text' spellcheck='false' value='");
            ai_html_escape(&b, A.ep_model);
            edb_add(&b, "' placeholder='model id (Load lists them)'></div>");
        }
        edb_addf(&b, "<div class='knAiField'><label>Reasoning effort (thinking models)</label><select id='knAiEffort' onchange='KE.kn.aiEffort(this.value)'>"
                     "<option value=''%s>Model default</option><option value='low'%s>Low</option><option value='medium'%s>Medium</option>"
                     "<option value='high'%s>High</option></select></div>",
                 !A.effort[0] ? " selected" : "", !strcmp(A.effort, "low") ? " selected" : "", !strcmp(A.effort, "medium") ? " selected" : "",
                 !strcmp(A.effort, "high") ? " selected" : "");
        edb_addf(&b, "<div class='knAiNote'>Editor tools: %s. Destructive tools ask first%s.</div>",
                 ai_tools_available() ? "the AI can read and edit the open map" : ai_tools_unavailable_reason(),
                 A.allow_all ? " (allowed for this session: <span class='knAiLink' onclick='KE.kn.aiAsk()'>ask again</span>)" : "");
        edb_add(&b, "<div class='knAiBtnRow'>");
        if (A.custom) edb_add(&b, "<div class='knAiBtn' onclick='KE.kn.aiEpSave(1)'>Load models</div>");
        edb_add(&b, "<div class='knAiBtn knAiPrimary' onclick='KE.kn.aiEpSave(0)'>Done</div></div>");
    }
    html_dom_set_html(A.pop_el, b.buf ? b.buf : "");
    html_dom_set_display(A.pop_el, "");
    edb_free(&b);
}

static void read_input(const char *id, char *out, const size_t n) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_get_value(e, out, n);
}

static void set_source(const int custom) {
    if (A.custom == custom) return;
    if (A.streaming) stop_turn();
    A.custom = custom;
    endpoint_save();
    cJSON_Delete(A.models);
    A.models = NULL;
    A.models_state = 0;
    if (A.banner_kind == 2) set_banner(0, NULL);
    models_load();
    fill_models();
    health_banner();
    update_controls();
}

// ---------------------------------------------------------------------------
// public

void ed_ai_toggle(void) {
    build();
    if (!A.built) return;
    A.open = !A.open;
    html_dom_set_display(A.root, A.open ? "" : "none");
    if (!A.open) return;
    if (A.models_state != 1 && (A.models_state != 2 || g_ed.now - A.models_at > 300)) models_load();
    fill_models();
    if (!signed_in()) set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
    else if (!A.custom && !brain_ready()) set_banner(3, "AI needs the account service: this editor runs offline.");
    else if (A.banner_kind == 4) set_banner(0, NULL);
    health_banner();
    render_all();
    update_controls();
    html_dom_focus(A.input);
}

void ed_ai_tick(void) {
    http_stream_poll();
    if (!A.built) return;
    const AccountInfo *acc = account_get();
    if ((acc ? acc->id : 0) != A.signed_in_seen) {
        if (acc && A.banner_kind == 4) set_banner(0, NULL);
        if (!acc && A.open) set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
        if (A.open) health_banner();
        update_controls();
    }
    if (A.tool_step && !A.streaming) process_tools();
    if (A.streaming && A.live_dirty && g_ed.now - A.render_at >= AI_RENDER_EVERY_S) render_live();
}

void ed_ai_reset(void) {
    if (A.hs) http_stream_cancel(A.hs);
    if (A.ep_models_hs) http_stream_cancel(A.ep_models_hs);
    ai_buf_free(&A.ep_models_body);
    turn_reset();
    cJSON_Delete(A.conv);
    cJSON_Delete(A.models);
    cJSON_Delete(A.server_tool_names);
    memset(A.ep_key, 0, sizeof(A.ep_key));
    memset(&A, 0, sizeof(A));
}

int ed_ai_keydown(const HtmlEvent *ev) {
    if (A.pop && ev && html_key_is_escape(ev->key)) {
        A.pop = 0;
        render_pop();
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// handlers

#define H(name) static int name(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n)
#define UNUSED (void) u; (void) ev; (void) a; (void) n

static const char *arg_str(const HtmlArg *a, const int n, const int i) { return n > i && a[i].kind == HTML_ARG_STR && a[i].str ? a[i].str : ""; }
static double arg_num(const HtmlArg *a, const int n, const int i) { return n > i ? a[i].num : 0; }

H(h_toggle) {
    UNUSED;
    ed_ai_toggle();
    return 1;
}

H(h_send) {
    UNUSED;
    if (!A.input) return 1;
    const int len = html_dom_get_value(A.input, NULL, 0);
    char *text = len > 0 ? malloc((size_t) len + 1) : NULL;
    if (!text) return 1;
    html_dom_get_value(A.input, text, (size_t) len + 1);
    char *s = text;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == '\t')) s[--l] = 0;
    if (s[0] && !A.streaming && !A.tool_step && signed_in()) {
        html_dom_set_value(A.input, "");
        submit_text(s);
    } else if (!signed_in()) {
        set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
    }
    free(text);
    return 1;
}

H(h_key) {
    (void) u; (void) a; (void) n;
    if (ev && html_key_is_enter(ev->key) && !(ev->mods & 1)) { // Enter sends, Shift+Enter is a new line
        h_send(NULL, ev, NULL, 0);
        return 0;
    }
    if (ev && html_key_is_escape(ev->key)) {
        html_dom_blur(A.input);
        return 0;
    }
    return 1;
}

H(h_stop) {
    UNUSED;
    stop_turn();
    return 1;
}

H(h_model) {
    (void) u; (void) a; (void) n;
    char buf[96] = "";
    if (ev && ev->value && ev->value[0]) snprintf(buf, sizeof(buf), "%s", ev->value);
    else if (A.model_sel) html_dom_get_value(A.model_sel, buf, sizeof(buf));
    if (!buf[0]) return 1;
    if (A.custom) {
        snprintf(A.ep_model, sizeof(A.ep_model), "%s", buf);
        endpoint_save();
    } else {
        snprintf(A.model, sizeof(A.model), "%s", buf);
        settings_save();
        if (!model_warning()) health_banner();
    }
    update_controls();
    return 1;
}

H(h_use) {
    (void) u; (void) ev;
    const char *id = arg_str(a, n, 0);
    if (!id[0] || A.custom) return 1;
    snprintf(A.model, sizeof(A.model), "%s", id);
    settings_save();
    fill_models();
    set_banner(0, NULL);
    if (!model_warning()) health_banner();
    update_controls();
    return 1;
}

static void new_chat(void) {
    if (A.streaming || A.tool_step) stop_turn();
    cJSON_Delete(A.conv);
    A.conv = cJSON_CreateArray();
    A.chat_id[0] = 0;
    A.sess_in = A.sess_out = 0;
    A.sess_cost = 0;
    A.rounds = 0;
    if (A.banner_kind == 3) set_banner(0, NULL);
    render_all();
    update_controls();
}

H(h_new) {
    UNUSED;
    new_chat();
    return 1;
}

H(h_trace) {
    (void) u; (void) ev;
    if (n < 1) return 1;
    cJSON *m = cJSON_GetArrayItem(A.conv, (int) a[0].num);
    if (!m) return 1;
    cJSON *meta = meta_of(m);
    ed_json_set_bool(meta, "open", !ed_json_truthy(jget(meta, "open")));
    render_all();
    return 1;
}

H(h_tool) {
    (void) u; (void) ev;
    if (n < 2) return 1;
    cJSON *m = cJSON_GetArrayItem(A.conv, (int) a[0].num);
    const cJSON *call = cJSON_GetArrayItem(jget(m, "tool_calls"), (int) a[1].num);
    if (!m || !call) return 1;
    cJSON *tm = tool_meta(m, ed_json_str(call, "id", ""));
    ed_json_set_bool(tm, "open", !ed_json_truthy(jget(tm, "open")));
    render_all();
    return 1;
}

H(h_approve) {
    (void) u; (void) ev;
    approve((int) arg_num(a, n, 0), (int) arg_num(a, n, 1), (int) arg_num(a, n, 2));
    return 1;
}

H(h_copy) {
    (void) u; (void) ev;
    if (n < 1) return 1;
    const cJSON *m = cJSON_GetArrayItem(A.conv, (int) a[0].num);
    if (m) html_clipboard_write(ed_json_str(m, "content", ""));
    editor_message("Copied the answer.");
    return 1;
}

// Retry: drop everything after the user message before this reply and ask again
H(h_retry) {
    (void) u; (void) ev;
    if (n < 1 || A.streaming || A.tool_step) return 1;
    int i = (int) a[0].num;
    while (i > 0 && strcmp(ed_json_str(cJSON_GetArrayItem(A.conv, i), "role", ""), "user")) i--;
    if (i < 0 || strcmp(ed_json_str(cJSON_GetArrayItem(A.conv, i), "role", ""), "user")) return 1;
    while (cJSON_GetArraySize(A.conv) > i + 1) cJSON_DeleteItemFromArray(A.conv, i + 1);
    A.rounds = 0;
    send_turn();
    return 1;
}

H(h_signin) {
    UNUSED;
    if (!app_route_url("/social.html")) editor_message("Sign in from the Krunker Hub.");
    return 1;
}

H(h_dismiss) {
    UNUSED;
    set_banner(0, NULL);
    health_banner();
    return 1;
}

H(h_chip) {
    (void) u; (void) ev;
    if (arg_num(a, n, 0) == 0) A.tools_on = !A.tools_on;
    else A.web_on = !A.web_on;
    settings_save();
    update_controls();
    return 1;
}

H(h_pop) {
    (void) u; (void) ev;
    const int which = (int) arg_num(a, n, 0);
    A.pop = A.pop == which ? 0 : which;
    render_pop();
    return 1;
}

H(h_hist) {
    (void) u; (void) ev;
    if (A.streaming || A.tool_step) stop_turn();
    history_open(arg_str(a, n, 0));
    A.pop = 0;
    render_pop();
    render_all();
    update_controls();
    return 1;
}

H(h_hist_del) {
    (void) u; (void) ev;
    history_delete(arg_str(a, n, 0));
    html_ui_stop_propagation(); // not the row's open
    render_pop();
    return 0;
}

H(h_src) {
    (void) u; (void) a; (void) n;
    set_source(ev && ev->value && ev->value[0] == '1');
    render_pop();
    return 1;
}

H(h_effort) {
    (void) u; (void) a; (void) n;
    const char *v = ev && ev->value ? ev->value : "";
    snprintf(A.effort, sizeof(A.effort), "%s", !strcmp(v, "low") || !strcmp(v, "medium") || !strcmp(v, "high") ? v : "");
    settings_save();
    return 1;
}

// Done (0) / Load models (1): the endpoint fields into storage
H(h_ep_save) {
    (void) u; (void) ev;
    if (A.custom) {
        read_input("knAiEpUrl", A.ep_url, sizeof(A.ep_url));
        read_input("knAiEpKey", A.ep_key, sizeof(A.ep_key));
        read_input("knAiEpModel", A.ep_model, sizeof(A.ep_model));
        endpoint_save();
        if (arg_num(a, n, 0) == 1 || !A.models) {
            A.models_state = 0;
            models_load();
        }
        fill_models();
    }
    if (arg_num(a, n, 0) == 0) {
        A.pop = 0;
        render_pop();
    }
    update_controls();
    return 1;
}

H(h_ask) {
    UNUSED;
    A.allow_all = 0;
    render_pop();
    return 1;
}

#undef H
#undef UNUSED

void ed_ai_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.kn.aiToggle", h_toggle},   {"KE.kn.aiSend", h_send},       {"KE.kn.aiKey", h_key},         {"KE.kn.aiStop", h_stop},
        {"KE.kn.aiModel", h_model},     {"KE.kn.aiNew", h_new},         {"KE.kn.aiTrace", h_trace},     {"KE.kn.aiTool", h_tool},
        {"KE.kn.aiCopy", h_copy},       {"KE.kn.aiRetry", h_retry},     {"KE.kn.aiSignIn", h_signin},   {"KE.kn.aiDismiss", h_dismiss},
        {"KE.kn.aiApprove", h_approve}, {"KE.kn.aiChip", h_chip},       {"KE.kn.aiPop", h_pop},         {"KE.kn.aiHist", h_hist},
        {"KE.kn.aiHistDel", h_hist_del}, {"KE.kn.aiSrc", h_src},        {"KE.kn.aiEffort", h_effort},   {"KE.kn.aiEpSave", h_ep_save},
        {"KE.kn.aiAsk", h_ask},         {"KE.kn.aiUse", h_use},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// ---------------------------------------------------------------------------
// KRUNKNATIVE_EDITOR_SCRIPT "ai:..." (tools/editor/ai_e2e.py)
//   ai:open / ai:close, ai:model:<id>, ai:send:<text>, ai:stop, ai:new, ai:trace:<i>, ai:tool:<i>:<k>,
//   ai:approve:<i>:<k>:<0 deny|1 allow|2 allow all>, ai:tools:<0|1>, ai:web:<0|1>, ai:effort:<e>,
//   ai:endpoint:<url>|<key>|<model> (own endpoint; "ai:endpoint:" = back to kn_brain),
//   ai:history:<n> (open the nth saved chat), ai:pop:<0|1|2>, ai:dump:<path> (JSON state)

static void dump_state(const char *path) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "open", A.open);
    cJSON_AddBoolToObject(o, "signed_in", signed_in());
    cJSON_AddBoolToObject(o, "streaming", A.streaming);
    cJSON_AddBoolToObject(o, "tool_step", A.tool_step);
    cJSON_AddStringToObject(o, "model", A.custom ? A.ep_model : A.model);
    cJSON_AddBoolToObject(o, "custom", A.custom);
    cJSON_AddBoolToObject(o, "tools_on", A.tools_on);
    cJSON_AddBoolToObject(o, "tools_available", ai_tools_available());
    cJSON_AddBoolToObject(o, "web", A.web_on && web_available());
    cJSON_AddBoolToObject(o, "allow_all", A.allow_all);
    cJSON_AddStringToObject(o, "health", A.health);
    cJSON_AddNumberToObject(o, "banner_kind", A.banner_kind);
    cJSON_AddStringToObject(o, "banner", A.banner_text);
    cJSON *ids = cJSON_AddArrayToObject(o, "models");
    const cJSON *m;
    cJSON_ArrayForEach(m, A.models) cJSON_AddItemToArray(ids, cJSON_CreateString(ed_json_str(m, "id", "")));
    cJSON_AddItemToObject(o, "conv", A.conv ? cJSON_Duplicate(A.conv, 1) : cJSON_CreateArray());
    cJSON_AddItemToObject(o, "history", history_load());
    char *text = malloc(65536);
    if (text) {
        text[0] = 0;
        if (A.list) html_dom_get_text(A.list, text, 65536);
        cJSON_AddStringToObject(o, "list_text", text);
        text[0] = 0;
        if (A.pop_el && A.pop) html_dom_get_text(A.pop_el, text, 65536);
        cJSON_AddStringToObject(o, "pop_text", text);
        free(text);
    }
    char usage[512] = "";
    if (A.usage) html_dom_get_text(A.usage, usage, sizeof(usage));
    cJSON_AddStringToObject(o, "usage", usage);
    char acc[128] = "";
    if (A.account) html_dom_get_text(A.account, acc, sizeof(acc));
    cJSON_AddStringToObject(o, "account", acc);
    cJSON_AddBoolToObject(o, "shown", A.root && html_dom_is_shown(A.root));
    char *s = cJSON_Print(o);
    FILE *f = fopen(path, "w");
    if (f && s) fputs(s, f);
    if (f) fclose(f);
    free(s);
    cJSON_Delete(o);
}

int ed_ai_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "ai") || !arg) return 0;
    const char *v = strchr(arg, ':');
    const size_t kl = v ? (size_t) (v - arg) : strlen(arg);
    v = v ? v + 1 : "";
#define IS(k) (kl == strlen(k) && !strncmp(arg, k, kl))
    if (IS("open")) {
        if (!A.open) ed_ai_toggle();
    } else if (IS("close")) {
        if (A.open) ed_ai_toggle();
    } else if (IS("model")) {
        if (A.custom) snprintf(A.ep_model, sizeof(A.ep_model), "%s", v);
        else snprintf(A.model, sizeof(A.model), "%s", v);
        fill_models();
        update_controls();
    } else if (IS("send")) {
        submit_text(v);
    } else if (IS("stop")) {
        stop_turn();
    } else if (IS("new")) {
        new_chat();
    } else if (IS("trace")) {
        HtmlArg x = {HTML_ARG_NUM, atof(v), NULL};
        h_trace(NULL, NULL, &x, 1);
    } else if (IS("tool") || IS("approve")) {
        int i = 0, k = 0, c = 1;
        sscanf(v, "%d:%d:%d", &i, &k, &c);
        if (IS("tool")) {
            HtmlArg x[2] = {{HTML_ARG_NUM, i, NULL}, {HTML_ARG_NUM, k, NULL}};
            h_tool(NULL, NULL, x, 2);
        } else {
            approve(i, k, c);
        }
    } else if (IS("tools")) {
        A.tools_on = atoi(v) != 0;
        update_controls();
    } else if (IS("web")) {
        A.web_on = atoi(v) != 0;
        update_controls();
    } else if (IS("effort")) {
        snprintf(A.effort, sizeof(A.effort), "%s", v);
    } else if (IS("endpoint")) {
        if (!v[0]) set_source(0);
        else {
            char url[512] = "", key[512] = "", model[96] = "";
            const char *p1 = strchr(v, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
            snprintf(url, sizeof(url), "%.*s", (int) (p1 ? (size_t) (p1 - v) : strlen(v)), v);
            if (p1) snprintf(key, sizeof(key), "%.*s", (int) (p2 ? (size_t) (p2 - p1 - 1) : strlen(p1 + 1)), p1 + 1);
            if (p2) snprintf(model, sizeof(model), "%s", p2 + 1);
            snprintf(A.ep_url, sizeof(A.ep_url), "%s", url);
            snprintf(A.ep_key, sizeof(A.ep_key), "%s", key);
            snprintf(A.ep_model, sizeof(A.ep_model), "%s", model);
            if (A.custom) {
                endpoint_save();
                A.models_state = 0;
                models_load();
            } else {
                set_source(1);
            }
        }
    } else if (IS("history")) {
        cJSON *h = history_load();
        const cJSON *c = cJSON_GetArrayItem(h, atoi(v));
        if (c) {
            char id[32];
            snprintf(id, sizeof(id), "%s", ed_json_str(c, "id", ""));
            history_open(id);
            render_all();
            update_controls();
        }
        cJSON_Delete(h);
    } else if (IS("pop")) {
        A.pop = atoi(v);
        render_pop();
    } else if (IS("dump")) {
        dump_state(v);
    } else {
        fprintf(stderr, "[editor] ai: unknown command %s\n", arg);
    }
#undef IS
    return 1;
}
