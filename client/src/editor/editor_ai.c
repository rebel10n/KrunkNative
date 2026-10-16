// The map editor's AI panel (editor_ai.h): a compact chat docked over the 3D
// view, styled like the editor's dark panels (ui/css/editor-ai.css). The interaction model is
// abulxt's AI chat: model picker (smartest first, ai_model_rank), streamed answers rendered as
// markdown, collapsed "Thought for Ns" traces, tool calls as expandable cards, Stop / Retry / Copy,
// local history, a usage line; and VS Code's chat checkpoints: every user message can be edited and
// resent, rewound (it and everything after it leave the chat), or restored as a checkpoint (the
// editor changes made since are undone too), with a Redo until the next message.
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
#include <ctype.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <http_stream.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AI_RENDER_EVERY_S 0.066  // live message re-render while streaming (~15 Hz)
#define AI_MAX_CONV_BYTES (640u * 1024u) // older turns are dropped from a request beyond this (brain body limit 1 MiB)
#define AI_MAX_TOOL_ROUNDS 25    // automatic model <-> tool rounds after one user message (Default / Allow all pause here)
#define AI_MAX_AUTOPILOT_ROUNDS 60 // Autopilot keeps going without prompting up to this hard safety cap
#define AI_HISTORY_MAX 40        // chats kept in storage (knAiChats)
#define AI_HISTORY_BYTES (1536u * 1024u)

static struct {
    int built, open;
    HtmlElem root, list, live, input, model_sel, banner, usage, send, stop;
    HtmlElem model_pop_el, ring, perm_sel, mode_sel; // model dropdown overlay, usage ring, perm/mode pickers
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
    int perm;                // tool approvals: 0 ask (default), 1 allow all this session, 2 autopilot
    int mode;                // agent mode: 0 Agent (tools + web, acts), 1 Ask (answer only), 2 Plan (read-only, plan first)
    int rounds;              // automatic tool rounds since the user's last message
    // context-window usage ring (VS Code's chat gauge)
    int64_t ctx_used;        // tokens the conversation would send next (exact when known, else estimated)
    int ctx_est;             // 1 when ctx_used is an estimate (no exact prompt_tokens yet)
    // model dropdown (custom picker with a per-model flyout; replaces the old <select>)
    int model_pop;           // the model list is open
    int fly_left;            // the hover flyout opens to the left (panel right-docked: the side with room)
    int fly_idx;             // the model whose flyout is shown (-1 none)
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
    int model_picked;        // the user chose A.model (else the service's default is used)
    // checkpoints (see "checkpoints" below)
    char sid[24];            // this editor session: undo depths of other sessions mean nothing here
    int edit_idx;            // user message edited in place (-1 none)
    int confirm_idx;         // user message whose confirmation is shown (-1 none)
    int confirm_kind;        // 1 restore checkpoint, 2 send an edit (undo the later map edits?)
    char *edit_text;         // the edit waiting for that confirmation (owned)
    cJSON *redo_conv;        // messages a rewind / restore took out (Redo puts them back; owned)
    cJSON *redo_map;         // [{p: project id, n: steps undone, undo, redo, top}] of a restore (owned)
    int redo_kind;           // 1 rewound, 2 checkpoint restored
    int redo_steps;          // editor changes the restore undid
    char *redo_text;         // the text the rewind put into the composer (owned)
} A;

static void render_all(void);
static void render_live(void);
static void set_banner(int kind, const char *text);
static void send_turn(void);
static void update_controls(void);
static void process_tools(void);
static void history_save(void);
static void history_delete(const char *id);
static void models_load(void);
static void render_model_pop(void);
static void update_ring(void);
static void render_pop(void);
static void redo_clear(void);
static void edit_end(void);
static int g_keep_scroll; // render_all keeps the list where it is (editing an earlier message)
static void render_user(EdBuf *b, int idx, const cJSON *m, int busy);
static cJSON *cp_snapshot(void);
static int cp_changes(int idx, int *ai);
static void tool_undo_mark(int *before);
static int tool_undo_count(int before);

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

static int signed_in(void) { return active_account() != NULL && account_session_token()[0]; }

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
    A.mode = (int) ed_json_num(o, "mode", 0);
    if (A.mode < 0 || A.mode > 2) A.mode = 0;
    A.perm = (int) ed_json_num(o, "perm", 0);
    if (A.perm < 0 || A.perm > 2) A.perm = 0;
    snprintf(A.effort, sizeof(A.effort), "%s", ed_json_str(o, "effort", ""));
    // the stored model counts only when the user picked it ("picked"; patch 2 stored whatever was
    // selected, the service's fallback included): otherwise the service's default (glm-5.3-flash)
    A.model_picked = json_flag(o, "picked", 0) && ed_json_str(o, "model", "")[0];
    if (A.model_picked) snprintf(A.model, sizeof(A.model), "%s", ed_json_str(o, "model", ""));
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
    cJSON_AddNumberToObject(o, "mode", A.mode);
    cJSON_AddNumberToObject(o, "perm", A.perm);
    cJSON_AddStringToObject(o, "effort", A.effort);
    if (!A.custom && A.model_picked) {
        cJSON_AddStringToObject(o, "model", A.model);
        cJSON_AddBoolToObject(o, "picked", 1);
    }
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
        "<div id='knAiHead' class='knAiHead'>"
        "<div id='knAiTitle' class='knAiTitle'><i class='material-icons knAiSpark'>auto_awesome</i><span>AI</span></div>"
        "<div id='knAiModel' class='knAiModel' title='Model (smartest first \xe2\x80\x93 hover a model for its reasoning level)' onclick='KE.kn.aiModelMenu()'>"
        "<span id='knAiModelName' class='knAiModelName'></span><i class='material-icons knAiModelCaret'>arrow_drop_down</i></div>"
        "<div id='knAiNewBtn' class='knAiIcon material-icons' title='New chat' onclick='KE.kn.aiNew()'>add_comment</div>"
        "<div id='knAiHistBtn' class='knAiIcon material-icons' title='History' onclick='KE.kn.aiPop(1)'>history</div>"
        "<div id='knAiSetBtn' class='knAiIcon material-icons' title='Settings' onclick='KE.kn.aiPop(2)'>tune</div>"
        "<div id='knAiCloseBtn' class='knAiIcon material-icons' title='Close' onclick='KE.kn.aiToggle()'>close</div>"
        "</div>"
        "<div id='knAiPop' class='knAiPop' style='display:none'></div>"
        "<div id='knAiBanner' class='knAiBanner' style='display:none'></div>"
        "<div id='knAiList' class='knAiList'></div>"
        "<div class='knAiComposer'>"
        "<textarea id='knAiInput' class='knAiInput' rows='2' spellcheck='false' placeholder='Ask anything about this map'"
        " onkeydown='return KE.kn.aiKey()'></textarea>"
        "<div class='knAiBar knAiBar2'>"
        "<i class='material-icons knAiBarIcon' title='Agent mode'>tune</i>"
        "<select id='knAiMode' class='knAiMini' title='What the AI may do' onchange='KE.kn.aiMode(this.value)'>"
        "<option value='0'>Agent</option><option value='1'>Ask</option><option value='2'>Plan</option></select>"
        "<i class='material-icons knAiBarIcon knAiShield' title='Tool permissions'>verified_user</i>"
        "<select id='knAiPerm' class='knAiMini' title='When the AI may run tools' onchange='KE.kn.aiPerm(this.value)'>"
        "<option value='0'>Ask first</option><option value='1'>Allow all</option><option value='2'>Autopilot</option></select>"
        "<div class='knAiGrow'></div>"
        "<div id='knAiRing' class='knAiRing' title='Context window usage'></div>"
        "</div>"
        "<div class='knAiBar'>"
        "<div id='knAiTools' class='knAiBtnS knAiChip' title='Let the AI read and edit the open map' onclick='KE.kn.aiChip(0)'>Tools</div>"
        "<div id='knAiWeb' class='knAiBtnS knAiChip' title='Let the AI search the web' onclick='KE.kn.aiChip(1)'>Web</div>"
        "<div class='knAiGrow'></div>"
        "<div id='knAiSend' class='knAiBtnS knAiSend' title='Send (Enter)' onclick='KE.kn.aiSend()'>Ask</div>"
        "<div id='knAiStop' class='knAiBtnS knAiStop' style='display:none' title='Stop' onclick='KE.kn.aiStop()'><span class='knAiSq'></span>Stop</div>"
        "</div>"
        "<div id='knAiUsage' class='knAiUsage'></div>"
        "</div>"
        "</div>");
    // the model picker's dropdown lives outside the panel (the panel clips overflow; its per-model
    // flyout must reach the empty side of the screen), as a sibling overlay in #center
    html_dom_insert_html(center, HTML_INSERT_BEFORE_END,
        "<div id='knAiModelPop' class='knAiModelPop' style='display:none'></div>");
    A.root = html_dom_by_id("knAi");
    A.list = html_dom_by_id("knAiList");
    A.input = html_dom_by_id("knAiInput");
    A.model_sel = html_dom_by_id("knAiModel");
    A.model_pop_el = html_dom_by_id("knAiModelPop");
    A.ring = html_dom_by_id("knAiRing");
    A.mode_sel = html_dom_by_id("knAiMode");
    A.perm_sel = html_dom_by_id("knAiPerm");
    A.banner = html_dom_by_id("knAiBanner");
    A.usage = html_dom_by_id("knAiUsage");
    A.send = html_dom_by_id("knAiSend");
    A.stop = html_dom_by_id("knAiStop");
    A.pop_el = html_dom_by_id("knAiPop");
    A.built = A.root != 0;
    A.signed_in_seen = -1;
    A.edit_idx = A.confirm_idx = -1;
    if (!A.sid[0]) snprintf(A.sid, sizeof(A.sid), "s%lx%04x", (unsigned long) time(NULL), (unsigned) (rand() ^ (int) (clock() & 0xffff)) & 0xffff);
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
    // "degraded" (the service answered without the token, which works fine in practice) shows no
    // banner - it only nagged the user. A truly "down" service still warns.
    if (!strcmp(A.health, "down")) {
        char t[400];
        snprintf(t, sizeof(t), "AI service unavailable: %s", A.health_reason);
        set_banner(2, t);
    } else if (A.banner_kind == 2) {
        set_banner(0, NULL);
    }
}

