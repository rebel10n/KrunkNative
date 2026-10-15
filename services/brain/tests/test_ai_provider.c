/*
 * services/brain/tests/test_ai_provider.c - the AI provider profiles (services/brain/ai_provider.c),
 * pure, no database:
 *   three profiles and three consumers, each resolving to its own upstream;
 *   the legacy single-key config (ai.upstream_*) as the "default" profile;
 *   env precedence (KN_AI_<NAME>_URL / _KEY over the file, KN_AI_<CONSUMER>_PROVIDER over the file,
 *   KN_AI_UPSTREAM_* over ai.upstream_*);
 *   routing failures (missing, unconfigured, non-http url, reserved name);
 *   the limits (rpm over every consumer of a profile, the daily token budget) and the counters;
 *   redaction: no key in a config dump (kn_config_dump_redacted), a provider dump or /ai/status.
 *
 *   kn_brain_ai_provider_test          (exit 0 = all checks passed)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ai_provider.h"
#include "knsvc/config.h"

static int fails;

static void check(const int cond, const char *what) {
    printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

static const char *urlof(const AiProvider *p) { return p ? p->url : "(none)"; }
static const char *keyof(const AiProvider *p) { return p && p->key ? p->key : "(none)"; }
static const char *modelof(const AiProvider *p) { return p ? p->model : "(none)"; }
static const char *nameof(const AiProvider *p) { return p ? p->name : "(none)"; }

static const char *const kEnvNames[] = {
    "KN_AI_UPSTREAM_URL", "KN_AI_UPSTREAM_KEY", "KN_AI_DEFAULT_URL", "KN_AI_DEFAULT_KEY",
    "KN_AI_EDITOR_URL", "KN_AI_EDITOR_KEY", "KN_AI_VISION_URL", "KN_AI_VISION_KEY",
    "KN_AI_FAST_URL", "KN_AI_FAST_KEY", "KN_AI_LAB_URL", "KN_AI_LAB_KEY",
    "KN_AI_METER_URL", "KN_AI_METER_KEY", "KN_AI_BAD_URL", "KN_AI_BAD_KEY",
    "KN_AI_EDITOR_PROVIDER", "KN_AI_BANTER_PROVIDER", "KN_AI_MODERATION_PROVIDER",
};

static void clear_env(void) {
    for (size_t i = 0; i < sizeof(kEnvNames) / sizeof(kEnvNames[0]); i++) unsetenv(kEnvNames[i]);
}

/* the JSON as a config file (kn_config_load merges the KN_* environment) */
static kn_config *load_json(const char *json, const char *tag) {
    char path[256], err[256];
    snprintf(path, sizeof(path), "ai_provider_test_%s.json", tag);
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fputs(json, f);
    fclose(f);
    kn_config *cfg = kn_config_load(path, 0, NULL, err, sizeof(err));
    unlink(path);
    if (!cfg) fprintf(stderr, "config %s: %s\n", tag, err);
    return cfg;
}

/* the profiles are copies: the config can go once they are loaded */
static int load(const char *json, const char *tag) {
    kn_config *cfg = load_json(json, tag);
    if (!cfg) return 0;
    ai_provider_load(cfg);
    kn_config_free(cfg);
    return 1;
}

static const char *kThree =
    "{\"ai\": {"
    " \"upstream_url\": \"https://default.example/v1\", \"upstream_key\": \"sk-DEFAULT-KEY-1111\","
    " \"providers\": {"
    "  \"editor\": {\"url\": \"https://editor.example/v1/\", \"key\": \"sk-EDITOR-KEY-2222\", \"model\": \"glm-5.3-flash\","
    "              \"rpm\": 60, \"daily_budget_tokens\": 1000000},"
    "  \"vision\": {\"url\": \"https://vision.example/v1\", \"key\": \"sk-VISION-KEY-3333\", \"model\": \"gemini-3.8-flash-high\","
    "              \"rpm\": 30},"
    "  \"fast\": {\"url\": \"https://fast.example/v1\", \"key\": \"sk-FAST-KEY-4444\", \"rpm\": 120, \"daily_budget_tokens\": 50000}"
    " },"
    " \"editor\": {\"provider\": \"editor\"},"
    " \"banter\": {\"provider\": \"fast\"},"
    " \"moderation\": {\"provider\": \"vision\"}"
    "}}";

