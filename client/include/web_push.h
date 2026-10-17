#pragma once
// Web Push for the web build's Notifications switch (the menu's Notifications toggle, v10_menu.c): the
// browser's subscription (html_dom.h: the page's service worker, ui/sw.js), the brain's record of it
// (/api/push/subscribe, /unsubscribe; services/brain/push.c) and the switch's stored choice. This is the
// subscribe side only: the brain does not send pushes yet. Client state lives here (client/src/ui).
//
// Signed in: a subscription is recorded with the brain. A guest gets the browser subscription and the
// caption "Sign in to get push notifications"; the next time the panel opens (web_push_refresh) it is
// recorded once the account is signed in. Native builds: no Web Push (web_push_supported() is 0).
typedef void (*WebPushChangedFn)(void);

int         web_push_supported(void);  // 1 on the web when the browser has the Push API (secure context)
int         web_push_on(void);         // the switch's stored choice (per browser, not account state); 0 when unsupported
void        web_push_set(int on);      // the switch: on subscribes this browser; off unsubscribes it (and the brain's row)
void        web_push_refresh(void);    // when on: subscribes again and records it (a sign-in since the switch was turned on)
const char *web_push_status(void);     // the caption under the switch
void        web_push_set_listener(WebPushChangedFn fn); // called when an answer changes the caption (NULL: none)
