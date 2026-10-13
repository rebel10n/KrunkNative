#pragma once
// Streamed HTTP(S) requests for the client (the editor's AI panel: kn_brain's /ai/chat SSE stream,
// or a user's own OpenAI-compatible endpoint). The body arrives in pieces as the server sends it,
// unlike backend_http_request (backend.h), which hands over the whole answer at the end.
//
//   native POSIX  one worker thread per stream over http_conn.c (https:// needs OpenSSL:
//                 KRUNKNATIVE_HAVE_TLS); chunked transfer-encoding decoded on the fly
//   Windows       WinHTTP (backend_winhttp.c kn_winhttp_stream) on a worker thread
//   web           the browser fetch() with a ReadableStream reader; aborted by AbortController
//
// Every callback runs on the main thread from http_stream_poll() (call it once per frame), in
// order: on_open once the status line and headers are in (not on a transport error), on_data for
// each piece, on_done exactly once (status 0 = transport error, `error` says why; else the HTTP
// status and error NULL). After http_stream_cancel() no callback runs any more.
#include <stddef.h>

typedef struct HttpStream HttpStream;

typedef struct {
    void (*on_open)(void *user, int status, const char *content_type);
    void (*on_data)(void *user, const char *data, size_t len);
    void (*on_done)(void *user, int status, const char *error);
} HttpStreamCallbacks;

// method "GET" / "POST"; headers: extra "Name: value\r\n" lines (NULL = none; never logged);
// body: request body sent as application/json (NULL = none). NULL when it could not start
// (bad URL, https without TLS support, out of memory): on_done is then not called.
HttpStream *http_stream_start(const char *method, const char *url, const char *headers, const char *body,
                              const HttpStreamCallbacks *cb, void *user);
// Stops the transfer; no callbacks after this returns. The handle is invalid afterwards.
void http_stream_cancel(HttpStream *s);
// Main thread, once per frame: delivers the callbacks.
void http_stream_poll(void);
// Streams started and not yet finished or cancelled.
int http_stream_active(void);