// No account indicator in the panel (the hub shows who is signed in); the sign-in gating stays:
// a guest gets the sign-in card. This only remembers whom the panel was drawn for.
static void update_account(void) {
    const AccountInfo *acc = active_account();
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
    // mode / permission pickers reflect the state (and drive tool exposure below)
    char v[2] = {0};
    if (A.mode_sel) { v[0] = (char) ('0' + A.mode); html_dom_set_value(A.mode_sel, v); }
    if (A.perm_sel) { v[0] = (char) ('0' + A.perm); html_dom_set_value(A.perm_sel, v); }
    // Ask mode offers no tools at all; Agent / Plan do (Plan only the read-only ones, enforced in
    // ai_tools_defs' filtering). Hide the Tools / Web chips when they cannot apply.
    const int tools_apply = A.mode != 1 && ai_tools_available();
    const int web_apply = A.mode != 1 && web_available();
    HtmlElem tools = html_dom_by_id("knAiTools"), web = html_dom_by_id("knAiWeb");
    html_dom_set_display(tools, tools_apply ? "" : "none");
    html_dom_set_class(tools, "knAiOn", A.tools_on && model_has_tools());
    html_dom_set_attr(tools, "title", A.mode == 2 ? "Plan mode: the AI may read the map (read-only tools)"
                                      : !model_has_tools() ? "This model cannot use tools"
                                      : A.tools_on ? "Editor tools on: the AI can read and edit the open map" : "Editor tools off");
    html_dom_set_display(web, web_apply ? "" : "none");
    html_dom_set_class(web, "knAiOn", A.web_on && model_has_tools());
    html_dom_set_class(html_dom_by_id("knAiSetBtn"), "knAiOn", A.custom);
    update_account();
    update_usage();
    update_ring();
}

// The closed picker now shows just the selected model's name (the list is smartest first); the
// per-model detail moved into the dropdown's hover flyout (render_model_pop).
static void fill_models(void) {
    if (!A.model_sel) return;
    const char *cur = A.custom ? A.ep_model : A.model;
    HtmlElem name = html_dom_by_id("knAiModelName");
    if (name) {
        const char *lbl = cur[0] ? model_label(cur) : (A.models_state == 1 ? "Loading\xe2\x80\xa6" : "No models");
        html_dom_set_text(name, lbl);
    }
    if (A.model_pop) render_model_pop();
}

// ---------------------------------------------------------------------------
// the model dropdown: a custom picker (the panel clips overflow, so the list and its per-model
// flyout live in the #knAiModelPop overlay in #center) with the models smartest first and, on hover,
// a flyout to the side of the screen that has room showing the model's reasoning level and detail

static double model_ctx_size(void) {
    const cJSON *row = model_row(A.custom ? A.ep_model : A.model);
    return row ? ed_json_num(row, "context", 0) : 0;
}

// the model's reasoning summary ("levels low, medium, high" / "yes" / "none") into out
static void model_reasoning_text(const cJSON *row, char *out, const size_t n) {
    const cJSON *eff = cJSON_GetObjectItemCaseSensitive(row, "efforts");
    if (cJSON_IsArray(eff) && cJSON_GetArraySize(eff) > 0) { // "low / medium / high"
        size_t o = 0;
        out[0] = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, eff) {
            if (!cJSON_IsString(it)) continue;
            o += (size_t) snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? " / " : "", it->valuestring);
        }
        return;
    }
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(row, "reasoning");
    snprintf(out, n, "%s", ed_json_truthy(r) ? "yes (model default)" : "none");
}

static void model_flyout(EdBuf *b, const cJSON *row) {
    char reason[96];
    model_reasoning_text(row, reason, sizeof(reason));
    edb_add(b, "<div class='knAiFly'><div class='knAiFlyName'>");
    ai_html_escape(b, ed_json_str(row, "name", ed_json_str(row, "id", "")));
    edb_add(b, "</div><div class='knAiFlyRow'><span class='knAiFlyK'>Reasoning</span><span class='knAiFlyV'>");
    ai_html_escape(b, reason);
    edb_add(b, "</span></div>");
    const double ctx = ed_json_num(row, "context", 0);
    if (ctx > 0) {
        char c[32];
        fmt_int(c, sizeof(c), (int64_t) ctx);
        edb_addf(b, "<div class='knAiFlyRow'><span class='knAiFlyK'>Context</span><span class='knAiFlyV'>%s tokens</span></div>", c);
    }
    const cJSON *tl = cJSON_GetObjectItemCaseSensitive(row, "tools");
    edb_addf(b, "<div class='knAiFlyRow'><span class='knAiFlyK'>Tools</span><span class='knAiFlyV'>%s</span></div>", (!tl || ed_json_truthy(tl)) ? "yes" : "no");
    if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(row, "vision")))
        edb_add(b, "<div class='knAiFlyRow'><span class='knAiFlyK'>Vision</span><span class='knAiFlyV'>yes</span></div>");
    const double pin = ed_json_num(row, "price_in", -1), pout = ed_json_num(row, "price_out", -1);
    if (pin >= 0 || pout >= 0)
        edb_addf(b, "<div class='knAiFlyRow'><span class='knAiFlyK'>Price</span><span class='knAiFlyV'>$%.2f in, $%.2f out /Mtok</span></div>",
                 pin < 0 ? 0 : pin, pout < 0 ? 0 : pout);
    const char *desc = ed_json_str(row, "description", "");
    if (desc[0]) {
        edb_add(b, "<div class='knAiFlyDesc'>");
        ai_html_escape(b, desc);
        edb_add(b, "</div>");
    }
    edb_add(b, "</div>");
}

