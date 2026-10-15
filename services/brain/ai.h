/*
 * services/brain/ai.h - the brain's AI proxy for the map editor (services/brain/ai.c).
 *
 * The client NEVER holds the upstream key: it asks the brain, which injects the credentials
 * server-side and relays the stream. Routes (public listener; each also under /api/ai/... for
 * proxies that only pass /api/):
 *
 *   GET  /ai/models    {success, models:[...], default, health, limits, server_tools}
 *                      (public: the panel lists models before sign-in; no secrets in it)
 *   GET  /ai/status    {enabled, configured, health, limits}
 *   POST /ai/chat      {model, messages, tools?, effort?, max_tokens?, stream?(default true),
 *                       web_search?} -> SSE (shared/include/ai_proto.h "kn_brain's own stream"),
 *                      or one JSON answer with "stream": false
 *
 * Auth (POST /ai/chat): the normal session token (body "token", query "token" or
 * "Authorization: Bearer"), resolved through svc-accounts. No or an expired session ->
 * 401 {"error":"SignInRequired","message":"Sign in to use AI"}. Signed-in accounts get a
 * generous per-account budget (ai.limit.*; 429 {"error":"RateLimited"} beyond it).
 *
 * Upstreams are named provider profiles (ai_provider.h). Each consumer routes to one:
 *   ai.editor.provider      the editor's chat (the /ai/ routes) default "default"
 *   ai.banter.provider      bot chat lines (bot-banter)         default "default"
 *   ai.moderation.provider  avatar review (ai_moderate_image)    default "default"
 *   ai.providers.<name>.url / .key      "https://host/v1" and the Bearer key (never logged, never in
 *                           an answer or a dump); KN_AI_<NAME>_URL / KN_AI_<NAME>_KEY override them
 *   ai.providers.<name>.model           the profile's model ("" = the consumer's choice; the editor's
 *                           default model when set, else ai.default_model)
 *   ai.providers.<name>.rpm             requests per minute over the profile's consumers (0 = none)
 *   ai.providers.<name>.daily_budget_tokens  prompt + completion tokens per UTC day (0 = none)
 *   "default" is the legacy upstream: ai.upstream_url / ai.upstream_key, KN_AI_UPSTREAM_URL /
 *   KN_AI_UPSTREAM_KEY (KN_AI_DEFAULT_URL / _KEY and ai.providers.default.* take precedence). A
 *   config with only those keeps working. Counters: /ai/status "providers" and "consumers".
 *   ai.enabled              (bool, default 1)   off = 503 {"error":"AIUnavailable"}
 *   ai.upstream_url         KN_AI_UPSTREAM_URL  the "default" profile's OpenAI-compatible base (".../v1")
 *   ai.upstream_key         KN_AI_UPSTREAM_KEY  the "default" profile's Bearer key (never logged, never sent to a client)
 *   ai.api                  "chat" (default) | "responses"
 *   ai.default_model        (default glm-5.3-flash)
 *   ai.models_hide          ids left out of /ai/models (list)
 *   ai.max_tokens / ai.timeout_ms / ai.connect_timeout_ms / ai.max_messages
 *   ai.models_ttl_s         /ai/models cache lifetime
 *   ai.health_interval_s    known-prompt probe period (0 = only at start)
 *   ai.limit.rpm / ai.limit.daily / ai.limit.daily_tokens / ai.limit.concurrent
 *   ai.banter.enabled       (bool, default 1)   bot chat lines for game servers (`bot-banter`)
 *   ai.banter.rpm           brain-wide bot lines per minute (default 120; beyond: "rate limited")
 *   ai.banter.max_tokens    output cap of a bot line request (default 200; the line is clipped to 70 chars anyway)
 *   ai.banter.timeout_ms    (default 8000)
 *
 * Health: at start and every ai.health_interval_s the brain sends the default model a prompt
 * with a random token and checks the answer echoes it. "degraded" = the upstream answers but
 * drops or rewrites request content (the editor shows "AI service degraded").
 */
#ifndef KN_BRAIN_AI_H
#define KN_BRAIN_AI_H

#include "brain.h"
#include "ai_provider.h"

int ai_init(Brain *b);
void ai_shutdown(Brain *b);

/* 1 when the proxy has an upstream URL + key; 0 when it is unconfigured (routes answer 503). */
int ai_configured(void);

/* Health probe state: AI_HEALTH_* and a short human-readable reason ("" when healthy). */
enum { AI_HEALTH_UNKNOWN = 0, AI_HEALTH_OK = 1, AI_HEALTH_DEGRADED = 2, AI_HEALTH_DOWN = 3, AI_HEALTH_UNCONFIGURED = 4 };
int ai_health(void);
const char *ai_health_reason(void);

/* {state, reason, checked_at, latency_ms, model} (public fields only, never the key). Caller frees. */
cJSON *ai_health_json(void);

/*
 * Server-side tools (the "tool bridge"): tools the brain runs itself, in the middle of a
 * /ai/chat stream, without a round trip to the editor (web search: ai_search.c). When the
 * model calls only server tools, the brain runs them, appends the results and asks the model
 * again; calls of client tools (the editor's) end the stream with finish_reason "tool_calls".
 * Every call is announced to the client: kn.server_tool {id, name, arguments, status:"running"},
 * then {id, name, status:"done"|"error", result, ms}.
 */
typedef void (*AiToolDoneFn)(void *ud, int is_error, const char *result);
typedef struct AiServerTools {
    const char *name;                       /* provider, for logs ("web") */
    cJSON *(*defs)(void);                   /* chat-shape [{type:"function",function:{...}}]; NULL = not ready. Caller frees */
    int (*owns)(const char *tool_name);
    void (*call)(const char *tool_name, const char *args_json, AiToolDoneFn done, void *ud); /* done runs on the loop thread, once */
} AiServerTools;
void ai_register_server_tools(const AiServerTools *t);

/*
 * Bot chat (shared/src/bot_chat.c, server/src/bot_banter.c): a game server's `bot-banter`
 * frame (internal WebSocket, doc/backend.md 5.2) asks for ONE short in-character chat line of a
 * bot player: {name, personality (chill|tryhard|salty|hype|goofy|quiet), event (win|lose|draw|
 * kill|death), other?, mode?, map?, kills?, deaths?, place?, players?}. The request is one
 * non-streamed completion with a tight prompt and ai.banter.max_tokens; the answer is cut to its
 * first line and 70 characters. done(ud, err, reply) runs once on the loop thread: reply {text}
 * on success, err otherwise ("AI unavailable" when the proxy is off / unconfigured, "rate
 * limited" past ai.banter.rpm, the upstream's failure). The game server filters the text itself.
 */
typedef void (*AiBanterDoneFn)(void *ud, const char *err, const cJSON *reply);
void ai_bot_banter(Brain *b, const cJSON *frame, AiBanterDoneFn done, void *ud);

/* ai_search.c: web search (a DuckDuckGo MCP server as a stdio child; ai.search.command) */
int ai_search_init(Brain *b);
void ai_search_shutdown(Brain *b);

#endif
