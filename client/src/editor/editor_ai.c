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
#define AI_MAX_CONV_BYTES (700u * 1024u) // older turns are dropped from a request beyond this (brain body limit 1 MiB)

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
} A;

static void render_all(void);
static void render_live(void);
static void set_banner(int kind, const char *text);
static void send_turn(void);
static void update_controls(void);

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
        "<div class='knAiIcon material-icons' title='Close' onclick='KE.kn.aiToggle()'>close</div>"
        "</div>"
        "<div id='knAiBanner' class='knAiBanner' style='display:none'></div>"
        "<div id='knAiList' class='knAiList'></div>"
        "<div class='knAiComposer'>"
        "<textarea id='knAiInput' class='knAiInput' rows='2' spellcheck='false' placeholder='Ask anything about this map'"
        " onkeydown='return KE.kn.aiKey()'></textarea>"
        "<div class='knAiBar'>"
        "<div id='knAiAccount' class='knAiAccount'></div>"
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
    A.built = A.root != 0;
    A.signed_in_seen = -1;
    if (!A.conv) A.conv = cJSON_CreateArray();
    char saved[96] = "";
    if (html_storage_get("knAiModel", saved, sizeof(saved)) > 0) snprintf(A.model, sizeof(A.model), "%s", saved);
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
    char in[32], out[32], cost[32], tok[32], line[256];
    fmt_int(in, sizeof(in), A.sess_in);
    fmt_int(out, sizeof(out), A.sess_out);
    fmt_cost(cost, sizeof(cost), A.sess_cost);
    fmt_int(tok, sizeof(tok), A.tok_today);
    if (!A.sess_in && !A.sess_out && !A.req_today) {
        html_dom_set_text(A.usage, "Free for signed-in accounts");
        return;
    }
    snprintf(line, sizeof(line), "This chat: %s in / %s out%s%s%s \xc2\xb7 today: %d requests, %s tokens", in, out, cost[0] ? " \xc2\xb7 " : "",
             cost, cost[0] ? " (est.)" : "", A.req_today, tok);
    html_dom_set_text(A.usage, line);
}

static void update_controls(void) {
    if (!A.built) return;
    const int can = signed_in() && brain_ready();
    html_dom_set_display(A.send, A.streaming ? "none" : "");
    html_dom_set_display(A.stop, A.streaming ? "" : "none");
    html_dom_set_class(A.send, "knAiOff", !can);
    if (can) html_dom_remove_attr(A.input, "disabled");
    else html_dom_set_attr(A.input, "disabled", "");
    update_account();
    update_usage();
}

static void fill_models(void) {
    if (!A.model_sel) return;
    EdBuf b;
    edb_init(&b);
    if (!cJSON_GetArraySize(A.models)) {
        edb_add(&b, "<option value=''>");
        edb_add(&b, A.models_state == 1 ? "Loading models..." : "No models");
        edb_add(&b, "</option>");
    }
    const cJSON *m;
    cJSON_ArrayForEach(m, A.models) {
        const char *id = ed_json_str(m, "id", "");
        edb_add(&b, "<option value='");
        ai_html_escape(&b, id);
        edb_addf(&b, "'%s>", !strcmp(id, A.model) ? " selected" : "");
        ai_html_escape(&b, ed_json_str(m, "name", id));
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "reasoning"))) edb_add(&b, " \xc2\xb7 thinks");
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(m, "tools"))) edb_add(&b, " \xc2\xb7 tools");
        const double pin = ed_json_num(m, "price_in", -1), pout = ed_json_num(m, "price_out", -1);
        if (pin >= 0 && pout >= 0) edb_addf(&b, " \xc2\xb7 $%.2f/$%.2f", pin, pout);
        edb_add(&b, "</option>");
    }
    html_dom_set_html(A.model_sel, b.buf ? b.buf : "");
    if (A.model[0]) html_dom_set_value(A.model_sel, A.model);
    edb_free(&b);
}

// ---------------------------------------------------------------------------
// GET /ai/models

