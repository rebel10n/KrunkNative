#pragma once
#include <stddef.h>
// Which ranked backend the Ranked popup uses (client/src/ui/ranked_ui.c). Two account systems can be
// signed in at once: a krunker.io account (krunker_account.h), which uses v10.0.2's ranked services at
// krunker.io (krunker_ranked.h), and a KrunkNative account, which uses kn_brain's /mm/ranked/* routes.
// krunker.io wins when both are signed in: v10's ranked is the krunker.io one, and the KrunkNative
// account stays signed in beside it (its own servers keep it). A guest (neither) gets no ranked backend.
typedef enum {
    RANKED_BACKEND_NONE = 0, // nobody signed in: the popup asks for a sign-in
    RANKED_BACKEND_KN,       // a KrunkNative account: kn_brain's routes
    RANKED_BACKEND_KRUNKER,  // a krunker.io account: krunker.io's API, GAPI and gamefrontend
} RankedBackend;

// The routing rule, as a pure function. ranked_active_backend() (ranked_ui.c) is the one place that
// feeds it the account state and the one the popup asks.
RankedBackend ranked_backend_route(int krunker_signed_in, int kn_signed_in);

// The game socket of a ranked match (v10's joinAssignment / quickJoinRankedMatch -> _33940 -> _33929,
// v10:98678, 98729, 97583, 222104-222135): the assignment's connection "host[:port]" (port 3000 when none)
// as the socket's URL, the krunker.io account's fresh token as its only parameter (at=; no gameId or
// clientKey, which the seek sends). production (krunker.io's own API): wss://host/ws? with no port, as
// v10's `//host` on krunker.io (_7263); otherwise <wss|ws>://host:port/ws? (secure: the API is https).
// url gets the socket's URL, log the same without the token (what the log shows). 0 = no host, or a
// result that does not fit.
int ranked_match_url(const char *connection, int production, int secure, const char *token,
                     char *url, size_t url_n, char *log, size_t log_n);
// The host of a connection ("host" of "host:port"): isInOtherRankedMatch compares it (v10:98528). 0 = none.
int ranked_match_host(const char *connection, char *host, size_t host_n);
