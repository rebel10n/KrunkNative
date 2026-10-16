#pragma once
// krunker.io's friends event bus (v10.0.2 _49718, v10:139770): one text WebSocket to
// <EVENT_BUS_URI>/ws?at=<the account's fresh access token> carrying JSON messages both ways
// (friends:list, presence:*, msg:*, unread:summary, error). The client opens it while krunker.io is
// the active account backend (backend_ui.h) and the krunker.io account is signed in; v10_social.c
// drives it exactly like kn_brain's ["eb", {...}] frames. Natively the socket goes through the
// client's proxy (http_conn.h / http_proxy.h: the krunker.io scope covers *.krunker.io) with
// Origin: https://krunker.io like the game socket; on the web it is the browser's WebSocket. The
// Windows build has no WebSocket client for it (krunker_eb_open refuses, like krunker_net).
//
// Main thread only. The URL (it carries the token) is never logged.

enum {
    KRUNKER_EB_OPEN = 0,   // onopen
    KRUNKER_EB_MESSAGE,    // a text message (text = the JSON)
    KRUNKER_EB_CLOSED,     // onclose (code = the close code; 1006 = dropped / never opened)
};
typedef void (*KrunkerEbFn)(void *user, int event, const char *text, int code);

// Opens the socket (an open one is closed first, without a KRUNKER_EB_CLOSED). 0 = refused at once.
int  krunker_eb_open(const char *url);
// One text message; 0 = not open (v10 _49728: dropped).
int  krunker_eb_send(const char *text);
// Closes with 1000 ("client disconnect", v10's logout); no KRUNKER_EB_CLOSED follows.
void krunker_eb_close(void);
int  krunker_eb_is_open(void);
// Delivers what arrived since the last call (main thread, once per frame).
void krunker_eb_poll(KrunkerEbFn fn, void *user);