static void render_model_pop(void) {
    if (!A.model_pop_el) return;
    if (!A.model_pop) {
        html_dom_set_display(A.model_pop_el, "none");
        html_dom_set_html(A.model_pop_el, "");
        return;
    }
    const char *cur = A.custom ? A.ep_model : A.model;
    EdBuf b;
    edb_init(&b);
    // a flex row: the hover flyout (#knAiModelFly, h_model_hover; hidden by visibility so it keeps its
    // slot) on the side with room, and the scrolling list (its max height is set at open time,
    // model_menu_toggle); the flyout is outside the scroll box, so nothing clips it
    static const char *const fly_html = "<div id='knAiModelFly' class='knAiFly knAiFlyHidden'></div>";
    if (A.fly_left) edb_add(&b, fly_html);
    edb_add(&b, "<div id='knAiModelList' class='knAiModelList'>");
    if (!cJSON_GetArraySize(A.models)) edb_addf(&b, "<div class='knAiNote'>%s</div>", A.models_state == 1 ? "Loading models\xe2\x80\xa6" : "No models.");
    const cJSON *m;
    int i = 0;
    cJSON_ArrayForEach(m, A.models) {
        const char *id = ed_json_str(m, "id", "");
        const char *st = ed_json_str(m, "status", "");
        const int sel = !strcmp(id, cur);
        edb_addf(&b, "<div id='knAiMRow%d' class='knAiModelRow%s' onmouseenter='KE.kn.aiModelHover(%d)' onclick=\"KE.kn.aiModel('", i, sel ? " knAiModelSel" : "", i);
        ai_html_escape(&b, id);
        edb_add(&b, "')\"><span class='knAiModelRowName'>");
        ai_html_escape(&b, ed_json_str(m, "name", id));
        edb_add(&b, "</span>");
        if (!strcmp(st, "unavailable")) edb_add(&b, "<span class='knAiModelBad'>unavailable</span>");
        else if (!strcmp(st, "degraded")) edb_add(&b, "<span class='knAiModelWarn'>degraded</span>");
        else if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(m, "efforts")) || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "reasoning")))
            edb_add(&b, "<i class='material-icons knAiModelReason' title='reasoning model'>psychology</i>");
        if (sel) edb_add(&b, "<i class='material-icons knAiModelCheck'>check</i>");
        edb_add(&b, "</div>");
        i++;
    }
    edb_add(&b, "</div>");
    if (!A.fly_left) edb_add(&b, fly_html);
    A.fly_idx = -1;
    html_dom_set_html(A.model_pop_el, b.buf ? b.buf : "");
    html_dom_set_class(A.model_pop_el, "knAiMLeft", A.fly_left);
    html_dom_set_class(A.model_pop_el, "knAiMRight", !A.fly_left);
    html_dom_set_display(A.model_pop_el, "");
    edb_free(&b);
}

// the hovered model's flyout beside the list (idx -1 hides it): its top follows the row
static void model_hover(const int idx) {
    const HtmlElem fly = html_dom_by_id("knAiModelFly");
    if (!fly || !A.model_pop) return;
    const cJSON *row = cJSON_GetArrayItem(A.models, idx);
    if (idx < 0 || !row) {
        A.fly_idx = -1;
        html_dom_set_class(fly, "knAiFlyHidden", 1);
        return;
    }
    EdBuf b;
    edb_init(&b);
    model_flyout(&b, row);
    html_dom_set_html(fly, b.buf ? b.buf : "");
    edb_free(&b);
    char rid[32], s[32];
    snprintf(rid, sizeof(rid), "knAiMRow%d", idx);
    HtmlRect rr, pr, panel;
    const double k = A.root && html_dom_get_rect(A.root, &panel) && panel.w > 0 ? panel.w / 380.0 : 1.0;
    if (html_dom_get_rect(html_dom_by_id(rid), &rr) && html_dom_get_rect(A.model_pop_el, &pr)) {
        const double top = (rr.y - pr.y) / k - 4;
        snprintf(s, sizeof(s), "%.0fpx", top < 0 ? 0 : top);
        html_dom_set_style(fly, "margin-top", s);
    }
    A.fly_idx = idx;
    html_dom_set_class(fly, "knAiFlyHidden", 0);
}

static void model_menu_toggle(void) {
    A.model_pop = !A.model_pop;
    if (A.model_pop) {
        if (A.pop) {
            A.pop = 0;
            render_pop();
        }
        HtmlRect btn, center, panel;
        float vw = 0, vh = 0, dpr = 1;
        html_dom_viewport(&vw, &vh, &dpr);
        HtmlElem c = html_dom_by_id("center");
        const int have_panel = A.root && html_dom_get_rect(A.root, &panel);
        // the editor page is zoomed: rects are viewport px, styles are layout px (k = px per CSS px,
        // from the panel's known 380 px width, as turn_into_view measures it)
        const double k = have_panel && panel.w > 0 ? panel.w / 380.0 : 1.0;
        // the panel is normally right-docked: the flyout opens to whichever screen side has room
        if (have_panel && vw > 0) A.fly_left = panel.x >= (vw - (panel.x + panel.w));
        else A.fly_left = 1;
        if (A.model_sel && c && html_dom_get_rect(A.model_sel, &btn) && html_dom_get_rect(c, &center)) {
            char s[48];
            // the list under the picker; a left flyout takes its 248 px slot before the list
            const double left = (btn.x - center.x) / k - (A.fly_left ? 248 : 0), top = (btn.y + btn.h - center.y) / k + 2;
            snprintf(s, sizeof(s), "%.0fpx", left < 0 ? 0 : left);
            html_dom_set_style(A.model_pop_el, "left", s);
            snprintf(s, sizeof(s), "%.0fpx", top < 0 ? 0 : top);
            html_dom_set_style(A.model_pop_el, "top", s);
        }
        render_model_pop();
        // the list scrolls within the room below the picker (layout px); the flyout beside it does not
        HtmlRect pr;
        const HtmlElem list = html_dom_by_id("knAiModelList");
        if (list && vh > 0 && html_dom_get_rect(A.model_pop_el, &pr)) {
            char s[32];
            const double room = (vh - pr.y) / k - 24;
            snprintf(s, sizeof(s), "%.0fpx", room < 120 ? 120 : room);
            html_dom_set_style(list, "max-height", s);
        }
        return;
    }
    render_model_pop();
}

// ---------------------------------------------------------------------------
// context-window usage ring (VS Code's chat gauge): the conversation's token count over the model's
// context size. Exact prompt_tokens of the newest turn when known, else ~4 bytes/token.

static int64_t estimate_tokens(void) {
    int64_t bytes = 400; // the system prompt
    const cJSON *m;
    cJSON_ArrayForEach(m, A.conv) {
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "meta"), "failed"))) continue;
        bytes += (int64_t) strlen(ed_json_str(m, "content", ""));
        const cJSON *call;
        cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(m, "tool_calls"))
            bytes += (int64_t) strlen(ed_json_str(cJSON_GetObjectItemCaseSensitive(call, "function"), "arguments", ""));
    }
    return bytes / 4;
}

static void update_ctx(void) {
    int64_t exact = 0;
    const int n = cJSON_GetArraySize(A.conv);
    for (int i = n - 1; i >= 0; i--) {
        const cJSON *mm = cJSON_GetArrayItem(A.conv, i);
        if (strcmp(ed_json_str(mm, "role", ""), "assistant")) continue;
        exact = (int64_t) ed_json_num(cJSON_GetObjectItemCaseSensitive(mm, "meta"), "tokens_in", 0);
        break;
    }
    const int64_t est = estimate_tokens();
    A.ctx_used = exact > est ? exact : est;
    A.ctx_est = exact <= est;
}

// data:image/svg+xml of a VS Code-style progress ring (r=15.915 -> circumference ~100, so the
// dash length is the percentage directly); percent-encoded text (image.c decodes it)
static void ring_svg_uri(EdBuf *out, double frac, const char *color) {
    double pct = frac * 100;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char svg[512];
    snprintf(svg, sizeof(svg),
             "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 36 36'>"
             "<circle cx='18' cy='18' r='15.915' fill='none' stroke='#3a3a3a' stroke-width='4'/>"
             "<circle cx='18' cy='18' r='15.915' fill='none' stroke='%s' stroke-width='4' stroke-linecap='round' "
             "stroke-dasharray='%.2f 100' transform='rotate(-90 18 18)'/></svg>",
             color, pct);
    edb_add(out, "data:image/svg+xml,");
    for (const char *p = svg; *p; p++) {
        const unsigned char ch = (unsigned char) *p;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') edb_addf(out, "%c", ch);
        else edb_addf(out, "%%%02X", ch);
    }
}