static void test_three_profiles(void) {
    printf("three profiles, three consumers:\n");
    clear_env();
    if (!load(kThree, "three")) {
        check(0, "the config loads");
        return;
    }
    const AiProvider *ed = ai_provider_get("editor");
    const AiProvider *bn = ai_provider_get("banter");
    const AiProvider *md = ai_provider_get("moderation");
    check(ed && bn && md, "each consumer resolves to a configured profile");
    check(!strcmp(nameof(ed), "editor") && !strcmp(nameof(bn), "fast") && !strcmp(nameof(md), "vision"), "each consumer is routed to its own profile");
    check(ed != bn && bn != md && ed != md, "three distinct upstreams");
    check(!strcmp(urlof(ed), "https://editor.example/v1") && !strcmp(keyof(ed), "sk-EDITOR-KEY-2222"), "editor: its url (trailing slash cut) and its key");
    check(!strcmp(urlof(bn), "https://fast.example/v1") && !strcmp(keyof(bn), "sk-FAST-KEY-4444"), "banter: fast's url and key");
    check(!strcmp(urlof(md), "https://vision.example/v1") && !strcmp(keyof(md), "sk-VISION-KEY-3333"), "moderation: vision's url and key");
    check(!strcmp(modelof(ed), "glm-5.3-flash") && !strcmp(modelof(md), "gemini-3.8-flash-high") && modelof(bn)[0] == 0,
          "models: editor and moderation as set, banter the consumer's choice");
    check(ed->limit.rpm == 60 && ed->limit.daily_budget_tokens == 1000000 && bn->limit.rpm == 120 && bn->limit.daily_budget_tokens == 50000
              && md->limit.rpm == 30 && md->limit.daily_budget_tokens == 0,
          "limits: per profile (rpm, daily_budget_tokens), 0 = none");
    check(ai_provider_get("nobody") == NULL && ai_provider_get(NULL) == NULL, "an unknown consumer has no profile");
    ai_provider_shutdown();
}

static void test_legacy(void) {
    printf("legacy single key (ai.upstream_url / ai.upstream_key):\n");
    clear_env();
    if (!load("{\"ai\": {\"upstream_url\": \"https://legacy.example/v1/\", \"upstream_key\": \"sk-LEGACY-KEY-5555\"}}", "legacy")) {
        check(0, "the config loads");
        return;
    }
    const AiProvider *ed = ai_provider_get("editor");
    const AiProvider *bn = ai_provider_get("banter");
    const AiProvider *md = ai_provider_get("moderation");
    check(ed && ed == bn && bn == md, "every consumer resolves to the default profile");
    check(!strcmp(nameof(ed), "default") && !strcmp(urlof(ed), "https://legacy.example/v1") && !strcmp(keyof(ed), "sk-LEGACY-KEY-5555"),
          "the default profile is ai.upstream_url / ai.upstream_key (trailing slash cut)");
    check(ed && ed->limit.rpm == 0 && ed->limit.daily_budget_tokens == 0 && modelof(ed)[0] == 0, "no limits and no model override");
    ai_provider_shutdown();

    if (!load("{\"ai\": {\"upstream_url\": \"https://legacy.example/v1\"}}", "nokey")) {
        check(0, "the config loads");
        return;
    }
    check(ai_provider_get("editor") == NULL && ai_provider_get("banter") == NULL, "a url without a key: not configured, every consumer NULL");
    ai_provider_shutdown();
}

