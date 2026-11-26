#pragma once
#include <stddef.h>
// A WebSocket to one of krunker.io's services, as the page's `new WebSocket(url)`: natively a thread over
// http_conn (through the client's proxy, Origin https://krunker.io like the game socket's handshake in
// krunker_net.c), on the web the browser's WebSocket. The Windows build has none (krunker_ws_open refuses,
// as krunker_net's krunker.io socket does there). Events are queued and handed to the main thread by
// krunker_ws_poll. Two clients use it: the friends event bus (krunker_eb.h, text) and the hub page's
// social socket (krunker_social.h, binary msgpack frames).
//
// Main thread only. The URL (it carries tokens) is never logged.

enum {
    KRUNKER_WS_OPEN = 0, // onopen
    KRUNKER_WS_MESSAGE,  // one whole message (binary 1 = an ArrayBuffer, 0 = a string)
    KRUNKER_WS_CLOSED,   // onclose (code = the close code; 1006 = dropped / never opened)
};
typedef void (*KrunkerWsFn)(void *user, int event, const void *data, size_t len, int binary, int code);

typedef struct KrunkerWs KrunkerWs;

// One socket slot; tag names it in the log ("krunker-eb"). Lives for the run.
KrunkerWs *krunker_ws_new(const char *tag);
// Opens the socket (an open one is closed first, without a KRUNKER_WS_CLOSED). 0 = refused at once.
int krunker_ws_open(KrunkerWs *w, const char *url);
// One message (binary or text); 0 = not open (the page's send on a closed socket: dropped).
int krunker_ws_send(KrunkerWs *w, const void *data, size_t len, int binary);
// Closes with 1000 ("client disconnect"); no KRUNKER_WS_CLOSED follows.
void krunker_ws_close(KrunkerWs *w);
int krunker_ws_is_open(const KrunkerWs *w);
// Delivers what arrived since the last call (main thread, once per frame).
void krunker_ws_poll(KrunkerWs *w, KrunkerWsFn fn, void *user);
