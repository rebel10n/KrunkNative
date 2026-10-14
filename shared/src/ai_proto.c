// OpenAI-compatible chat protocol (shared/include/ai_proto.h): request bodies for the
// chat-completions and responses styles, one SSE stream parser for both, /models normalising.
#include "ai_proto.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// small helpers

void ai_buf_add(AiBuf *b, const char *s, const size_t n) {
    if (!n) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void ai_buf_free(AiBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_clear(AiBuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, const double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        if (end && !*end) return d;
    }
    return def;
}

static const cJSON *jget(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o, k); }

static void copy(char *out, const size_t n, const char *s) { snprintf(out, n, "%s", s ? s : ""); }

AiApi ai_api_parse(const char *s, const AiApi def) {
    if (!s || !*s) return def;
    if (!strcasecmp(s, "responses") || !strcasecmp(s, "response")) return AI_API_RESPONSES;
    if (!strcasecmp(s, "chat") || !strcasecmp(s, "chat_completions") || !strcasecmp(s, "chat/completions") || !strcasecmp(s, "completions"))
        return AI_API_CHAT;
    return def;
}

const char *ai_api_name(const AiApi api) { return api == AI_API_RESPONSES ? "responses" : "chat"; }
const char *ai_api_path(const AiApi api) { return api == AI_API_RESPONSES ? "/responses" : "/chat/completions"; }

// ---------------------------------------------------------------------------
// request bodies

static cJSON *chat_messages(const cJSON *messages) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *m;
    cJSON_ArrayForEach(m, messages) {
        if (!cJSON_IsObject(m)) continue;
        cJSON *c = cJSON_Duplicate(m, 1);
        // our own bookkeeping never goes upstream
        cJSON_DeleteItemFromObjectCaseSensitive(c, "reasoning_content");
        cJSON_DeleteItemFromObjectCaseSensitive(c, "usage");
        cJSON_DeleteItemFromObjectCaseSensitive(c, "meta");
        const cJSON *content = jget(c, "content");
        if (!content || cJSON_IsNull(content)) {
            // assistant turns that only call tools: content null is valid; others need text
            if (!jget(c, "tool_calls")) {
                cJSON_DeleteItemFromObjectCaseSensitive(c, "content");
                cJSON_AddStringToObject(c, "content", "");
            }
        }
        cJSON_AddItemToArray(out, c);
    }
    return out;
}

static cJSON *responses_input(const cJSON *messages, char **instructions) {
    cJSON *out = cJSON_CreateArray();
    AiBuf sys = {0};
    const cJSON *m;
    cJSON_ArrayForEach(m, messages) {
        const char *role = jstr(m, "role");
        const char *content = jstr(m, "content");
        if (!role) continue;
        if (!strcmp(role, "system") || !strcmp(role, "developer")) {
            if (content) {
                if (sys.len) ai_buf_add(&sys, "\n\n", 2);
                ai_buf_add(&sys, content, strlen(content));
            }
            continue;
        }
        if (!strcmp(role, "tool")) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "type", "function_call_output");
            cJSON_AddStringToObject(it, "call_id", jstr(m, "tool_call_id") ? jstr(m, "tool_call_id") : "");
            cJSON_AddStringToObject(it, "output", content ? content : "");
            cJSON_AddItemToArray(out, it);
            continue;
        }
        if (content && content[0]) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "role", !strcmp(role, "assistant") ? "assistant" : "user");
            cJSON_AddStringToObject(it, "content", content);
            cJSON_AddItemToArray(out, it);
        }
        const cJSON *calls = jget(m, "tool_calls");
        const cJSON *tc;
        cJSON_ArrayForEach(tc, calls) {
            const cJSON *fn = jget(tc, "function");
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "type", "function_call");
            cJSON_AddStringToObject(it, "call_id", jstr(tc, "id") ? jstr(tc, "id") : "");
            cJSON_AddStringToObject(it, "name", jstr(fn, "name") ? jstr(fn, "name") : "");
            cJSON_AddStringToObject(it, "arguments", jstr(fn, "arguments") ? jstr(fn, "arguments") : "{}");
            cJSON_AddItemToArray(out, it);
        }
    }
    *instructions = sys.data; // NULL when none
    return out;
}

static cJSON *responses_tools(const cJSON *tools) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        const cJSON *fn = jget(t, "function");
        if (!fn) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "function");
        cJSON_AddStringToObject(o, "name", jstr(fn, "name") ? jstr(fn, "name") : "");
        if (jstr(fn, "description")) cJSON_AddStringToObject(o, "description", jstr(fn, "description"));
        const cJSON *p = jget(fn, "parameters");
        cJSON_AddItemToObject(o, "parameters", p ? cJSON_Duplicate(p, 1) : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
        cJSON_AddItemToArray(out, o);
    }
    return out;
}