static void models_done(void *user, const int status, const char *body, const size_t len) {
    (void) user;
    cJSON *o = status ? cJSON_ParseWithLength(body, len) : NULL;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(o, "models");
    const cJSON *health = cJSON_GetObjectItemCaseSensitive(o, "health");
    snprintf(A.health, sizeof(A.health), "%s", ed_json_str(health, "state", ""));
    snprintf(A.health_reason, sizeof(A.health_reason), "%s", ed_json_str(health, "reason", ""));
    if (cJSON_IsArray(list) && cJSON_GetArraySize(list) > 0) {
        cJSON_Delete(A.models);
        A.models = cJSON_Duplicate(list, 1);
        A.models_state = 2;
        A.models_error[0] = 0;
        if (!A.model[0] || !model_row(A.model)) snprintf(A.model, sizeof(A.model), "%s", ed_json_str(o, "default", ""));
        if (!model_row(A.model)) snprintf(A.model, sizeof(A.model), "%s", ed_json_str(cJSON_GetArrayItem(A.models, 0), "id", ""));
    } else {
        A.models_state = 3;
        snprintf(A.models_error, sizeof(A.models_error), "%s",
                 o ? ed_json_str(o, "message", "the AI service is not available") : status ? "the AI service answered with an error" : body);
    }
    cJSON_Delete(o);
    if (!A.built) return;
    fill_models();
    if (A.models_state == 3 && A.banner_kind != 4) {
        char t[400];
        snprintf(t, sizeof(t), "AI unavailable: %s", A.models_error);
        set_banner(2, t);
    } else {
        health_banner();
    }
}