static void test_env(void) {
    printf("env precedence:\n");
    clear_env();
    setenv("KN_AI_EDITOR_URL", "https://env-editor.example/v1", 1);
    setenv("KN_AI_EDITOR_KEY", "sk-ENV-EDITOR-6666", 1);
    setenv("KN_AI_BANTER_PROVIDER", "lab", 1);
    setenv("KN_AI_LAB_URL", "https://lab.example/v1", 1);
    setenv("KN_AI_LAB_KEY", "sk-LAB-KEY-7777", 1);
    if (load(kThree, "env")) {
        const AiProvider *ed = ai_provider_get("editor");
        const AiProvider *bn = ai_provider_get("banter");
        const AiProvider *md = ai_provider_get("moderation");
        check(!strcmp(urlof(ed), "https://env-editor.example/v1"), "KN_AI_EDITOR_URL beats ai.providers.editor.url");
        check(!strcmp(keyof(ed), "sk-ENV-EDITOR-6666"), "KN_AI_EDITOR_KEY beats ai.providers.editor.key");
        check(!strcmp(nameof(bn), "lab") && !strcmp(urlof(bn), "https://lab.example/v1") && !strcmp(keyof(bn), "sk-LAB-KEY-7777"),
              "KN_AI_BANTER_PROVIDER beats ai.banter.provider; lab exists from its env alone");
        check(!strcmp(keyof(md), "sk-VISION-KEY-3333") && !strcmp(urlof(md), "https://vision.example/v1"), "a profile without env keeps its file values");
    } else {
        check(0, "the config loads");
    }
    ai_provider_shutdown();

    clear_env();
    setenv("KN_AI_UPSTREAM_URL", "https://upstream-env.example/v1", 1);
    setenv("KN_AI_UPSTREAM_KEY", "sk-ENV-DEFAULT-8888", 1);
    if (load("{\"ai\": {\"upstream_url\": \"https://file.example/v1\", \"upstream_key\": \"sk-FILE-KEY-0000\"}}", "envlegacy")) {
        const AiProvider *ed = ai_provider_get("editor");
        check(!strcmp(urlof(ed), "https://upstream-env.example/v1") && !strcmp(keyof(ed), "sk-ENV-DEFAULT-8888"),
              "KN_AI_UPSTREAM_URL / _KEY beat ai.upstream_url / ai.upstream_key");
    } else {
        check(0, "the config loads");
    }
    ai_provider_shutdown();

    clear_env();
    setenv("KN_AI_DEFAULT_URL", "https://default-env.example/v1", 1);
    setenv("KN_AI_DEFAULT_KEY", "sk-DEFAULT-ENV-9999", 1);
    if (load("{\"ai\": {\"upstream_url\": \"https://file.example/v1\", \"upstream_key\": \"sk-FILE-KEY-0000\"}}", "envdefault")) {
        const AiProvider *ed = ai_provider_get("editor");
        check(!strcmp(urlof(ed), "https://default-env.example/v1") && !strcmp(keyof(ed), "sk-DEFAULT-ENV-9999"),
              "KN_AI_DEFAULT_URL / _KEY (the default profile) beat the legacy keys");
    } else {
        check(0, "the config loads");
    }
    ai_provider_shutdown();
    clear_env();
}

static void test_routing_failures(void) {
    printf("routing failures:\n");
    clear_env();
    const char *json =
        "{\"ai\": {\"providers\": {"
        "   \"bad\": {\"url\": \"ftp://bad.example/v1\", \"key\": \"sk-BAD-KEY-0000\"},"
        "   \"upstream\": {\"url\": \"https://reserved.example/v1\", \"key\": \"sk-RESERVED-KEY-0001\"}"
        " }, \"editor\": {\"provider\": \"nope\"}, \"banter\": {\"provider\": \"bad\"}, \"moderation\": {\"provider\": \"upstream\"}}}";
    if (!load(json, "fail")) {
        check(0, "the config loads");
        return;
    }
    check(ai_provider_get("editor") == NULL, "a routed profile that does not exist: NULL");
    check(ai_provider_get("banter") == NULL, "a non-http(s) url: the profile is not configured, NULL");
    check(ai_provider_get("moderation") == NULL, "the reserved name 'upstream' is not a profile, NULL");
    ai_provider_shutdown();
}

static void test_limits(void) {
    printf("limits and counters:\n");
    clear_env();
    const char *json =
        "{\"ai\": {\"providers\": {"
        "   \"fast\": {\"url\": \"https://fast.example/v1\", \"key\": \"sk-FAST-LIMIT-9999\", \"rpm\": 2},"
        "   \"meter\": {\"url\": \"https://meter.example/v1\", \"key\": \"sk-METER-KEY-1212\", \"daily_budget_tokens\": 1000}"
        " }, \"banter\": {\"provider\": \"fast\"}, \"editor\": {\"provider\": \"fast\"}, \"moderation\": {\"provider\": \"meter\"}}}";
    if (!load(json, "limits")) {
        check(0, "the config loads");
        return;
    }
    const AiProvider *fast = ai_provider_get("banter");
    char msg[200];
    int retry = -1;
    check(ai_provider_check(fast, msg, sizeof(msg), &retry) == 0, "rpm: admitted before any request");
    ai_provider_begin(fast, "banter");
    ai_provider_begin(fast, "banter");
    check(ai_provider_check(fast, msg, sizeof(msg), &retry) == -1 && strstr(msg, "Too many AI requests this minute") && retry > 0,
          "rpm: refused at the limit with a message and a retry time");
    check(ai_provider_check(ai_provider_get("editor"), msg, sizeof(msg), &retry) == -1, "rpm: counts across every consumer of the profile (editor too)");

    const AiProvider *meter = ai_provider_get("moderation");
    check(ai_provider_check(meter, msg, sizeof(msg), &retry) == 0, "budget: admitted under the daily budget");
    ai_provider_begin(meter, "moderation");
    ai_provider_charge(meter, "moderation", 600);
    check(ai_provider_check(meter, msg, sizeof(msg), &retry) == 0, "budget: 600 of 1000 tokens used, still admitted");
    ai_provider_charge(meter, "moderation", 500);
    check(ai_provider_check(meter, msg, sizeof(msg), &retry) == -1 && strstr(msg, "Daily AI token budget used up") && retry > 0,
          "budget: refused once the day's tokens reach the budget");

    cJSON *o = cJSON_CreateObject();
    ai_provider_status_add(o);
    char *txt = cJSON_PrintUnformatted(o);
    check(txt && !strstr(txt, "sk-FAST-LIMIT-9999") && !strstr(txt, "sk-METER-KEY-1212") && !strstr(txt, "meter.example"),
          "status: no key and no url in the answer");
    check(txt && strstr(txt, "\"requests_last_minute\":2") && strstr(txt, "\"tokens_today\":1100"),
          "status: the rpm window (2) and the profile's tokens today (1100)");
    check(txt && strstr(txt, "\"moderation\":{\"provider\":\"meter\""), "status: consumers name their profile");
    free(txt);
    cJSON_Delete(o);
    ai_provider_shutdown();
}