char *ai_request_body(const AiApi api, const char *model, const cJSON *messages, const cJSON *tools, const char *effort,
                      const int stream, const int max_tokens) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", model ? model : "");
    const int has_tools = cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0;
    if (api == AI_API_RESPONSES) {
        char *instructions = NULL;
        cJSON_AddItemToObject(o, "input", responses_input(messages, &instructions));
        if (instructions) cJSON_AddStringToObject(o, "instructions", instructions);
        free(instructions);
        if (has_tools) cJSON_AddItemToObject(o, "tools", responses_tools(tools));
        if (effort && effort[0]) cJSON_AddStringToObject(cJSON_AddObjectToObject(o, "reasoning"), "effort", effort);
        if (max_tokens > 0) cJSON_AddNumberToObject(o, "max_output_tokens", max_tokens);
        cJSON_AddBoolToObject(o, "stream", stream != 0);
        cJSON_AddBoolToObject(o, "store", 0);
    } else {
        cJSON_AddItemToObject(o, "messages", chat_messages(messages));
        if (has_tools) cJSON_AddItemToObject(o, "tools", cJSON_Duplicate(tools, 1));
        if (effort && effort[0]) cJSON_AddStringToObject(o, "reasoning_effort", effort);
        if (max_tokens > 0) cJSON_AddNumberToObject(o, "max_tokens", max_tokens);
        cJSON_AddBoolToObject(o, "stream", stream != 0);
        if (stream) cJSON_AddBoolToObject(cJSON_AddObjectToObject(o, "stream_options"), "include_usage", 1);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// ---------------------------------------------------------------------------
// stream state

void ai_stream_init(AiStream *s, const AiDeltaFn on_delta, void *user) {
    memset(s, 0, sizeof(*s));
    s->api = -1;
    s->on_delta = on_delta;
    s->user = user;
}

void ai_stream_free(AiStream *s) {
    ai_buf_free(&s->line);
    ai_buf_free(&s->data);
    ai_buf_free(&s->text);
    ai_buf_free(&s->reasoning);
    ai_buf_free(&s->think_hold);
    for (int i = 0; i < AI_MAX_TOOL_CALLS; i++) ai_buf_free(&s->calls[i].args);
    s->ncalls = 0;
}

static void emit(AiStream *s, const int kind, const char *t, const size_t n) {
    if (!n) return;
    ai_buf_add(kind == AI_DELTA_REASONING ? &s->reasoning : &s->text, t, n);
    if (s->on_delta) s->on_delta(s->user, kind, t, n);
}

static void emit_reasoning(AiStream *s, const char *t) {
    if (t && *t) emit(s, AI_DELTA_REASONING, t, strlen(t));
}

// content text, with a leading <think>...</think> block routed to the reasoning channel
static const char k_open[] = "<think>", k_close[] = "</think>";

static void content_piece(AiStream *s, const char *t, size_t n);

static void think_resolve_start(AiStream *s) {
    AiBuf *h = &s->think_hold;
    size_t i = 0;
    while (i < h->len && isspace((unsigned char) h->data[i])) i++;
    const size_t rest = h->len - i;
    if (rest < sizeof(k_open) - 1 && !strncmp(h->data + i, k_open, rest)) return; // undecided
    if (rest >= sizeof(k_open) - 1 && !strncmp(h->data + i, k_open, sizeof(k_open) - 1)) {
        s->think_state = 1;
        char *after = strdup(h->data + i + sizeof(k_open) - 1);
        buf_clear(h);
        if (after) {
            content_piece(s, after, strlen(after));
            free(after);
        }
        return;
    }
    s->think_state = 2;
    char *all = h->data ? strdup(h->data) : NULL;
    buf_clear(h);
    if (all) {
        emit(s, AI_DELTA_TEXT, all, strlen(all));
        free(all);
    }
}

static void content_piece(AiStream *s, const char *t, const size_t n) {
    if (!n) return;
    if (s->think_state == 2) {
        emit(s, AI_DELTA_TEXT, t, n);
        return;
    }
    ai_buf_add(&s->think_hold, t, n);
    if (s->think_state == 0) {
        think_resolve_start(s);
        return;
    }
    // inside <think>
    AiBuf *h = &s->think_hold;
    char *end = h->data ? strstr(h->data, k_close) : NULL;
    if (end) {
        emit(s, AI_DELTA_REASONING, h->data, (size_t) (end - h->data));
        const char *after = end + sizeof(k_close) - 1;
        while (*after == '\n' || *after == '\r' || *after == ' ') after++;
        char *rest = strdup(after);
        buf_clear(h);
        s->think_state = 2;
        if (rest) {
            emit(s, AI_DELTA_TEXT, rest, strlen(rest));
            free(rest);
        }
        return;
    }
    const size_t keep = sizeof(k_close) - 2; // a partial "</think" may follow
    if (h->len > keep) {
        const size_t out = h->len - keep;
        emit(s, AI_DELTA_REASONING, h->data, out);
        memmove(h->data, h->data + out, keep + 1);
        h->len = keep;
    }
}

static AiToolCall *call_at(AiStream *s, const int index) {
    if (index < 0 || index >= AI_MAX_TOOL_CALLS) return NULL;
    while (s->ncalls <= index) memset(&s->calls[s->ncalls++], 0, sizeof(AiToolCall));
    return &s->calls[index];
}

static AiToolCall *call_by_item(AiStream *s, const char *item_id) {
    for (int i = 0; item_id && i < s->ncalls; i++) {
        if (!strcmp(s->calls[i].item_id, item_id) || !strcmp(s->calls[i].id, item_id)) return &s->calls[i];
    }
    return NULL;
}

static void tool_delta(AiStream *s, const AiToolCall *c) {
    if (s->on_delta) s->on_delta(s->user, AI_DELTA_TOOL, c->name, strlen(c->name));
}

static void read_usage(AiStream *s, const cJSON *u) {
    if (!cJSON_IsObject(u)) return;
    s->has_usage = 1;
    // chat: prompt/completion_tokens; responses: input/output_tokens
    s->prompt_tokens = (int64_t) jnum(u, "prompt_tokens", jnum(u, "input_tokens", (double) s->prompt_tokens));
    s->completion_tokens = (int64_t) jnum(u, "completion_tokens", jnum(u, "output_tokens", (double) s->completion_tokens));
    const cJSON *cd = jget(u, "completion_tokens_details");
    if (!cd) cd = jget(u, "output_tokens_details");
    if (cd) s->reasoning_tokens = (int64_t) jnum(cd, "reasoning_tokens", (double) s->reasoning_tokens);
    const cJSON *pd = jget(u, "prompt_tokens_details");
    if (!pd) pd = jget(u, "input_tokens_details");
    if (pd) s->cached_tokens = (int64_t) jnum(pd, "cached_tokens", (double) s->cached_tokens);
}

static void read_error(AiStream *s, const cJSON *o) {
    const cJSON *e = jget(o, "error");
    const char *msg = cJSON_IsString(e) ? e->valuestring : jstr(e, "message");
    if (!msg) msg = jstr(o, "message");
    copy(s->error, sizeof(s->error), msg ? msg : "upstream error");
}

// one chat.completion.chunk
static void chat_chunk(AiStream *s, const cJSON *o) {
    if (jget(o, "error")) {
        read_error(s, o);
        return;
    }
    if (jstr(o, "model")) copy(s->model, sizeof(s->model), jstr(o, "model"));
    read_usage(s, jget(o, "usage"));
    const cJSON *ch = cJSON_GetArrayItem(jget(o, "choices"), 0);
    if (!ch) return;
    const cJSON *d = jget(ch, "delta");
    if (!d) d = jget(ch, "message"); // a non-streamed answer
    const char *fr = jstr(ch, "finish_reason");
    if (fr) copy(s->finish, sizeof(s->finish), fr);
    if (!d) return;
    emit_reasoning(s, jstr(d, "reasoning_content"));
    emit_reasoning(s, jstr(d, "reasoning"));
    const char *c = jstr(d, "content");
    if (c) content_piece(s, c, strlen(c));
    const cJSON *tc;
    int auto_index = 0;
    cJSON_ArrayForEach(tc, jget(d, "tool_calls")) {
        const cJSON *ix = jget(tc, "index");
        AiToolCall *call = call_at(s, cJSON_IsNumber(ix) ? ix->valueint : (jstr(tc, "id") ? s->ncalls : auto_index));
        auto_index++;
        if (!call) continue;
        if (jstr(tc, "id") && jstr(tc, "id")[0]) copy(call->id, sizeof(call->id), jstr(tc, "id"));
        const cJSON *fn = jget(tc, "function");
        const char *name = jstr(fn, "name");
        if (name && name[0]) {
            copy(call->name, sizeof(call->name), name);
            tool_delta(s, call);
        }
        const char *args = jstr(fn, "arguments");
        if (args) ai_buf_add(&call->args, args, strlen(args));
    }
}

static void responses_item(AiStream *s, const cJSON *item, const int done) {
    const char *type = jstr(item, "type");
    if (!type || strcmp(type, "function_call")) return;
    AiToolCall *c = call_by_item(s, jstr(item, "id"));
    if (!c) c = call_by_item(s, jstr(item, "call_id"));
    if (!c) {
        c = call_at(s, s->ncalls);
        if (!c) return;
        copy(c->item_id, sizeof(c->item_id), jstr(item, "id"));
    }
    copy(c->id, sizeof(c->id), jstr(item, "call_id") ? jstr(item, "call_id") : jstr(item, "id"));
    if (jstr(item, "name")) copy(c->name, sizeof(c->name), jstr(item, "name"));
    const char *args = jstr(item, "arguments");
    if (args && (done || !c->args.len) && args[0]) {
        buf_clear(&c->args);
        ai_buf_add(&c->args, args, strlen(args));
    }
    if (!done) tool_delta(s, c);
}

// one responses event (type = the event name)
static void responses_event(AiStream *s, const char *type, const cJSON *o) {
    if (!strcmp(type, "response.output_text.delta") || !strcmp(type, "response.refusal.delta")) {
        const char *d = jstr(o, "delta");
        if (d) content_piece(s, d, strlen(d));
    } else if (!strcmp(type, "response.reasoning_text.delta") || !strcmp(type, "response.reasoning_summary_text.delta") ||
               !strcmp(type, "response.reasoning.delta")) {
        emit_reasoning(s, jstr(o, "delta"));
    } else if (!strcmp(type, "response.output_item.added")) {
        responses_item(s, jget(o, "item"), 0);
    } else if (!strcmp(type, "response.output_item.done")) {
        responses_item(s, jget(o, "item"), 1);
    } else if (!strcmp(type, "response.function_call_arguments.delta")) {
        AiToolCall *c = call_by_item(s, jstr(o, "item_id"));
        if (!c) {
            c = call_at(s, s->ncalls);
            if (c) copy(c->item_id, sizeof(c->item_id), jstr(o, "item_id"));
        }
        const char *d = jstr(o, "delta");
        if (c && d) ai_buf_add(&c->args, d, strlen(d));
    } else if (!strcmp(type, "response.function_call_arguments.done")) {
        AiToolCall *c = call_by_item(s, jstr(o, "item_id"));
        const char *a = jstr(o, "arguments");
        if (c && a) {
            buf_clear(&c->args);
            ai_buf_add(&c->args, a, strlen(a));
        }
    } else if (!strcmp(type, "response.created") || !strcmp(type, "response.in_progress")) {
        const cJSON *r = jget(o, "response");
        if (jstr(r, "model")) copy(s->model, sizeof(s->model), jstr(r, "model"));
    } else if (!strcmp(type, "response.completed") || !strcmp(type, "response.incomplete") || !strcmp(type, "response.failed")) {
        const cJSON *r = jget(o, "response");
        read_usage(s, jget(r, "usage"));
        if (jstr(r, "model")) copy(s->model, sizeof(s->model), jstr(r, "model"));
        const char *st = jstr(r, "status");
        if (!strcmp(type, "response.incomplete")) copy(s->finish, sizeof(s->finish), "length");
        else if (!strcmp(type, "response.failed")) {
            const cJSON *e = jget(r, "error");
            copy(s->error, sizeof(s->error), jstr(e, "message") ? jstr(e, "message") : "the response failed");
        } else copy(s->finish, sizeof(s->finish), s->ncalls ? "tool_calls" : st && strcmp(st, "completed") ? st : "stop");
        s->done = 1;
    } else if (!strcmp(type, "error")) {
        read_error(s, o);
    }
}

static void dispatch(AiStream *s) {
    if (!s->data.len) {
        s->event[0] = 0;
        return;
    }
    const char *d = s->data.data;
    if (!strcmp(d, "[DONE]")) {
        s->done = 1;
    } else {
        cJSON *o = cJSON_Parse(d);
        if (o && !strncmp(s->event, "kn.", 3)) {
            if (s->on_event) s->on_event(s->user, s->event, o);
        } else if (o) {
            const char *type = s->event[0] ? s->event : jstr(o, "type");
            if (s->api < 0) s->api = type && !strncmp(type, "response", 8) ? AI_API_RESPONSES : AI_API_CHAT;
            if (type && (!strncmp(type, "response.", 9) || !strcmp(type, "error"))) responses_event(s, type, o);
            else chat_chunk(s, o);
            cJSON_Delete(o);
        }
    }
    s->events++;
    buf_clear(&s->data);
    s->event[0] = 0;
}

static void sse_line(AiStream *s, char *line, size_t n) {
    if (n && line[n - 1] == '\r') line[--n] = 0;
    if (!n) {
        dispatch(s);
        return;
    }
    if (line[0] == ':') return; // comment / keep-alive
    char *colon = memchr(line, ':', n);
    const char *field = line, *value = "";
    size_t vlen = 0;
    if (colon) {
        *colon = 0;
        value = colon + 1;
        if (*value == ' ') value++;
        vlen = n - (size_t) (value - line);
    }
    if (!strcmp(field, "data")) {
        if (s->data.len) ai_buf_add(&s->data, "\n", 1);
        ai_buf_add(&s->data, value, vlen);
        if (!s->data.data) ai_buf_add(&s->data, "", 0);
    } else if (!strcmp(field, "event")) {
        snprintf(s->event, sizeof(s->event), "%.*s", (int) vlen, value);
    }
}

int ai_stream_feed(AiStream *s, const char *bytes, const size_t n) {
    const int before = s->events;
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] == '\n') {
            ai_buf_add(&s->line, "", 0);
            sse_line(s, s->line.data ? s->line.data : (char *) "", s->line.len);
            buf_clear(&s->line);
        } else {
            // runs of plain bytes in one go
            size_t j = i;
            while (j < n && bytes[j] != '\n') j++;
            ai_buf_add(&s->line, bytes + i, j - i);
            i = j - 1;
        }
    }
    return s->events - before;
}