static void models_load(void) {
    if (A.models_state == 1 || !brain_ready()) return;
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

static void render_tool_card(EdBuf *b, const int idx, const int k, const cJSON *call, const cJSON *result, const cJSON *meta) {
    const cJSON *fn = jget(call, "function");
    const char *name = ed_json_str(fn, "name", "?");
    const char *id = ed_json_str(call, "id", "");
    const cJSON *tm = jget(jget(meta, "tools"), id);
    const char *status = ed_json_str(tm, "status", result ? "done" : "pending");
    const char *content = result ? ed_json_str(result, "content", "") : ed_json_str(tm, "result", "");
    const int is_err = !strcmp(status, "error") || !strcmp(status, "denied") || !strncmp(content, "Error:", 6);
    const int open = ed_json_truthy(jget(tm, "open"));
    const char *icon = strstr(name, "search") ? "search" : strstr(name, "fetch") ? "public" : strstr(name, "render") ? "photo_camera"
                     : strstr(name, "playtest") ? "sports_esports" : strstr(name, "validate") ? "rule" : "build";
    edb_addf(b, "<div class='knAiTool%s'><div class='knAiToolHead' onclick='KE.kn.aiTool(%d,%d)'>", is_err ? " knAiToolErr" : "", idx, k);
    edb_addf(b, "<i class='material-icons'>%s</i><span class='knAiToolName'>", icon);
    ai_html_escape(b, name);
    edb_add(b, "</span><span class='knAiToolState'>");
    if (!strcmp(status, "running") || !strcmp(status, "pending")) edb_add(b, "<span class='knAiDot'></span>running");
    else if (!strcmp(status, "approval")) edb_add(b, "waiting for approval");
    else if (!strcmp(status, "denied")) edb_add(b, "&#10007; denied");
    else if (is_err) edb_add(b, "&#10007; failed");
    else edb_add(b, "&#10003; done");
    const double ms = ed_json_num(tm, "ms", -1);
    if (ms >= 0) edb_addf(b, " \xc2\xb7 %.1fs", ms / 1000.0);
    edb_add(b, "</span></div>");
    if (open) {
        edb_add(b, "<div class='knAiToolBody'><div class='knAiToolLbl'>arguments</div><pre class='knAiPre'>");
        const char *args = ed_json_str(fn, "arguments", "{}");
        cJSON *pa = cJSON_Parse(args);
        char *pretty = pa ? cJSON_Print(pa) : NULL;
        ai_html_escape(b, pretty ? pretty : args);
        free(pretty);
        cJSON_Delete(pa);
        edb_add(b, "</pre>");
        if (content[0]) {
            edb_add(b, "<div class='knAiToolLbl'>result</div><pre class='knAiPre'>");
            const size_t cl = strlen(content);
            if (cl > 6000) {
                char *cut = malloc(6001);
                if (cut) {
                    memcpy(cut, content, 6000);
                    cut[6000] = 0;
                    ai_html_escape(b, cut);
                    free(cut);
                }
                edb_addf(b, "\n[... %zu more bytes]", cl - 6000);
            } else {
                ai_html_escape(b, content);
            }
            edb_add(b, "</pre>");
        }
        edb_add(b, "</div>");
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
    ai_html_escape(b, model_label(ed_json_str(meta, "model", A.model)));
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
                    "<div class='knAiEmptyText'>Ask about Krunker mapping, or describe what to build.</div></div>");
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
    const cJSON *name;
    cJSON_ArrayForEach(name, A.tool_starts) {
        edb_add(&b, "<div class='knAiTool'><div class='knAiToolHead'><i class='material-icons'>build</i><span class='knAiToolName'>");
        ai_html_escape(&b, name->valuestring);
        edb_add(&b, "</span><span class='knAiToolState'><span class='knAiDot'></span>preparing</span></div></div>");
    }
    const cJSON *ev;
    cJSON_ArrayForEach(ev, A.server_tools) {
        const char *st = ed_json_str(ev, "status", "");
        edb_add(&b, "<div class='knAiTool'><div class='knAiToolHead'><i class='material-icons'>search</i><span class='knAiToolName'>");
        ai_html_escape(&b, ed_json_str(ev, "name", ""));
        edb_add(&b, "</span><span class='knAiToolState'>");
        if (!strcmp(st, "running")) edb_add(&b, "<span class='knAiDot'></span>running");
        else edb_addf(&b, "%s \xc2\xb7 %.1fs", !strcmp(st, "error") ? "&#10007; failed" : "&#10003; done", ed_json_num(ev, "ms", 0) / 1000.0);
        edb_add(&b, "</span></div></div>");
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
        // a new round after server tools: the shown text keeps growing in the same live message
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
    else if (A.errbody.len < 65536) ai_buf_add(&A.errbody, data, len);
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
    // clear "last" on older replies: the footer belongs to the newest
    const cJSON *m;
    cJSON_ArrayForEach(m, A.conv) cJSON_DeleteItemFromObjectCaseSensitive(meta_of((cJSON *) m), "last");
    cJSON *last_assistant = NULL;
    cJSON *it;
    while (added && (it = cJSON_DetachItemFromArray(added, 0))) {
        if (!strcmp(ed_json_str(it, "role", ""), "assistant")) last_assistant = it;
        // brain-run tool results: their card shows the result (tool messages are not drawn)
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
        if (cost < 0) cost = (ed_json_num(model_row(A.live_model), "price_in", 0) * tin + ed_json_num(model_row(A.live_model), "price_out", 0) * tout) / 1e6;
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
    if (A.http_status == 401 || !strcmp(code, "SignInRequired")) *signin = 1;
    if (o && ed_json_str(o, "message", NULL)) snprintf(out, n, "%s", ed_json_str(o, "message", ""));
    else {
        char e[240];
        ai_error_text(A.errbody.data, A.errbody.len, e, sizeof(e));
        snprintf(out, n, "HTTP %d%s%s", A.http_status, e[0] ? ": " : "", e);
    }
    cJSON_Delete(o);
}

static void hs_done(void *user, const int status, const char *error) {
    (void) user;
    char err[400] = "";
    int signin = 0;
    if (!status) snprintf(err, sizeof(err), "Cannot reach the AI service (%s)", error ? error : "network error");
    else if (A.http_status != 200 || !A.sse) error_from_body(err, sizeof(err), &signin);
    else {
        ai_stream_finish(&A.st);
        if (A.st.error[0]) snprintf(err, sizeof(err), "%s", A.st.error);
        else if (!A.st.done && !A.done && !A.st.text.len && !A.st.ncalls) snprintf(err, sizeof(err), "The answer ended early");
    }
    turn_commit(err[0] ? err : NULL);
    const char *finish = A.done ? ed_json_str(A.done, "finish", "") : A.st.finish;
    (void) finish;
    turn_reset();
    if (signin) set_banner(4, "Your session expired: sign in again in the Krunker Hub.");
    else if (err[0] && A.http_status == 429) set_banner(2, err);
    render_all();
    update_controls();
}

static cJSON *system_message(void) {
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "You are the AI assistant of the Krunker Box Office map editor (Krunker v7.1.4 maps). Answer briefly and concretely; "
                "use markdown. Coordinates are Krunker units with y up; an object's p is its bottom centre, s its size.\n");
    if (g_ed.proj) {
        const char *name = ed_json_str(g_ed.proj->map_config, "name", "");
        edb_addf(&b, "Open map: \"%s\", %zu objects (%d collidable, %d spawn points).\n", name[0] ? name : "untitled", g_ed.proj->count,
                 g_ed.proj->obj_count[1], g_ed.proj->obj_count[2]);
    }
    const AccountInfo *acc = account_get();
    if (acc) edb_addf(&b, "The user is signed in as %s.\n", acc->name);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "system");
    cJSON_AddStringToObject(m, "content", b.buf ? b.buf : "");
    edb_free(&b);
    return m;
}

// The request's messages: the system prompt + the conversation (failed placeholders left out,
// oldest turns dropped beyond the size budget).
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
    if (!brain_ready()) {
        set_banner(3, "AI needs the account service: this editor runs offline.");
        return;
    }
    turn_reset();
    ai_stream_init(&A.st, stream_delta, NULL);
    A.st.on_event = stream_event;
    A.tool_starts = cJSON_CreateArray();
    A.server_tools = cJSON_CreateObject();
    snprintf(A.live_model, sizeof(A.live_model), "%s", A.model);
    cJSON *body = cJSON_CreateObject();
    if (A.model[0]) cJSON_AddStringToObject(body, "model", A.model);
    cJSON_AddItemToObject(body, "messages", request_messages());
    cJSON_AddBoolToObject(body, "stream", 1);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char *url = brain_url("/ai/chat");
    char headers[400];
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", account_session_token());
    static const HttpStreamCallbacks cb = {hs_open, hs_data, hs_done};
    A.http_status = 0;
    A.sse = 0;
    A.started = g_ed.now;
    A.think_end = 0;
    A.hs = json && url ? http_stream_start("POST", url, headers, json, &cb, NULL) : NULL;
    memset(headers, 0, sizeof(headers));
    free(json);
    free(url);
    if (!A.hs) {
        set_banner(3, "Cannot start the request (this build has no TLS for an https:// account service?)");
        return;
    }
    A.streaming = 1;
    if (A.banner_kind == 3) set_banner(0, NULL);
    render_all();
    update_controls();
}