static void update_ring(void) {
    if (!A.ring) return;
    update_ctx();
    const double max = model_ctx_size();
    const double frac = max > 0 ? (double) A.ctx_used / max : 0;
    const char *color = frac < 0.6 ? "#1d8eb9" : frac < 0.85 ? "#d6a63a" : "#d9534f";
    char used[32], title[192];
    fmt_int(used, sizeof(used), A.ctx_used);
    if (max > 0) {
        char cap[32];
        fmt_int(cap, sizeof(cap), (int64_t) max);
        snprintf(title, sizeof(title), "Context: %s / %s tokens (%d%%)%s", used, cap, (int) (frac * 100 + 0.5), A.ctx_est ? " est." : "");
    } else {
        snprintf(title, sizeof(title), "Context: ~%s tokens (est.)", used);
    }
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<img class='knAiRingImg' src='");
    ring_svg_uri(&b, max > 0 ? frac : 0, color);
    edb_add(&b, "'>");
    html_dom_set_html(A.ring, b.buf ? b.buf : "");
    html_dom_set_attr(A.ring, "title", title); // always shown (empty until the model's size is known), like VS Code's
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
        // kn_brain: the service's default (glm-5.3-flash, or what its probe falls back to) unless
        // the user picked a model that is still listed
        if (!A.custom && (!A.model_picked || !model_row(cur))) snprintf(cur, cn, "%s", ed_json_str(o, "default", ""));
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
    const int busy = A.streaming || A.tool_step;
    for (int i = 0; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(A.conv, i);
        const char *role = ed_json_str(m, "role", "");
        if (!strcmp(role, "user")) {
            render_user(&b, i, m, busy);
        } else if (!strcmp(role, "assistant")) {
            render_assistant(&b, i, m);
        }
    }
    if (A.redo_conv && !busy) { // after a rewind / restore: Redo until the next message
        const int k = cJSON_GetArraySize(A.redo_conv);
        edb_add(&b, "<div id='knAiRedoBar' class='knAiRedoBar'><i class='material-icons'>restore</i><span class='knAiGrow'>");
        if (A.redo_kind == 2) edb_addf(&b, "Checkpoint restored \xc2\xb7 %d editor change%s undone", A.redo_steps, A.redo_steps == 1 ? "" : "s");
        else edb_addf(&b, "Rewound \xc2\xb7 %d message%s removed", k, k == 1 ? "" : "s");
        edb_add(&b, "</span><span class='knAiBtnS knAiChip' title='Put the removed messages back");
        if (A.redo_kind == 2) edb_add(&b, " and redo the editor changes");
        edb_add(&b, "' onclick='KE.kn.aiRedo()'>Redo</span></div>");
    }
    edb_add(&b, "<div id='knAiLive'></div>");
    html_dom_set_html(A.list, b.buf ? b.buf : "");
    edb_free(&b);
    A.live = html_dom_by_id("knAiLive");
    render_live();
    if (!g_keep_scroll) html_dom_scroll_to_bottom(A.list);
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
    const int tools_shown = A.tools_on && ai_tools_available() && model_has_tools();
    if (A.mode == 1) { // Ask
        edb_add(&b, "Mode: Ask. Answer the user's question directly. Do not call tools or edit the map; if a change is needed, explain "
                    "how the user (or Agent mode) would do it.\n");
    } else if (A.mode == 2) { // Plan
        edb_add(&b, "Mode: Plan. Investigate with the read-only tools if they help (map_info, objects_query, prefabs, render_view), then "
                    "present a clear, numbered plan of the edits you would make. Do NOT modify the map: editing tools are withheld until the "
                    "user approves and switches to Agent mode.\n");
    } else if (tools_shown) { // Agent
        edb_add(&b, "Mode: Agent. You can inspect and change the map open in the editor with the editor tools (they act on the shown map "
                    "unless you pass project). Look before you edit (map_info, objects_query, prefabs); batch edits in one call; every edit "
                    "is one undo step the user can revert; render_view / render_topdown let you see the result, map_validate and playtest "
                    "check it. Destructive tools (deletes, saves, scripts) wait for the user's approval. Say what you changed.\n");
    }
    if (A.mode != 1 && A.web_on && web_available() && model_has_tools())
        edb_add(&b, "web_search and web_fetch look things up on the web.\n");
    const AccountInfo *acc = active_account();
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

// the tool definitions the current mode exposes: Agent all, Plan read-only, Ask none
static cJSON *mode_tools_defs(void) {
    if (A.mode == 1) return NULL;
    cJSON *defs = ai_tools_defs();
    if (!defs || A.mode != 2) return defs;
    for (int i = cJSON_GetArraySize(defs) - 1; i >= 0; i--) {
        const cJSON *d = cJSON_GetArrayItem(defs, i);
        const char *nm = ed_json_str(cJSON_GetObjectItemCaseSensitive(d, "function"), "name", "");
        if (!ai_tool_read_only(nm)) cJSON_DeleteItemFromArray(defs, i);
    }
    return defs;
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
    cJSON *tools = (A.tools_on && tools_ok && A.mode != 1) ? mode_tools_defs() : NULL; // Ask: none; Plan: read-only; Agent: all
    if (tools && !cJSON_GetArraySize(tools)) {
        cJSON_Delete(tools);
        tools = NULL;
    }
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
        if (A.mode != 1 && A.web_on && web_available() && tools_ok) cJSON_AddBoolToObject(body, "web_search", 1);
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
    redo_clear(); // a new message ends the Redo of a rewind / restore
    edit_end();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToObject(meta_of(m), "cp", cp_snapshot()); // this message's checkpoint
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
    int undo_before = 0;
    tool_undo_mark(&undo_before);
    cJSON *res = ai_tool_run(name, ed_json_str(fn, "arguments", "{}"));
    const int undo_steps = tool_undo_count(undo_before);
    if (undo_steps) ed_json_set_num(tm, "undo", undo_steps); // the checkpoint's "by the AI" count
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
        // Autopilot runs to a higher hard cap without pausing; the other levels pause at 25 and ask
        // the user to continue. Neither escapes the editor tool scopes (enforced by kn_editor_mcp).
        const int cap = A.perm == 2 ? AI_MAX_AUTOPILOT_ROUNDS : AI_MAX_TOOL_ROUNDS;
        if (A.rounds >= cap) {
            char t[96];
            snprintf(t, sizeof(t), "Stopped after %d tool rounds: ask again to continue.", cap);
            set_banner(2, t);
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
        // the mode is the hard limit: Ask runs no tools, Plan only read-only ones (no edits)
        if (A.mode == 1) {
            tool_answer(m, id, "Error: Ask mode is read-only: answer directly instead of calling tools.");
            ed_json_set_str(tm, "status", "denied");
            continue;
        }
        if (A.mode == 2 && !ai_tool_read_only(name)) {
            tool_answer(m, id, "Error: Plan mode makes no edits: present a numbered plan for the user to approve, then they switch to Agent mode.");
            ed_json_set_str(tm, "status", "denied");
            continue;
        }
        if (ai_tool_needs_approval(name) && A.perm == 0 && !ed_json_truthy(jget(tm, "approved"))) {
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
        if (choice == 2 && A.perm < 1) { // "Allow all this session": raise the permission level
            A.perm = 1;
            settings_save();
        }
        ed_json_set_bool(tm, "approved", 1);
        ed_json_set_str(tm, "status", "pending");
    }
    A.tool_step = 1;
    render_all();
    update_controls();
}

// ---------------------------------------------------------------------------
// checkpoints (after VS Code's chat checkpoints, src/vs/workbench/contrib/chat: chatEditingActions
// RestoreCheckpointAction, the "Checkpoint Restored · Redo" bar of chatListRenderer)
//
// Every user message records where each open project's undo history stood when it was sent
// (meta.cp {sid, p: [{id, n: undo entries, top: the newest entry's signature}]}, kept with the
// local history), and every editor tool call how many undo steps it added (meta.tools[id].undo).
//   Edit       the message becomes an editor in place; Send drops it and everything after it and
//              sends the new text (asking first when editor changes were made after it: undo them
//              or keep them).
//   Rewind     it and everything after it leave the chat (the map stays), its text goes back into
//              the composer.
//   Restore checkpoint (shown when editor changes were made since): Rewind + the editor's undo back
//              to that point, after a confirmation. Only when the history still leads there: the
//              same editor session (sid) and the same entry at n (a load or the history limit
//              moved it: that project is left alone).
// After a rewind / restore, Redo puts the messages back (and redoes the editor changes, if nothing
// was edited since) until the next message is sent.

static void entry_sig(const EditorHistoryEntry *list, const size_t count, const size_t n, char *out, const size_t cap) {
    if (!n || n > count) {
        snprintf(out, cap, "-");
        return;
    }
    const EditorHistoryEntry *e = &list[n - 1];
    snprintf(out, cap, "%c:%u:%u", e->action, (unsigned) e->uuid, (unsigned) e->batch);
}

// the shown project's last second of UI edits become undo steps first (as the bus's undo does)
static void poll_shown(void) {
    if (!g_ed.shown) return;
    EditorProject *prev;
    ed_project_enter(g_ed.shown, &prev);
    ed_history_poll();
    ed_project_leave(prev);
}

// undo steps (an entry or a whole batch) in p's entries [from, undo_count)
static int steps_since(const EditorProject *p, const size_t from) {
    int steps = 0;
    size_t i = p->undo_count;
    while (i > from) {
        const uint32_t batch = p->undos[--i].batch;
        while (batch && i > from && p->undos[i - 1].batch == batch) i--;
        steps++;
    }
    return steps;
}

static cJSON *cp_snapshot(void) {
    poll_shown();
    cJSON *cp = cJSON_CreateObject();
    cJSON_AddStringToObject(cp, "sid", A.sid);
    cJSON *ps = cJSON_AddArrayToObject(cp, "p");
    for (size_t i = 0; i < g_ed.project_count; i++) {
        const EditorProject *p = g_ed.projects[i];
        char sig[48];
        entry_sig(p->undos, p->undo_count, p->undo_count, sig, sizeof(sig));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", p->id);
        cJSON_AddNumberToObject(o, "n", (double) p->undo_count);
        cJSON_AddStringToObject(o, "top", sig);
        cJSON_AddItemToArray(ps, o);
    }
    return cp;
}

// a checkpoint's project entry whose history still leads back to it (NULL: gone or moved on)
static EditorProject *cp_project(const cJSON *e) {
    EditorProject *p = ed_project_find((int) ed_json_num(e, "id", -1));
    const double n = ed_json_num(e, "n", -1);
    if (!p || n < 0 || (size_t) n > p->undo_count) return NULL;
    char sig[48];
    entry_sig(p->undos, p->undo_count, (size_t) n, sig, sizeof(sig));
    return strcmp(sig, ed_json_str(e, "top", "")) ? NULL : p;
}

static const cJSON *cp_of(const int idx) {
    const cJSON *m = cJSON_GetArrayItem(A.conv, idx);
    if (!m || strcmp(ed_json_str(m, "role", ""), "user")) return NULL;
    const cJSON *cp = jget(jget(m, "meta"), "cp");
    return cp && !strcmp(ed_json_str(cp, "sid", ""), A.sid) ? cp : NULL;
}

// Editor changes (undo steps over the projects) made since user message idx; -1 = none can be
// undone from here (another session's chat, or the history moved on). *ai: those of the AI's tools.
static int cp_changes(const int idx, int *ai) {
    if (ai) *ai = 0;
    const cJSON *cp = cp_of(idx);
    if (!cp) return -1;
    int total = 0, any = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, jget(cp, "p")) {
        const EditorProject *p = cp_project(e);
        if (!p) continue;
        total += steps_since(p, (size_t) ed_json_num(e, "n", 0));
        any = 1;
    }
    if (!any) return -1;
    if (ai) {
        int n = 0;
        for (int i = idx + 1; i < cJSON_GetArraySize(A.conv); i++) {
            const cJSON *tm;
            cJSON_ArrayForEach(tm, jget(jget(cJSON_GetArrayItem(A.conv, i), "meta"), "tools")) n += (int) ed_json_num(tm, "undo", 0);
        }
        *ai = n < total ? n : total;
    }
    return total;
}

// undoes the editor back to user message idx's checkpoint; the redo record into *redo
static int cp_undo(const int idx, cJSON **redo) {
    poll_shown();
    *redo = cJSON_CreateArray();
    const cJSON *cp = cp_of(idx);
    int steps = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, jget(cp, "p")) {
        EditorProject *p = cp_project(e);
        if (!p) continue;
        const size_t n = (size_t) ed_json_num(e, "n", 0);
        EditorProject *prev;
        ed_project_enter(p, &prev);
        int k = 0;
        while (p->undo_count > n && ed_history_step(0)) k++;
        ed_project_leave(prev);
        if (!k) continue;
        steps += k;
        char sig[48];
        entry_sig(p->redos, p->redo_count, p->redo_count, sig, sizeof(sig));
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", p->id);
        cJSON_AddNumberToObject(r, "k", k);
        cJSON_AddNumberToObject(r, "undo", (double) p->undo_count);
        cJSON_AddNumberToObject(r, "redo", (double) p->redo_count);
        cJSON_AddStringToObject(r, "top", sig);
        cJSON_AddItemToArray(*redo, r);
    }
    if (steps) g_ed.dirty_panel = g_ed.dirty_counts = 1;
    return steps;
}