void ai_stream_finish(AiStream *s) {
    if (s->line.len) {
        char *l = s->line.data;
        sse_line(s, l, s->line.len);
        buf_clear(&s->line);
    }
    dispatch(s);
    if (s->think_hold.len) {
        char *h = strdup(s->think_hold.data);
        const int kind = s->think_state == 1 ? AI_DELTA_REASONING : AI_DELTA_TEXT;
        buf_clear(&s->think_hold);
        if (h) {
            emit(s, kind, h, strlen(h));
            free(h);
        }
    }
    s->think_state = 2;
    if (!s->finish[0] && !s->error[0]) copy(s->finish, sizeof(s->finish), s->ncalls ? "tool_calls" : "stop");
}

int ai_parse_response(AiStream *s, const char *json, const size_t n) {
    cJSON *o = cJSON_ParseWithLength(json, n);
    if (!o) return 0;
    if (jget(o, "error") && !jget(o, "choices") && !jget(o, "output")) {
        read_error(s, o);
    } else if (jget(o, "output")) {
        s->api = AI_API_RESPONSES;
        const cJSON *item;
        cJSON_ArrayForEach(item, jget(o, "output")) {
            const char *type = jstr(item, "type");
            if (type && !strcmp(type, "message")) {
                const cJSON *part;
                cJSON_ArrayForEach(part, jget(item, "content")) {
                    const char *t = jstr(part, "text");
                    if (t) content_piece(s, t, strlen(t));
                }
            } else if (type && !strcmp(type, "reasoning")) {
                const cJSON *part;
                cJSON_ArrayForEach(part, jget(item, "summary")) emit_reasoning(s, jstr(part, "text"));
                cJSON_ArrayForEach(part, jget(item, "content")) emit_reasoning(s, jstr(part, "text"));
            } else {
                responses_item(s, item, 1);
            }
        }
        read_usage(s, jget(o, "usage"));
        if (jstr(o, "model")) copy(s->model, sizeof(s->model), jstr(o, "model"));
        copy(s->finish, sizeof(s->finish), s->ncalls ? "tool_calls" : "stop");
    } else {
        s->api = AI_API_CHAT;
        chat_chunk(s, o);
    }
    cJSON_Delete(o);
    ai_stream_finish(s);
    return 1;
}