static void stop_turn(void) {
    if (!A.streaming) return;
    http_stream_cancel(A.hs);
    A.hs = NULL;
    ai_stream_finish(&A.st);
    turn_commit("Stopped");
    turn_reset();
    render_all();
    update_controls();
}

static void submit_text(const char *text) {
    if (!text || !text[0] || A.streaming) return;
    if (!signed_in()) {
        set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
        update_controls();
        return;
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(A.conv, m);
    send_turn();
    if (!A.streaming) render_all();
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
    else if (!brain_ready()) set_banner(3, "AI needs the account service: this editor runs offline.");
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
    if (A.streaming && A.live_dirty && g_ed.now - A.render_at >= AI_RENDER_EVERY_S) render_live();
}

void ed_ai_reset(void) {
    if (A.hs) http_stream_cancel(A.hs);
    turn_reset();
    cJSON_Delete(A.conv);
    cJSON_Delete(A.models);
    memset(&A, 0, sizeof(A));
}

int ed_ai_keydown(const HtmlEvent *ev) {
    (void) ev;
    return 0;
}

// ---------------------------------------------------------------------------
// handlers

static int h_toggle(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    ed_ai_toggle();
    return 1;
}

static int h_send(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    if (!A.input) return 1;
    const int len = html_dom_get_value(A.input, NULL, 0);
    char *text = len > 0 ? malloc((size_t) len + 1) : NULL;
    if (!text) return 1;
    html_dom_get_value(A.input, text, (size_t) len + 1);
    // trim
    char *s = text;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == '\t')) s[--l] = 0;
    if (s[0] && !A.streaming && signed_in()) {
        html_dom_set_value(A.input, "");
        submit_text(s);
    } else if (!signed_in()) {
        set_banner(4, "AI in the editor is free for Krunker Box Office accounts.");
    }
    free(text);
    return 1;
}