// the editor's redo of a restore; 0 when a project's history moved on since (nothing redone)
static int cp_redo(void) {
    poll_shown();
    const cJSON *r;
    cJSON_ArrayForEach(r, A.redo_map) {
        const EditorProject *p = ed_project_find((int) ed_json_num(r, "id", -1));
        char sig[48];
        if (!p || p->undo_count != (size_t) ed_json_num(r, "undo", -1) || p->redo_count != (size_t) ed_json_num(r, "redo", -1)) return 0;
        entry_sig(p->redos, p->redo_count, p->redo_count, sig, sizeof(sig));
        if (strcmp(sig, ed_json_str(r, "top", ""))) return 0;
    }
    cJSON_ArrayForEach(r, A.redo_map) {
        EditorProject *p = ed_project_find((int) ed_json_num(r, "id", -1)), *prev;
        ed_project_enter(p, &prev);
        for (int k = (int) ed_json_num(r, "k", 0); k > 0 && ed_history_redo_undoable(); k--) {} // undoable again
        ed_project_leave(prev);
    }
    g_ed.dirty_panel = g_ed.dirty_counts = 1;
    return 1;
}

// undo steps over every project (a tool call's count: after - before)
static void tool_undo_mark(int *before) {
    poll_shown();
    int n = 0;
    for (size_t i = 0; i < g_ed.project_count; i++) n += steps_since(g_ed.projects[i], 0);
    *before = n;
}

static int tool_undo_count(const int before) {
    int after;
    tool_undo_mark(&after);
    return after > before ? after - before : 0;
}

static void redo_clear(void) {
    cJSON_Delete(A.redo_conv);
    cJSON_Delete(A.redo_map);
    free(A.redo_text);
    A.redo_conv = A.redo_map = NULL;
    A.redo_text = NULL;
    A.redo_kind = A.redo_steps = 0;
}

static void edit_end(void) {
    A.edit_idx = A.confirm_idx = -1;
    A.confirm_kind = 0;
    free(A.edit_text);
    A.edit_text = NULL;
}

static void render_keep(void) {
    g_keep_scroll = 1;
    render_all();
    g_keep_scroll = 0;
}

// scrolls the list so user turn idx (its confirmation / editor) is in view
static void turn_into_view(const int idx) {
    char id[24];
    snprintf(id, sizeof(id), "knAiU%d", idx);
    const HtmlElem e = html_dom_by_id(id);
    HtmlRect er, lr;
    HtmlScroll sc;
    if (!e || !A.list || !html_dom_get_rect(e, &er) || !html_dom_get_rect(A.list, &lr) || !html_dom_get_scroll(A.list, &sc) || sc.client_h <= 0) return;
    const float k = lr.h / sc.client_h; // viewport px per CSS px (the editor page is scaled)
    if (k <= 0) return;
    float top = sc.top;
    if (er.y < lr.y + 4 * k) top -= (lr.y + 8 * k - er.y) / k;
    else if (er.y + er.h > lr.y + lr.h) top += (er.y + er.h - (lr.y + lr.h) + 8 * k) / k;
    if (er.h > lr.h) top = sc.top + (er.y - lr.y - 8 * k) / k; // taller than the list: its top
    if (top != sc.top) html_dom_set_scroll(A.list, sc.left, top < 0 ? 0 : top);
}