cJSON *ai_stream_message(const AiStream *s) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "assistant");
    cJSON_AddStringToObject(m, "content", s->text.data ? s->text.data : "");
    if (s->reasoning.len) cJSON_AddStringToObject(m, "reasoning_content", s->reasoning.data);
    if (s->ncalls) {
        cJSON *calls = cJSON_AddArrayToObject(m, "tool_calls");
        for (int i = 0; i < s->ncalls; i++) {
            const AiToolCall *c = &s->calls[i];
            if (!c->name[0]) continue;
            cJSON *tc = cJSON_CreateObject();
            char id[96];
            if (c->id[0]) copy(id, sizeof(id), c->id);
            else snprintf(id, sizeof(id), "call_%d", i);
            cJSON_AddStringToObject(tc, "id", id);
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_AddObjectToObject(tc, "function");
            cJSON_AddStringToObject(fn, "name", c->name);
            cJSON_AddStringToObject(fn, "arguments", c->args.len ? c->args.data : "{}");
            cJSON_AddItemToArray(calls, tc);
        }
    }
    return m;
}

cJSON *ai_stream_usage(const AiStream *s) {
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "prompt_tokens", (double) s->prompt_tokens);
    cJSON_AddNumberToObject(u, "completion_tokens", (double) s->completion_tokens);
    if (s->reasoning_tokens) cJSON_AddNumberToObject(u, "reasoning_tokens", (double) s->reasoning_tokens);
    if (s->cached_tokens) cJSON_AddNumberToObject(u, "cached_tokens", (double) s->cached_tokens);
    return u;
}

