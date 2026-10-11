#pragma once
// Private glue between backend_common.c and the transports (backend_native.c,
// backend_web.c). Main thread only.
#include <stddef.h>

void backend_store_url(const char *url);
void backend_social_ws_url(char *out, size_t n);   // ws(s)://.../social/ws?enc=json
void backend_dispatch_text(const char *text);       // one received JSON frame
void backend_dispatch_event(const char *label);     // synthetic ["__open"] / ["__close"]
int backend_social_allowed(void);                   // transports: 0 = do not open the socket yet (probe)
