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
 * Config (all overridable with KN_AI_* env vars, e.g. KN_AI_UPSTREAM_KEY):
 *   ai.enabled              (bool, default 1)   off = 503 {"error":"AIUnavailable"}
 *   ai.upstream_url         KN_AI_UPSTREAM_URL  OpenAI-compatible base (".../v1")
 *   ai.upstream_key         KN_AI_UPSTREAM_KEY  Bearer key (never logged, never sent to a client)
 *   ai.api                  "chat" (default) | "responses"
 *   ai.default_model        (default glm-5.3-flash)
 *   ai.models_hide          ids left out of /ai/models (list)
 *   ai.max_tokens / ai.timeout_ms / ai.connect_timeout_ms / ai.max_messages
 *   ai.models_ttl_s         /ai/models cache lifetime
 *   ai.health_interval_s    known-prompt probe period (0 = only at start)
 *   ai.limit.rpm / ai.limit.daily / ai.limit.daily_tokens / ai.limit.concurrent
 *
 * Health: at start and every ai.health_interval_s the brain sends the default model a prompt
 * with a random token and checks the answer echoes it. "degraded" = the upstream answers but
 * drops or rewrites request content (the editor shows "AI service degraded").
 */
#ifndef KN_BRAIN_AI_H
#define KN_BRAIN_AI_H

#include "brain.h"

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

#endif