// ---------------------------------------------------------------------------
// /models

// Capability ranking of the model picker: smartest first (the editor's AI panel lists the models in
// this order). Source: a hand-maintained table of the families the provider serves, ordered by the
// maintainers from public leaderboards (Artificial Analysis Intelligence Index and LMArena text,
// as of 2026-10) and the provider's own tiering (pro > base > flash > mini; more parameters first
// within a family). The longest matching prefix of the id (its last path segment, case-insensitive)
// wins. Ids the table does not know get a score from their metadata (ai_model_rank): parameter
// count from the id ("-70b"), price as a proxy of size, reasoning and tool support; capped below
// the explicitly ranked frontier models. Update the table when the provider adds families.
static const struct {
    const char *prefix;
    int score;
} k_model_rank[] = {
    {"deepseek-v4-pro", 100}, {"kimi-k2.7", 97},         {"kimi-k2", 96},          {"glm-5.3", 94},
    {"glm-5.2", 92},          {"glm-5", 90},             {"deepseek-v4-flash", 89}, {"glm-5.3-flash", 88},
    {"gpt-oss-120b", 86},     {"nemotron-3-120b", 84},   {"qwen3.8", 82},          {"llama-4-maverick", 80},
    {"llama-3.3-70b", 78},    {"gemma-4", 76},           {"qwen3", 74},            {"glm-4.7", 72},
    {"llama-4-scout", 70},    {"gpt-oss-20b", 68},       {"qwq-32b", 66},          {"deepseek-r1-distill", 64},
    {"mistral-small", 62},    {"qwen2.5-coder", 60},     {"gemma-sea-lion", 56},   {"llama-3.2-11b", 50},
    {"llama-3.1-8b", 46},     {"granite-4", 44},         {"mistral-7b", 40},       {"gemma-7b", 38},
    {"llama-2-7b", 36},       {"llama-3.2-3b", 34},      {"gemma-2b", 30},         {"llama-3.2-1b", 28},
    {"llama-guard", 10}, // a safety classifier, not a chat model
};