// User message idx and everything after it leave the chat (into the Redo stash); undo_map: the
// editor changes since go too; put_text: its text into the empty composer.
static void rewind_to(const int idx, const int undo_map, const int put_text) {
    if (A.streaming || A.tool_step) stop_turn();
    const cJSON *m = cJSON_GetArrayItem(A.conv, idx);
    if (!m || strcmp(ed_json_str(m, "role", ""), "user")) return;
    char *text = strdup(ed_json_str(m, "content", ""));
    redo_clear();
    edit_end();
    cJSON *redo_map = NULL;
    const int steps = undo_map ? cp_undo(idx, &redo_map) : 0;
    A.redo_conv = cJSON_CreateArray();
    while (cJSON_GetArraySize(A.conv) > idx) cJSON_AddItemToArray(A.redo_conv, cJSON_DetachItemFromArray(A.conv, idx));
    A.redo_map = redo_map;
    A.redo_kind = undo_map ? 2 : 1;
    A.redo_steps = steps;
    if (put_text && A.input && text) {
        char cur[4] = "";
        html_dom_get_value(A.input, cur, sizeof(cur));
        if (!cur[0]) {
            html_dom_set_value(A.input, text);
            A.redo_text = text;
            text = NULL;
        }
    }
    free(text);
    A.rounds = 0;
    if (cJSON_GetArraySize(A.conv)) history_save();
    else if (A.chat_id[0]) { // nothing left: the saved chat goes until Redo or a new message saves it again
        char id[32];
        snprintf(id, sizeof(id), "%s", A.chat_id);
        history_delete(id);
        snprintf(A.chat_id, sizeof(A.chat_id), "%s", id);
    }
    render_all();
    update_controls();
}

static void redo_apply(void) {
    if (!A.redo_conv || A.streaming || A.tool_step) return;
    const int map_ok = cJSON_GetArraySize(A.redo_map) ? cp_redo() : 1;
    cJSON *it;
    while ((it = cJSON_DetachItemFromArray(A.redo_conv, 0))) cJSON_AddItemToArray(A.conv, it);
    if (A.redo_text && A.input) { // the composer still holds just the rewound text: cleared
        const int len = html_dom_get_value(A.input, NULL, 0);
        char *cur = len >= 0 ? malloc((size_t) len + 1) : NULL;
        if (cur) {
            html_dom_get_value(A.input, cur, (size_t) len + 1);
            if (!strcmp(cur, A.redo_text)) html_dom_set_value(A.input, "");
            free(cur);
        }
    }
    redo_clear();
    edit_end();
    if (!map_ok) set_banner(2, "The chat is back, but the editor changes were not redone: the map was edited after the restore.");
    history_save();
    render_all();
    update_controls();
}

// Send an edited user message: idx and everything after it go, then the new text is sent.
static void edit_send(const int idx, const char *text, const int undo_map) {
    char *t = strdup(text);
    if (!t) return;
    rewind_to(idx, undo_map, 0);
    redo_clear(); // a new message ends the Redo (VS Code drops the removed requests on send)
    submit_text(t);
    free(t);
}

static void restore_ask(const int idx) {
    if (A.streaming || A.tool_step) return;
    const int ch = cp_changes(idx, NULL);
    if (ch <= 0) { // nothing to undo: a plain rewind
        rewind_to(idx, 0, 1);
        return;
    }
    edit_end();
    A.confirm_idx = idx;
    A.confirm_kind = 1;
    render_keep();
    turn_into_view(idx);
}

// the user turn: checkpoint divider (or its confirmation), the bubble (or its editor), actions
static void render_user(EdBuf *b, const int idx, const cJSON *m, const int busy) {
    const char *text = ed_json_str(m, "content", "");
    edb_addf(b, "<div class='knAiUTurn' id='knAiU%d'>", idx);
    if (A.confirm_idx == idx) {
        int ai = 0;
        const int ch = cp_changes(idx, &ai);
        const int n = ch > 0 ? ch : 0;
        edb_add(b, "<div class='knAiConfirm'>");
        if (A.confirm_kind == 1) {
            edb_add(b, "<div class='knAiConfirmTitle'><i class='material-icons'>restore</i>Restore this checkpoint?</div><div class='knAiConfirmText'>");
            edb_addf(b, "This message and everything after it leave the chat, and the %d editor change%s made since %s undone", n, n == 1 ? "" : "s",
                     n == 1 ? "is" : "are");
            if (ai < n) edb_addf(b, " (%d by the AI, %d of your own)", ai, n - ai);
            edb_add(b, ". Redo brings them back until you send a new message.</div><div class='knAiConfirmBar'>"
                       "<span class='knAiBtnS knAiChip' onclick='KE.kn.aiConfirm(0)'>Cancel</span>"
                       "<span class='knAiBtnS knAiSend' onclick='KE.kn.aiConfirm(1)'>Restore</span></div>");
        } else {
            edb_add(b, "<div class='knAiConfirmTitle'><i class='material-icons'>edit</i>Undo the editor changes made after this message?</div><div class='knAiConfirmText'>");
            edb_addf(b, "%d editor change%s %s made after it", n, n == 1 ? "" : "s", n == 1 ? "was" : "were");
            if (ai) edb_addf(b, " (%d by the AI)", ai);
            edb_add(b, ". The later messages leave the chat either way.</div><div class='knAiConfirmBar'>"
                       "<span class='knAiBtnS knAiChip' onclick='KE.kn.aiConfirm(0)'>Cancel</span>"
                       "<span class='knAiBtnS knAiChip' onclick='KE.kn.aiConfirm(2)'>Keep and send</span>"
                       "<span class='knAiBtnS knAiSend' onclick='KE.kn.aiConfirm(1)'>Undo and send</span></div>");
        }
        edb_add(b, "</div>");
    } else if (!busy && A.edit_idx != idx) {
        const int ch = cp_changes(idx, NULL);
        if (ch > 0) {
            edb_addf(b, "<div class='knAiCp'><span class='knAiCpLine'></span><span class='knAiCpBtn' title='Undo the %d editor change%s made since "
                        "this message and remove it and everything after it' onclick='KE.kn.aiRestore(%d)'><i class='material-icons'>restore</i>"
                        "Restore checkpoint</span><span class='knAiCpLine'></span></div>",
                     ch, ch == 1 ? "" : "s", idx);
        }
    }
    edb_add(b, "<div class='knAiMsg knAiUser'>");
    if (A.edit_idx == idx) {
        edb_add(b, "<div class='knAiEdit'><textarea id='knAiEditInput' class='knAiEditInput' rows='3' spellcheck='false' "
                   "onkeydown='return KE.kn.aiEditKey()'>");
        ai_html_escape(b, A.edit_text ? A.edit_text : text);
        edb_add(b, "</textarea><div class='knAiEditBar'><span class='knAiBtnS knAiChip' onclick='KE.kn.aiEditCancel()'>Cancel</span>"
                   "<span class='knAiBtnS knAiSend' onclick='KE.kn.aiEditSend()'>Send</span></div></div>");
    } else {
        edb_add(b, "<div class='knAiBubble'>");
        ai_html_escape(b, text);
        edb_add(b, "</div>");
        if (!busy) {
            edb_addf(b, "<div class='knAiActs knAiUActs'><span class='knAiAct' title='Edit this message and send it again' onclick='KE.kn.aiEdit(%d)'>Edit</span>"
                        "<span class='knAiAct' title='Remove this message and everything after it (the map stays as it is)' "
                        "onclick='KE.kn.aiRewind(%d)'>Rewind</span></div>",
                     idx, idx);
        }
    }
    edb_add(b, "</div></div>");
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
        redo_clear();
        edit_end();
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
        static const char *const perm_note[] = {"destructive tools ask first",
                                                 "every tool is allowed this session (<span class='knAiLink' onclick='KE.kn.aiAsk()'>ask again</span>)",
                                                 "Autopilot: tools run and continue without prompting (<span class='knAiLink' onclick='KE.kn.aiAsk()'>ask again</span>)"};
        edb_addf(&b, "<div class='knAiNote'>Editor tools: %s. %s.</div>",
                 ai_tools_available() ? "the AI can read and edit the open map" : ai_tools_unavailable_reason(), perm_note[A.perm < 0 || A.perm > 2 ? 0 : A.perm]);
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
    if (!A.open) {
        if (A.model_pop) {
            A.model_pop = 0;
            render_model_pop();
        }
        return;
    }
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
    const AccountInfo *acc = active_account();
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
    redo_clear();
    edit_end();
    cJSON_Delete(A.conv);
    cJSON_Delete(A.models);
    cJSON_Delete(A.server_tool_names);
    memset(A.ep_key, 0, sizeof(A.ep_key));
    memset(&A, 0, sizeof(A));
}

