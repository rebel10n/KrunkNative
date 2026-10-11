/* Private glue between http.c and ws.c (socket takeover on upgrade). */
#ifndef KNSVC_HTTP_INTERNAL_H
#define KNSVC_HTTP_INTERNAL_H

#include "knsvc/buf.h"
#include "knsvc/http.h"

typedef struct kn_http_takeover {
    int fd;
    kn_loop *loop;
    kn_buf leftover;          /* bytes received after the upgrade request (owned) */
    char remote_addr[64];
} kn_http_takeover;

/* Detach the connection's socket from the HTTP server (no response is sent).
 * Releases req. 0 ok, -1 if the client is gone (req released too). */
int kn_http__takeover(kn_http_req *req, kn_http_takeover *out);

#endif
