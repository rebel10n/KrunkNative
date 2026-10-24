#pragma once
// The comp webhook's transport (the match_end body: shared/include/comp_webhook.h, built by accounts.c at a
// comp game's end). server_bin posts it from a detached worker thread with timeouts (server/src/comp_webhook.c),
// so the game loop never waits on it; the client's offline sandbox posts nothing (server/src/sandbox_backend.c).
// body is a cJSON print the call takes over (freed with cJSON_free). Returns 0 when a post was started.
int server_webhook_post(const char *url, char *body);