int ed_ai_keydown(const HtmlEvent *ev) {
    if ((A.pop || A.model_pop) && ev && html_key_is_escape(ev->key)) {
        A.pop = 0;
        A.model_pop = 0;
        render_pop();
        render_model_pop();
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
    (void) u; (void) n;
    char buf[96] = "";
    const char *id = arg_str(a, n, 0); // from a dropdown row
    if (id[0]) snprintf(buf, sizeof(buf), "%s", id);
    else if (ev && ev->value && ev->value[0]) snprintf(buf, sizeof(buf), "%s", ev->value);
    if (!buf[0]) return 1;
    A.model_pop = 0; // a pick closes the dropdown
    render_model_pop();
    if (A.custom) {
        snprintf(A.ep_model, sizeof(A.ep_model), "%s", buf);
        endpoint_save();
    } else {
        snprintf(A.model, sizeof(A.model), "%s", buf);
        A.model_picked = 1;
        settings_save();
        if (!model_warning()) health_banner();
    }
    fill_models();
    update_controls();
    return 1;
}

H(h_use) {
    (void) u; (void) ev;
    const char *id = arg_str(a, n, 0);
    if (!id[0] || A.custom) return 1;
    snprintf(A.model, sizeof(A.model), "%s", id);
    A.model_picked = 1;
    settings_save();
    fill_models();
    set_banner(0, NULL);
    if (!model_warning()) health_banner();
    update_controls();
    return 1;
}

static void new_chat(void) {
    if (A.streaming || A.tool_step) stop_turn();
    redo_clear();
    edit_end();
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
    if (A.pop && A.model_pop) {
        A.model_pop = 0;
        render_model_pop();
    }
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
    A.perm = 0;
    settings_save();
    render_pop();
    update_controls();
    return 1;
}

H(h_model_menu) {
    UNUSED;
    model_menu_toggle();
    return 1;
}

H(h_model_hover) {
    (void) u; (void) ev;
    model_hover(n > 0 ? (int) arg_num(a, n, 0) : -1);
    return 1;
}

H(h_mode) {
    (void) u; (void) a; (void) n;
    const char *v = ev && ev->value ? ev->value : "";
    A.mode = v[0] >= '0' && v[0] <= '2' ? v[0] - '0' : 0;
    settings_save();
    update_controls();
    return 1;
}

H(h_perm) {
    (void) u; (void) a; (void) n;
    const char *v = ev && ev->value ? ev->value : "";
    A.perm = v[0] >= '0' && v[0] <= '2' ? v[0] - '0' : 0;
    settings_save();
    update_controls();
    return 1;
}

// ---- checkpoints: Edit / Rewind / Restore checkpoint / Redo

static int user_index(const HtmlArg *a, const int n) {
    const int i = (int) arg_num(a, n, 0);
    const cJSON *m = cJSON_GetArrayItem(A.conv, i);
    return m && !strcmp(ed_json_str(m, "role", ""), "user") ? i : -1;
}

static void edit_open(const int i) {
    if (i < 0 || A.streaming || A.tool_step) return;
    edit_end();
    A.edit_idx = i;
    render_keep();
    turn_into_view(i);
    const HtmlElem e = html_dom_by_id("knAiEditInput");
    if (e) html_dom_focus(e);
}

// the edit's text (trimmed; caller frees), NULL when empty
static char *edit_value(void) {
    const HtmlElem e = html_dom_by_id("knAiEditInput");
    const int len = e ? html_dom_get_value(e, NULL, 0) : -1;
    char *t = len > 0 ? malloc((size_t) len + 1) : NULL;
    if (!t) return NULL;
    html_dom_get_value(e, t, (size_t) len + 1);
    size_t l = strlen(t), k = 0;
    while (l && strchr(" \n\r\t", t[l - 1])) t[--l] = 0;
    while (t[k] && strchr(" \n\r\t", t[k])) k++;
    if (!t[k]) {
        free(t);
        return NULL;
    }
    memmove(t, t + k, strlen(t + k) + 1);
    return t;
}

static void edit_submit(void) {
    const int i = A.edit_idx;
    if (i < 0 || A.streaming || A.tool_step) return;
    char *t = edit_value();
    if (!t) return;
    if (cp_changes(i, NULL) > 0) { // editor changes after it: undo them or keep them?
        free(A.edit_text);
        A.edit_text = t;
        A.confirm_idx = i;
        A.confirm_kind = 2;
        render_keep();
        turn_into_view(i);
        return;
    }
    edit_send(i, t, 0);
    free(t);
}

static void confirm_choice(const int choice) {
    const int i = A.confirm_idx, kind = A.confirm_kind;
    if (i < 0) return;
    if (kind == 1) {
        if (choice == 1) rewind_to(i, 1, 1);
        else {
            edit_end();
            render_keep();
        }
    } else if (kind == 2) {
        if (choice == 0) { // back to the editor, text kept
            A.confirm_idx = -1;
            A.confirm_kind = 0;
            render_keep();
            return;
        }
        char *t = A.edit_text ? strdup(A.edit_text) : NULL;
        if (t) edit_send(i, t, choice == 1);
        free(t);
    }
}

H(h_edit) {
    (void) u; (void) ev;
    edit_open(user_index(a, n));
    return 1;
}

H(h_edit_cancel) {
    UNUSED;
    edit_end();
    render_keep();
    return 1;
}

H(h_edit_send) {
    UNUSED;
    edit_submit();
    return 1;
}

H(h_edit_key) {
    (void) u; (void) a; (void) n;
    if (ev && html_key_is_enter(ev->key) && !(ev->mods & 1)) { // Enter sends, Shift+Enter is a new line
        edit_submit();
        return 0;
    }
    if (ev && html_key_is_escape(ev->key)) {
        edit_end();
        render_keep();
        return 0;
    }
    return 1;
}

H(h_rewind) {
    (void) u; (void) ev;
    const int i = user_index(a, n);
    if (i >= 0 && !A.streaming && !A.tool_step) rewind_to(i, 0, 1);
    return 1;
}

H(h_restore) {
    (void) u; (void) ev;
    const int i = user_index(a, n);
    if (i >= 0) restore_ask(i);
    return 1;
}

H(h_confirm) {
    (void) u; (void) ev;
    confirm_choice((int) arg_num(a, n, 0));
    return 1;
}

H(h_redo) {
    UNUSED;
    redo_apply();
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
        {"KE.kn.aiAsk", h_ask},         {"KE.kn.aiUse", h_use},         {"KE.kn.aiEdit", h_edit},       {"KE.kn.aiEditCancel", h_edit_cancel},
        {"KE.kn.aiEditSend", h_edit_send}, {"KE.kn.aiEditKey", h_edit_key}, {"KE.kn.aiRewind", h_rewind}, {"KE.kn.aiRestore", h_restore},
        {"KE.kn.aiConfirm", h_confirm}, {"KE.kn.aiRedo", h_redo},
        {"KE.kn.aiModelMenu", h_model_menu}, {"KE.kn.aiMode", h_mode},    {"KE.kn.aiPerm", h_perm},       {"KE.kn.aiModelHover", h_model_hover},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// ---------------------------------------------------------------------------
// KRUNKNATIVE_EDITOR_SCRIPT "ai:..." (tools/editor/ai_e2e.py)
//   ai:open / ai:close, ai:model:<id>, ai:send:<text>, ai:stop, ai:new, ai:trace:<i>, ai:tool:<i>:<k>,
//   ai:approve:<i>:<k>:<0 deny|1 allow|2 allow all>, ai:tools:<0|1>, ai:web:<0|1>, ai:effort:<e>,
//   ai:mode:<0 Agent|1 Ask|2 Plan>, ai:perm:<0 ask|1 allow all|2 autopilot>,
//   ai:endpoint:<url>|<key>|<model> (own endpoint; "ai:endpoint:" = back to kn_brain),
//   ai:history:<n> (open the nth saved chat), ai:pop:<0|1|2>, ai:dump:<path> (JSON state),
//   checkpoints: ai:edit:<i>, ai:edittext:<text>, ai:editsend, ai:rewind:<i>, ai:restore:<i>,
//   ai:confirm:<0 cancel|1 ok / undo and send|2 keep and send>, ai:redo,
//   ai:boxes:<path> (viewport rects of the header and composer controls), ai:openmodels (opens the
//   model dropdown), ai:hovermodel:<i> (a frame later: the mouse onto row i, its flyout shows in the
//   run's screenshot), ai:closemodels

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
    cJSON_AddBoolToObject(o, "allow_all", A.perm >= 1); // kept for compatibility
    cJSON_AddNumberToObject(o, "perm", A.perm);
    cJSON_AddNumberToObject(o, "mode", A.mode);
    cJSON_AddBoolToObject(o, "model_pop", A.model_pop);
    cJSON_AddBoolToObject(o, "fly_left", A.fly_left);
    update_ctx();
    cJSON_AddNumberToObject(o, "ctx_used", (double) A.ctx_used);
    cJSON_AddNumberToObject(o, "ctx_max", model_ctx_size());
    cJSON_AddBoolToObject(o, "ctx_est", A.ctx_est);
    cJSON *td = (A.tools_on && model_has_tools() && A.mode != 1) ? mode_tools_defs() : NULL;
    cJSON_AddNumberToObject(o, "tools_offered", td ? cJSON_GetArraySize(td) : 0); // Ask 0, Plan read-only only, Agent all
    cJSON_Delete(td);
    if (A.model_pop_el && A.model_pop) {
        char *mt = malloc(16384);
        const HtmlElem fly = html_dom_by_id("knAiModelFly"), list = html_dom_by_id("knAiModelList");
        if (mt) {
            mt[0] = 0;
            if (list) html_dom_get_text(list, mt, 16384);
            cJSON_AddStringToObject(o, "model_pop_text", mt);
            mt[0] = 0;
            if (fly && A.fly_idx >= 0) html_dom_get_text(fly, mt, 16384);
            cJSON_AddStringToObject(o, "fly_text", mt);
            free(mt);
        }
        HtmlRect r;
        cJSON_AddBoolToObject(o, "fly_shown", fly && A.fly_idx >= 0);
        if (fly && html_dom_get_rect(fly, &r)) cJSON_AddItemToObject(o, "fly_rect", cJSON_CreateDoubleArray((const double[]) {r.x, r.y, r.w, r.h}, 4));
        if (list && html_dom_get_rect(list, &r)) cJSON_AddItemToObject(o, "list_rect", cJSON_CreateDoubleArray((const double[]) {r.x, r.y, r.w, r.h}, 4));
    }
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
    cJSON_AddBoolToObject(o, "account_indicator", html_dom_by_id("knAiAccount") != 0);
    cJSON_AddBoolToObject(o, "shown", A.root && html_dom_is_shown(A.root));
    cJSON_AddBoolToObject(o, "model_picked", A.model_picked);
    cJSON_AddNumberToObject(o, "edit_idx", A.edit_idx);
    cJSON_AddNumberToObject(o, "confirm_idx", A.confirm_idx);
    cJSON_AddNumberToObject(o, "confirm_kind", A.confirm_kind);
    cJSON_AddNumberToObject(o, "redo_kind", A.redo_kind);
    cJSON_AddNumberToObject(o, "redo_steps", A.redo_steps);
    cJSON_AddNumberToObject(o, "redo_messages", cJSON_GetArraySize(A.redo_conv));
    cJSON *chg = cJSON_AddArrayToObject(o, "changes"); // per message: editor changes since (user messages), else null
    for (int i = 0; i < cJSON_GetArraySize(A.conv); i++) {
        int ai = 0;
        const int c = cp_changes(i, &ai);
        cJSON_AddItemToArray(chg, c >= 0 ? cJSON_CreateIntArray((const int[]) {c, ai}, 2) : cJSON_CreateNull());
    }
    char in[1024] = "";
    if (A.input) html_dom_get_value(A.input, in, sizeof(in));
    cJSON_AddStringToObject(o, "input", in);
    cJSON_AddNumberToObject(o, "objects", g_ed.proj ? (double) g_ed.proj->count : -1);
    char *s = cJSON_Print(o);
    FILE *f = fopen(path, "w");
    if (f && s) fputs(s, f);
    if (f) fclose(f);
    free(s);
    cJSON_Delete(o);
}

// viewport rects (CSS px) of the header row and the composer's buttons: the alignment and size
// checks of tools/editor/ai_e2e.py (Stop is measured shown for a moment)
static void dump_boxes(const char *path) {
    static const char *const ids[] = {"knAi", "knAiHead", "knAiTitle", "knAiModel", "knAiNewBtn", "knAiHistBtn", "knAiSetBtn", "knAiCloseBtn",
                                      "knAiTools", "knAiWeb", "knAiSend", "knAiStop", "knAiMode", "knAiPerm", "knAiRing", "knAiBanner",
                                      "knAiList", "knAiInput", "knAiUsage"};
    cJSON *o = cJSON_CreateObject();
    const int stop_hidden = A.stop && !html_dom_is_shown(A.stop);
    if (stop_hidden) html_dom_set_display(A.stop, "");
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        HtmlRect r;
        const HtmlElem e = html_dom_by_id(ids[i]);
        if (!e || !html_dom_get_rect(e, &r)) continue;
        cJSON_AddItemToObject(o, ids[i], cJSON_CreateDoubleArray((const double[]) {r.x, r.y, r.w, r.h}, 4));
    }
    if (stop_hidden) html_dom_set_display(A.stop, "none");
    char *t = cJSON_Print(o);
    FILE *f = fopen(path, "w");
    if (f && t) fputs(t, f);
    if (f) fclose(f);
    free(t);
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
        else {
            snprintf(A.model, sizeof(A.model), "%s", v);
            A.model_picked = 1;
        }
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
    } else if (IS("mode")) {
        A.mode = atoi(v) >= 0 && atoi(v) <= 2 ? atoi(v) : 0;
        settings_save();
        update_controls();
    } else if (IS("perm")) {
        A.perm = atoi(v) >= 0 && atoi(v) <= 2 ? atoi(v) : 0;
        settings_save();
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
    } else if (IS("edit")) {
        edit_open(atoi(v));
    } else if (IS("edittext")) {
        const HtmlElem e = html_dom_by_id("knAiEditInput");
        if (e) html_dom_set_value(e, v);
    } else if (IS("editsend")) {
        edit_submit();
    } else if (IS("rewind")) {
        HtmlArg x = {HTML_ARG_NUM, atof(v), NULL};
        h_rewind(NULL, NULL, &x, 1);
    } else if (IS("restore")) {
        HtmlArg x = {HTML_ARG_NUM, atof(v), NULL};
        h_restore(NULL, NULL, &x, 1);
    } else if (IS("confirm")) {
        confirm_choice(atoi(v));
    } else if (IS("redo")) {
        redo_apply();
    } else if (IS("boxes")) {
        dump_boxes(v);
    } else if (IS("openmodels")) {
        if (!A.model_pop) model_menu_toggle();
    } else if (IS("hovermodel")) {
        // moves the mouse onto model row <i> of the open dropdown (a frame after openmodels, once it
        // is laid out): the engine's mouseenter runs the row's handler, the flyout shows
        char rid[32];
        HtmlRect r;
        snprintf(rid, sizeof(rid), "knAiMRow%d", atoi(v));
        if (A.model_pop && html_dom_get_rect(html_dom_by_id(rid), &r)) {
            const double k = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
            html_ui_mouse_move((r.x + r.w / 2) * k, (r.y + r.h / 2) * k);
        }
    } else if (IS("closemodels")) {
        if (A.model_pop) model_menu_toggle();
    } else if (IS("dump")) {
        dump_state(v);
    } else {
        fprintf(stderr, "[editor] ai: unknown command %s\n", arg);
    }
#undef IS
    return 1;
}