static int has_ci(const char *s, const char *needle) { // strcasestr is not portable (MinGW)
    const size_t n = strlen(needle);
    for (; *s; s++) {
        if (!strncasecmp(s, needle, n)) return 1;
    }
    return 0;
}

static double log2_approx(double x) { // x >= 1; no libm in the shared core
    double l = 0;
    while (x >= 2) {
        x /= 2;
        l += 1;
    }
    return l + (x - 1);
}

// "-70b" / "_8b" / "-1.5b" in the id: billions of parameters (0 = none)
static double id_params_b(const char *id) {
    for (const char *p = id; *p; p++) {
        if ((*p != '-' && *p != '_' && *p != ':') || !isdigit((unsigned char) p[1])) continue;
        char *end = NULL;
        const double v = strtod(p + 1, &end);
        if (end && (*end == 'b' || *end == 'B') && (!end[1] || end[1] == '-' || end[1] == '_' || end[1] == ':')) return v;
    }
    return 0;
}

int ai_model_rank(const cJSON *row) {
    const char *full = jstr(row, "id") ? jstr(row, "id") : "";
    const char *id = strrchr(full, '/') ? strrchr(full, '/') + 1 : full;
    size_t best = 0;
    int score = -1;
    for (size_t i = 0; i < sizeof(k_model_rank) / sizeof(k_model_rank[0]); i++) {
        const size_t n = strlen(k_model_rank[i].prefix);
        if (n > best && !strncasecmp(id, k_model_rank[i].prefix, n)) {
            best = n;
            score = k_model_rank[i].score;
        }
    }
    if (score >= 0) return score;
    // unknown: metadata
    double s;
    const double params = id_params_b(id), price_out = jnum(row, "price_out", -1);
    if (params >= 1) s = 24 + 7 * log2_approx(params);       // 1B 24, 8B 45, 70B 67, 400B 84
    else if (params > 0) s = 20;
    else if (price_out > 0) s = 36 + 6 * log2_approx(1 + price_out * 2); // $0.3 40, $1.5 48, $4 55, $15 66
    else s = 30;
    const cJSON *r = jget(row, "reasoning"), *t = jget(row, "tools");
    if (cJSON_IsTrue(r)) s += 4;
    if (cJSON_IsTrue(t)) s += 2;
    if (has_ci(id, "lora") || has_ci(id, "guard") || has_ci(id, "embed") || has_ci(id, "whisper") || has_ci(id, "tts"))
        s -= 15; // adapters, classifiers, speech / embedding models
    if (s > 89) s = 89;
    if (s < 1) s = 1;
    return (int) s;
}