static void test_redaction(void) {
    printf("redaction:\n");
    clear_env();
    setenv("KN_AI_LAB_URL", "https://lab.example/v1", 1);
    setenv("KN_AI_LAB_KEY", "sk-LAB-KEY-7777", 1);
    setenv("KN_AI_UPSTREAM_KEY", "sk-ENV-DEFAULT-8888", 1);
    const char *json =
        "{\"ai\": {"
        " \"upstream_url\": \"https://default.example/v1\", \"upstream_key\": \"sk-DEFAULT-KEY-1111\","
        " \"providers\": {"
        "  \"editor\": {\"url\": \"https://editor.example/v1\", \"key\": \"sk-EDITOR-KEY-2222\"},"
        "  \"vision\": {\"url\": \"https://u:sk-USERINFO-0000@vision.example/v1?key=sk-QUERY-0000\", \"key\": \"sk-VISION-KEY-3333\"},"
        "  \"fast\": {\"url\": \"https://fast.example/v1\", \"key\": \"sk-FAST-KEY-4444\"}"
        " }, \"editor\": {\"provider\": \"editor\"}, \"banter\": {\"provider\": \"fast\"}, \"moderation\": {\"provider\": \"vision\"}}}";
    kn_config *cfg = load_json(json, "redact");
    if (!cfg) {
        check(0, "the config loads");
        return;
    }
    ai_provider_load(cfg);
    char *cd = kn_config_dump_redacted(cfg);
    char *pd = ai_provider_dump();
    const char *keys[] = {"sk-DEFAULT-KEY-1111", "sk-EDITOR-KEY-2222", "sk-VISION-KEY-3333", "sk-FAST-KEY-4444", "sk-LAB-KEY-7777",
                          "sk-ENV-DEFAULT-8888"};
    int leaks_cfg = 0, leaks_prov = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (cd && strstr(cd, keys[i])) leaks_cfg++;
        if (pd && strstr(pd, keys[i])) leaks_prov++;
    }
    check(cd && strstr(cd, "***") && strstr(cd, "upstream_url"), "config dump: the keys are masked, the rest is there");
    check(leaks_cfg == 0, "config dump (kn_config_dump_redacted): no key in it, env keys included");
    check(pd && strstr(pd, "\"key\":\"***\"") && strstr(pd, "\"url\":\"https://editor.example\""), "provider dump: keys masked, url cut to its origin");
    check(leaks_prov == 0, "provider dump (ai_provider_dump): no key in it");
    const int clean = pd && !strstr(pd, "sk-USERINFO-0000") && !strstr(pd, "sk-QUERY-0000") && !strstr(pd, "/v1");
    check(clean, "provider dump: no user info, query or path from a url");
    if (!clean && pd) printf("provider dump was:\n%s\n", pd);
    free(cd);
    free(pd);
    kn_config_free(cfg);
    ai_provider_shutdown();
    clear_env();
}

int main(void) {
    test_three_profiles();
    test_legacy();
    test_env();
    test_routing_failures();
    test_limits();
    test_redaction();
    printf("ai provider test: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
