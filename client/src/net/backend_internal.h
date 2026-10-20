#pragma once
// Private glue between backend_common.c and the transports (backend_native.c,
// backend_web.c). Main thread only.
#include <stddef.h>

// A brain request (backend_http_get, backend_http_request) with no answer by then has failed: status 0, the reason as
// its body (backend.h). Native: the socket's connect and receive timeout. Web: fetch() is aborted (backend_web.c).
#define BACKEND_HTTP_TIMEOUT_MS 8000

void backend_store_url(const char *url);
void backend_social_ws_url(char *out, size_t n);   // ws(s)://.../social/ws?enc=json
void backend_dispatch_text(const char *text);       // one received JSON frame
void backend_dispatch_event(const char *label);     // synthetic ["__open"] / ["__close"]
int backend_social_allowed(void);                   // transports: 0 = do not open the socket yet (probe)
void backend_probe_tick(void);                       // per frame: a probe with no answer has failed; the retry is due
// The brain gate (backend.h backend_set_brain_gate): 1 = the request must not be sent. url is the full
// URL of the request (backend_brain_refuses_url) or the path on the brain (backend_brain_refuses_path).
int backend_brain_refuses_url(const char *url);
int backend_brain_refuses_path(void);