// smartest first; ties: the dearer output (a bigger model), then the id
static int cmp_rank(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *) a, *y = *(const cJSON *const *) b;
    const int rx = ai_model_rank(x), ry = ai_model_rank(y);
    if (rx != ry) return ry - rx;
    const double px = jnum(x, "price_out", -1), py = jnum(y, "price_out", -1);
    if (px != py) return px < py ? 1 : -1;
    return strcmp(jstr(x, "id") ? jstr(x, "id") : "", jstr(y, "id") ? jstr(y, "id") : "");
}

cJSON *ai_models_normalise(const char *json, const size_t n) {
    cJSON *o = cJSON_ParseWithLength(json, n);
    if (!o) return NULL;
    const cJSON *list = cJSON_IsArray(o) ? o : jget(o, "data");
    if (!cJSON_IsArray(list)) list = jget(o, "models");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(o);
        return NULL;
    }
    const int count = cJSON_GetArraySize(list);
    cJSON **items = calloc((size_t) (count > 0 ? count : 1), sizeof(cJSON *));
    int k = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, list) {
        const char *id = jstr(m, "id");
        if (!id || !id[0] || !items) continue;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "id", id);
        const char *name = jstr(m, "display_name");
        if (!name) name = jstr(m, "name");
        cJSON_AddStringToObject(r, "name", name && name[0] ? name : id);
        const double ctx = jnum(m, "max_input_tokens", jnum(m, "context_length", jnum(m, "context_window", 0)));
        if (ctx > 0) cJSON_AddNumberToObject(r, "context", ctx);
        const double mt = jnum(m, "max_tokens", jnum(m, "max_output_tokens", 0));
        if (mt > 0) cJSON_AddNumberToObject(r, "max_tokens", mt);
        const cJSON *reasoning = jget(m, "reasoning");
        if (!cJSON_IsBool(reasoning)) reasoning = jget(m, "supports_reasoning");
        if (cJSON_IsBool(reasoning)) cJSON_AddBoolToObject(r, "reasoning", cJSON_IsTrue(reasoning));
        const cJSON *tools = jget(m, "toolCalling");
        if (!cJSON_IsBool(tools)) tools = jget(m, "supports_tools");
        if (!cJSON_IsBool(tools)) tools = jget(m, "tool_calling");
        if (cJSON_IsBool(tools)) cJSON_AddBoolToObject(r, "tools", cJSON_IsTrue(tools));
        const cJSON *vision = jget(m, "vision");
        if (cJSON_IsBool(vision) && cJSON_IsTrue(vision)) cJSON_AddBoolToObject(r, "vision", 1);
        const cJSON *efforts = jget(m, "supported_reasoning_efforts");
        if (cJSON_IsArray(efforts) && cJSON_GetArraySize(efforts) > 0) {
            cJSON *e = cJSON_AddArrayToObject(r, "efforts");
            const cJSON *it;
            cJSON_ArrayForEach(it, efforts) {
                if (cJSON_IsString(it)) cJSON_AddItemToArray(e, cJSON_CreateString(it->valuestring));
            }
        }
        const cJSON *pr = jget(m, "pricing");
        if (cJSON_IsObject(pr)) {
            double in = jnum(pr, "input_per_mtok", -1), out = jnum(pr, "output_per_mtok", -1);
            if (in < 0 && jget(pr, "prompt")) in = jnum(pr, "prompt", -1) * 1e6; // per-token prices (OpenRouter)
            if (out < 0 && jget(pr, "completion")) out = jnum(pr, "completion", -1) * 1e6;
            if (in >= 0) cJSON_AddNumberToObject(r, "price_in", in);
            if (out >= 0) cJSON_AddNumberToObject(r, "price_out", out);
            const char *cur = jstr(pr, "currency");
            cJSON_AddStringToObject(r, "currency", cur && cur[0] ? cur : "USD");
        }
        const cJSON *mods = jget(jget(m, "architecture"), "input_modalities");
        const cJSON *mo;
        cJSON_ArrayForEach(mo, mods) {
            if (cJSON_IsString(mo) && !strcmp(mo->valuestring, "image") && !jget(r, "vision")) cJSON_AddBoolToObject(r, "vision", 1);
        }
        const char *desc = jstr(m, "description");
        if (desc && desc[0]) {
            char clip[241];
            snprintf(clip, sizeof(clip), "%s", desc);
            cJSON_AddStringToObject(r, "description", clip);
        }
        items[k++] = r;
    }
    qsort(items, (size_t) k, sizeof(cJSON *), cmp_rank);
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < k; i++) cJSON_AddItemToArray(out, items[i]);
    free(items);
    cJSON_Delete(o);
    return out;
}