static int h_key(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
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

static int h_stop(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    stop_turn();
    return 1;
}

static int h_model(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    const char *v = ev && ev->value ? ev->value : "";
    if (!v[0] && A.model_sel) {
        static char buf[96];
        html_dom_get_value(A.model_sel, buf, sizeof(buf));
        v = buf;
    }
    if (v[0]) {
        snprintf(A.model, sizeof(A.model), "%s", v);
        html_storage_set("knAiModel", A.model);
    }
    return 1;
}

static int h_new(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    if (A.streaming) stop_turn();
    cJSON_Delete(A.conv);
    A.conv = cJSON_CreateArray();
    A.sess_in = A.sess_out = 0;
    A.sess_cost = 0;
    if (A.banner_kind == 3) set_banner(0, NULL);
    render_all();
    update_controls();
    return 1;
}

static int h_trace(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    if (n < 1) return 1;
    cJSON *m = cJSON_GetArrayItem(A.conv, (int) a[0].num);
    if (!m) return 1;
    cJSON *meta = meta_of(m);
    ed_json_set_bool(meta, "open", !ed_json_truthy(jget(meta, "open")));
    render_all();
    return 1;
}

static int h_tool(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    if (n < 2) return 1;
    cJSON *m = cJSON_GetArrayItem(A.conv, (int) a[0].num);
    const cJSON *call = cJSON_GetArrayItem(jget(m, "tool_calls"), (int) a[1].num);
    if (!m || !call) return 1;
    cJSON *meta = meta_of(m);
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(meta, "tools");
    if (!cJSON_IsObject(tools)) tools = cJSON_AddObjectToObject(meta, "tools");
    const char *id = ed_json_str(call, "id", "");
    cJSON *tm = cJSON_GetObjectItemCaseSensitive(tools, id);
    if (!tm) tm = cJSON_AddObjectToObject(tools, id);
    ed_json_set_bool(tm, "open", !ed_json_truthy(jget(tm, "open")));
    render_all();
    return 1;
}

static int h_copy(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    if (n < 1) return 1;
    const cJSON *m = cJSON_GetArrayItem(A.conv, (int) a[0].num);
    if (m) html_clipboard_write(ed_json_str(m, "content", ""));
    editor_message("Copied the answer.");
    return 1;
}

// Retry: drop everything after the user message before this reply and ask again
static int h_retry(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    if (n < 1 || A.streaming) return 1;
    int i = (int) a[0].num;
    while (i > 0 && strcmp(ed_json_str(cJSON_GetArrayItem(A.conv, i), "role", ""), "user")) i--;
    if (i < 0 || strcmp(ed_json_str(cJSON_GetArrayItem(A.conv, i), "role", ""), "user")) return 1;
    while (cJSON_GetArraySize(A.conv) > i + 1) cJSON_DeleteItemFromArray(A.conv, i + 1);
    send_turn();
    return 1;
}

static int h_signin(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    if (!app_route_url("/social.html")) editor_message("Sign in from the Krunker Hub.");
    return 1;
}

static int h_dismiss(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev; (void) a; (void) n;
    set_banner(0, NULL);
    health_banner();
    return 1;
}

void ed_ai_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.kn.aiToggle", h_toggle}, {"KE.kn.aiSend", h_send},   {"KE.kn.aiKey", h_key},       {"KE.kn.aiStop", h_stop},
        {"KE.kn.aiModel", h_model},   {"KE.kn.aiNew", h_new},     {"KE.kn.aiTrace", h_trace},   {"KE.kn.aiTool", h_tool},
        {"KE.kn.aiCopy", h_copy},     {"KE.kn.aiRetry", h_retry}, {"KE.kn.aiSignIn", h_signin}, {"KE.kn.aiDismiss", h_dismiss},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

// ---------------------------------------------------------------------------
// KRUNKNATIVE_EDITOR_SCRIPT "ai:..." (tools/editor/ai_e2e.py)
//   ai:open / ai:close, ai:model:<id>, ai:send:<text>, ai:stop, ai:new, ai:trace:<i>, ai:tool:<i>:<k>,
//   ai:dump:<path> (JSON: panel state, conversation, banner, models, rendered text)

static void dump_state(const char *path) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "open", A.open);
    cJSON_AddBoolToObject(o, "signed_in", signed_in());
    cJSON_AddBoolToObject(o, "streaming", A.streaming);
    cJSON_AddStringToObject(o, "model", A.model);
    cJSON_AddStringToObject(o, "health", A.health);
    cJSON_AddNumberToObject(o, "banner_kind", A.banner_kind);
    cJSON_AddStringToObject(o, "banner", A.banner_text);
    cJSON *ids = cJSON_AddArrayToObject(o, "models");
    const cJSON *m;
    cJSON_ArrayForEach(m, A.models) cJSON_AddItemToArray(ids, cJSON_CreateString(ed_json_str(m, "id", "")));
    cJSON_AddItemToObject(o, "conv", A.conv ? cJSON_Duplicate(A.conv, 1) : cJSON_CreateArray());
    char text[16384] = "";
    if (A.list) html_dom_get_text(A.list, text, sizeof(text));
    cJSON_AddStringToObject(o, "list_text", text);
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
        snprintf(A.model, sizeof(A.model), "%s", v);
        fill_models();
    } else if (IS("send")) {
        submit_text(v);
    } else if (IS("stop")) {
        stop_turn();
    } else if (IS("new")) {
        h_new(NULL, NULL, NULL, 0);
    } else if (IS("trace")) {
        HtmlArg x = {HTML_ARG_NUM, atof(v), NULL};
        h_trace(NULL, NULL, &x, 1);
    } else if (IS("tool")) {
        HtmlArg x[2] = {{HTML_ARG_NUM, atof(v), NULL}, {HTML_ARG_NUM, strchr(v, ':') ? atof(strchr(v, ':') + 1) : 0, NULL}};
        h_tool(NULL, NULL, x, 2);
    } else if (IS("dump")) {
        dump_state(v);
    } else {
        fprintf(stderr, "[editor] ai: unknown command %s\n", arg);
    }
#undef IS
    return 1;
}
