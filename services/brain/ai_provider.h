/*
 * services/brain/ai_provider.h - named AI provider profiles (ai_provider.c).
 *
 * One profile is one OpenAI-compatible endpoint with its own key. Each consumer is routed to a
 * profile by ai.<consumer>.provider (a profile name, default "default"):
 *
 *   editor      the map editor's chat (ai.c: /ai/chat, /ai/models, the health probe)
 *   banter      bot chat lines for game servers (ai.c: ai_bot_banter)
 *   moderation  avatar review (avatar.c: ai_moderate_image)
 *
 * Consumers never read ai.upstream_key themselves: they ask ai_provider_get(consumer) for the url,
 * key, model and limit. The key is never logged, never in an answer and never in a dump.
 */
#ifndef KN_BRAIN_AI_PROVIDER_H
#define KN_BRAIN_AI_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "knsvc/config.h"

typedef struct AiProviderLimit {
    int rpm;                      /* requests per minute over all consumers of the profile; 0 = none */
    int64_t daily_budget_tokens;  /* prompt + completion tokens per UTC day over all consumers; 0 = none */
} AiProviderLimit;

typedef struct AiProvider {
    const char *name;     /* the profile name ("default" = the legacy ai.upstream_* variables) */
    const char *url;      /* base without a trailing slash, e.g. "https://host/v1" */
    const char *key;      /* bearer key; NULL when the profile is not configured */
    const char *model;    /* the profile's model; "" = the consumer's own choice */
    AiProviderLimit limit;
} AiProvider;

/*
 * Load the profiles and the routing (call once at brain start, before any consumer runs; a
 * second call replaces the first). Profile names come from ai.providers.<name>, from the
 * environment (KN_AI_<NAME>_URL / KN_AI_<NAME>_KEY, which also make a profile with no file
 * entry) and "default". Per profile, the first hit wins:
 *   url    KN_AI_<NAME>_URL, ai.providers.<name>.url, (default only) ai.upstream_url
 *   key    KN_AI_<NAME>_KEY, ai.providers.<name>.key, (default only) ai.upstream_key
 *   model  ai.providers.<name>.model          rpm  ai.providers.<name>.rpm
 *   daily_budget_tokens  ai.providers.<name>.daily_budget_tokens
 * ai.upstream_url / ai.upstream_key are read through the knsvc lookup, so KN_AI_UPSTREAM_URL and
 * KN_AI_UPSTREAM_KEY override them. A profile is configured when it has an http(s) url and a key.
 * The name "upstream" is reserved (KN_AI_UPSTREAM_* are the legacy default).
 */
void ai_provider_load(const kn_config *cfg);
void ai_provider_shutdown(void);

/* The profile a consumer is routed to ("editor", "banter", "moderation"), or NULL when that
 * profile does not exist or is not configured (the consumer's feature is off). */
const AiProvider *ai_provider_get(const char *consumer);

/*
 * Limits (loop thread only). ai_provider_check counts nothing: 0 = admitted, -1 = refused with a
 * client message in msg and retry_s (0 = unknown). ai_provider_begin counts one admitted request
 * for the profile and the consumer; ai_provider_charge adds the tokens a finished request used.
 */
int ai_provider_check(const AiProvider *p, char *msg, size_t n, int *retry_s);
void ai_provider_begin(const AiProvider *p, const char *consumer);
void ai_provider_charge(const AiProvider *p, const char *consumer, int64_t tokens);

/* Adds "providers" (per profile: configured, model, rpm, daily_budget_tokens, requests in the last
 * minute and today, tokens today) and "consumers" (per consumer: provider, configured, requests and
 * tokens today) to o. Never a key; the URL is cut to its origin. */
void ai_provider_status_add(cJSON *o);

/* The routing and the profiles as JSON text, the key replaced by "***" (or ""). Caller frees. */
char *ai_provider_dump(void);

#endif