char *ai_error_text(const char *body, const size_t len, char *out, const size_t n) {
    out[0] = 0;
    if (!body || !len) return out;
    cJSON *o = cJSON_ParseWithLength(body, len);
    if (o) {
        const cJSON *e = jget(o, "error");
        const char *msg = cJSON_IsString(e) ? e->valuestring : jstr(e, "message");
        if (!msg) msg = jstr(o, "message");
        if (!msg) msg = jstr(o, "detail");
        if (msg) snprintf(out, n, "%s", msg);
        cJSON_Delete(o);
        if (out[0]) return out;
    }
    snprintf(out, n, "%.*s", (int) (len < 300 ? len : 300), body);
    for (char *p = out; *p; p++) {
        if (*p == '\n' || *p == '\r') *p = ' ';
    }
    return out;
}

// ---------------------------------------------------------------------------
// kn_brain's stream (chat.completion.chunk SSE events)

static char *sse_data(cJSON *o) {
    char *j = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!j) return NULL;
    const size_t n = strlen(j) + 9;
    char *out = malloc(n);
    if (out) snprintf(out, n, "data: %s\n\n", j);
    free(j);
    return out;
}

static cJSON *chunk_with_delta(cJSON **delta) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "object", "chat.completion.chunk");
    cJSON *ch = cJSON_CreateObject();
    cJSON_AddNumberToObject(ch, "index", 0);
    *delta = cJSON_AddObjectToObject(ch, "delta");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(o, "choices"), ch);
    return o;
}

char *ai_sse_delta(const int kind, const char *text, const size_t len) {
    if (!text || !len) return NULL;
    char *t = malloc(len + 1);
    if (!t) return NULL;
    memcpy(t, text, len);
    t[len] = 0;
    cJSON *delta;
    cJSON *o = chunk_with_delta(&delta);
    cJSON_AddStringToObject(delta, kind == AI_DELTA_REASONING ? "reasoning_content" : "content", t);
    free(t);
    return sse_data(o);
}

char *ai_sse_tool_calls(const AiStream *s) {
    if (!s->ncalls) return NULL;
    cJSON *delta;
    cJSON *o = chunk_with_delta(&delta);
    cJSON *calls = cJSON_AddArrayToObject(delta, "tool_calls");
    for (int i = 0; i < s->ncalls; i++) {
        const AiToolCall *c = &s->calls[i];
        if (!c->name[0]) continue;
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddNumberToObject(tc, "index", i);
        char id[96];
        if (c->id[0]) copy(id, sizeof(id), c->id);
        else snprintf(id, sizeof(id), "call_%d", i);
        cJSON_AddStringToObject(tc, "id", id);
        cJSON_AddStringToObject(tc, "type", "function");
        cJSON *fn = cJSON_AddObjectToObject(tc, "function");
        cJSON_AddStringToObject(fn, "name", c->name);
        cJSON_AddStringToObject(fn, "arguments", c->args.len ? c->args.data : "{}");
        cJSON_AddItemToArray(calls, tc);
    }
    return sse_data(o);
}

char *ai_sse_finish(const char *finish, const cJSON *usage, const char *model) {
    cJSON *delta;
    cJSON *o = chunk_with_delta(&delta);
    cJSON_AddStringToObject(cJSON_GetArrayItem(jget(o, "choices"), 0), "finish_reason", finish && finish[0] ? finish : "stop");
    if (usage) cJSON_AddItemToObject(o, "usage", cJSON_Duplicate(usage, 1));
    if (model && model[0]) cJSON_AddStringToObject(o, "model", model);
    return sse_data(o);
}

char *ai_sse_event(const char *event, const cJSON *data) {
    char *j = data ? cJSON_PrintUnformatted(data) : NULL;
    const size_t n = strlen(event) + (j ? strlen(j) : 2) + 18;
    char *out = malloc(n);
    if (out) snprintf(out, n, "event: %s\ndata: %s\n\n", event, j ? j : "{}");
    free(j);
    return out;
}

char *ai_sse_error(const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddStringToObject(e, "message", message && message[0] ? message : "error");
    cJSON_AddStringToObject(e, "code", code && code[0] ? code : "Error");
    char *out = ai_sse_event("error", o);
    cJSON_Delete(o);
    return out;
}

double ai_usage_cost(const cJSON *model_row, const int64_t prompt_tokens, const int64_t completion_tokens) {
    const double in = jnum(model_row, "price_in", -1), out = jnum(model_row, "price_out", -1);
    if (in < 0 && out < 0) return -1;
    return ((in > 0 ? in : 0) * (double) prompt_tokens + (out > 0 ? out : 0) * (double) completion_tokens) / 1e6;
}
