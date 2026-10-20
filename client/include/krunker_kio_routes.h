#pragma once
// The krunker.io reads v10.0.2 makes while krunker.io is the active account backend (account_backend() ==
// ACCOUNT_BACKEND_KRUNKER). The brain gate refuses every kn_brain request then, so each of these used to be a
// kn_brain call that never went out, and the Games window, the hub and the Servers footer showed the local
// fallback instead of krunker.io's data. The hosts come from krunker_account.h:
//   data client (api.krunker.io, v10 API_URI, _46358 / _10444): krunker_host_url(KRUNKER_HOST_API, ...) for the
//     Games window's maps and mods (webhooks/general) and the streams (v10:131895-131941, v10:25991)
//   social (social.krunker.io, v10 SOCIAL_URI): krunker_service_url(KRUNKER_SVC_SOCIAL, ...) for the player count
//     (v10:211170)
//   gapi (gapi.svc.krunker.io, v10 _10449 = new _10077(_7284)): krunker_host_url(KRUNKER_HOST_GAPI, ...) for the
//     leaderboards (v10:26987-27036)
// These builders take the base as the caller got it (no trailing slash needed) and write the path only, so the
// host check in client/tests/krunker_kio_routes_test.c sees the same URL the request carries. Pure: no client state.

#include <stddef.h>

// The timeout of these reads (krunker.io's lobby list uses the same, KRUNKER_LOBBIES_TIMEOUT_MS)
#define KRUNKER_KIO_READ_TIMEOUT_MS 6000

// "<api_base>/webhooks/general/<group>[?<query>]": group "maps/list", "maps/search", "mods/list" or "mods/search"
// (v10 listMaps, searchMap, listMods, searchMod). Returns the length, 0 when it does not fit.
size_t krunker_kio_webhook_url(char *out, size_t n, const char *api_base, const char *group, const char *query);

// The webhook group a Games / hub request maps to: "/maps" -> "maps/list", "/mods" -> "mods/list", "/search" with
// search_type "map" -> "maps/search" and "mod" -> "mods/search". NULL for every other path (a brain-only route).
const char *krunker_kio_webhook_group(const char *kn_path, const char *search_type);

// The query of a map search, "val=<query>&cat=<cat>" with URLSearchParams' encoding (a space is '+'). cat -1 is
// v10's "All" select value. Returns the length, 0 when it does not fit.
size_t krunker_kio_search_query(char *out, size_t n, const char *val, int cat);

// "<api_base>/streams/top?cb=2" (v10 getTopStreams, v10:25991-25992). Returns the length, 0 when it does not fit.
size_t krunker_kio_streams_url(char *out, size_t n, const char *api_base);

// "<social_base>/player-count" (v10 loadData's count, v10:211170). Returns the length, 0 when it does not fit.
size_t krunker_kio_player_count_url(char *out, size_t n, const char *social_base);

// The gapi path of a KrunkNative leaderboard path: "/api/leaderboards/<x>[?<query>]" -> "/leaderboards/<x>[?<query>]"
// (v10's leaderboards client, v10:26987-27036). Returns the length; 0 when the path is not a leaderboard, so the
// caller sends nothing for it.
size_t krunker_kio_gapi_leaderboard_path(char *out, size_t n, const char *kn_path);
