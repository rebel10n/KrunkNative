// Krunker v10.0.2 Social hub on the v10 main page (client/src/ui/v10_social.c) and the v10 gapi /
// event-bus transport it shares with the clan window (client/src/ui/v10_gapi.c).
#ifndef KRUNKNATIVE_V10_SOCIAL_H
#define KRUNKNATIVE_V10_SOCIAL_H

#include <backend.h>
#include <cJSON.h>
#include <html_dom.h>

// ---- gapi over the social socket (services/brain/social_gapi.c): v10's
// _10449.<group>.<call>() requests as ["gapi", rid, method, path, body]. The callback gets the
// HTTP-like status, `data` (NULL when absent) and the error ({code, message}, NULL on success);
// both are borrowed for the call. status 0 = no brain / the socket closed before the answer.
typedef void (*V10GapiFn)(void *user, int status, const cJSON *data, const cJSON *error);
int v10_gapi(const char *method, const char *path, const cJSON *body, V10GapiFn fn, void *user); // 0 = not sent (fn not called)
void v10_gapi_cancel(V10GapiFn fn, void *user); // forget pending requests of (fn, user): fn is not called for them
// v10's _34582: an error object -> the English text (errors.* i18n), else `fallback`
const char *v10_gapi_error_text(const cJSON *error, const char *fallback);
// A gapi GET answered as the brain's HTTP answer (BackendHttpFn, backend.h): the body is the {"data", "error"}
// envelope as text, so a caller that parses the brain's body parses the krunker.io answer the same way. For
// the official account's reads (v10_gapi: krunker.io while it is the active backend). 0 = not sent.
int v10_gapi_http(const char *path, BackendHttpFn fn, void *user);

// ---- the Social hub (v10_social.c)
int  v10_social_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void v10_social_tick(void);                // per frame: the event bus (connect / ping), menu badge
void v10_social_account_changed(void);     // login / logout: (dis)connect the friends event bus
int  v10_social_unread_total(void);        // sum of the friends' unread counts (the Social menu badge)
const cJSON *v10_social_friends(void);
char *v10_social_window_friends_html(void); // window 5's #friendList (malloc'd), NULL = not loaded yet     // the event bus friends list (window 5 Friends tab); NULL = none yet
void v10_social_scenario(const cJSON *state); // --render-scenario: state.social
int  v10_social_key(int key);              // Escape / Enter on the hub's popups: 1 = handled

#endif
