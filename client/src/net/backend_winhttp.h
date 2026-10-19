#pragma once
// WinHTTP transport of the brain client on Windows (backend_winhttp.c), used by
// backend_native.c in place of http_conn.c: HTTP(S) requests and the WS(S) social socket.
// WinHTTP verifies TLS against the Windows certificate store and follows the system proxy.
// The WebSocket API needs Windows 8+. Blocking calls, made from the backend's worker threads.
#ifdef _WIN32
#include <stddef.h>

// One request. Returns the HTTP status (0 = transport error, `err` says why); *body is the
// malloc'd response body (NUL-terminated, also on error statuses), NULL on a transport error.
int kn_winhttp_request(const char *method, const char *url, const char *json_body, int timeout_ms,
                       char **body, size_t *length, char *err, size_t errn);

// A streamed request (client/src/net/http_stream.c): headers = extra "Name: value\r\n" lines (NULL
// none), body sent as application/json (NULL none). on_head once with the status and content type,
// then on_data per piece as it arrives (return 0 to stop: the call then ends with status 0,
// "cancelled"). Returns the HTTP status, 0 on a transport error (`err` says why). Blocking.
int kn_winhttp_stream(const char *method, const char *url, const char *headers, const char *body, int timeout_ms,
                      void (*on_head)(void *user, int status, const char *content_type), int (*on_data)(void *user, const char *data, size_t len),
                      void *user, char *err, size_t errn);

typedef struct KnWinWs KnWinWs;
// ws:// / wss:// (http(s):// accepted): connect + upgrade. NULL on failure (`err` says why).
KnWinWs *kn_winws_open(const char *url, int timeout_ms, char *err, size_t errn);
// The same with extra request headers ("Name: value\r\n" lines, NULL: User-Agent only) and the HTTP status of a
// refused upgrade (*status_out: 0 when no answer came; status_out may be NULL). The proxy is the client's (http_proxy.h).
KnWinWs *kn_winws_open_as(const char *url, const char *headers, int timeout_ms, int *status_out, char *err, size_t errn);
int kn_winws_send_text(KnWinWs *ws, const char *text, size_t len); // 1 = sent (any thread, one sender)
int kn_winws_send_binary(KnWinWs *ws, const void *data, size_t len); // 1 = sent (one sender: the socket's send side)
// Blocks for the next whole message: 1 = *out (malloc'd, NUL-terminated), *is_text; 0 = closed / error.
int kn_winws_recv(KnWinWs *ws, char **out, size_t *len, int *is_text);
void kn_winws_abort(KnWinWs *ws); // unblocks a kn_winws_recv in another thread
void kn_winws_free(KnWinWs *ws);
#endif
