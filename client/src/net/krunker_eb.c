// krunker.io's friends event bus socket (client/include/krunker_eb.h): a text WebSocket over krunker_ws.h
// (natively a thread through the client's proxy, on the web the browser's WebSocket).
#include <krunker_eb.h>
#include <krunker_ws.h>
#include <stddef.h>
#include <string.h>

static KrunkerWs *g_ws;

static KrunkerWs *ws(void) {
    if (!g_ws) g_ws = krunker_ws_new("krunker-eb");
    return g_ws;
}

typedef struct {
    KrunkerEbFn fn;
    void *user;
} EbPoll;

static void on_event(void *user, const int event, const void *data, const size_t len, const int binary, const int code) {
    (void) len;
    const EbPoll *p = user;
    if (event == KRUNKER_WS_MESSAGE && binary) return; // v10: only string messages are parsed
    const int ev = event == KRUNKER_WS_OPEN ? KRUNKER_EB_OPEN : event == KRUNKER_WS_MESSAGE ? KRUNKER_EB_MESSAGE : KRUNKER_EB_CLOSED;
    if (p->fn) p->fn(p->user, ev, event == KRUNKER_WS_MESSAGE ? (const char *) data : NULL, code);
}

void krunker_eb_poll(const KrunkerEbFn fn, void *user) {
    if (!g_ws) return;
    EbPoll p = {fn, user};
    krunker_ws_poll(g_ws, on_event, &p);
}

int krunker_eb_is_open(void) { return krunker_ws_is_open(g_ws); }
int krunker_eb_open(const char *url) { return krunker_ws_open(ws(), url); }
int krunker_eb_send(const char *text) { return text && krunker_ws_send(g_ws, text, strlen(text), 0); }
void krunker_eb_close(void) { krunker_ws_close(g_ws); }
