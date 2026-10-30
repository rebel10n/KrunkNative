#pragma once
// Web Push for the menu's Notifications panel (v10_menu.c, Krunker v10's webpush component svelte-5kyah8,
// deob-10.0.5.js:146795-146806, :148622-148860): v10's stores $isWebPushSupported, $webPushPermission,
// $isWebPushSubscribed, $webPushLoading and $webPushError over the browser's subscription (html_dom.h, the
// page's service worker ui/sw.js) and the brain's record of it (/api/push/subscribe, /unsubscribe;
// services/brain/push.c, KrunkNative's stand-in for v10's /webpush/subscribe and /unsubscribe).
//
// KrunkNative differences (intentional): the brain records a subscription for a signed-in account only, so a
// guest's subscribe ends as v10's does when its server refuses one (the browser lets go, no error shown). The
// VAPID key comes from the brain (v10: config.webpushVapidPublicKey). v10 asks its server for the
// subscription's status (/webpush/status) when the panel mounts, on focus and every 30 min; the brain answers
// that route for v10's API client (push.c, v10_account.c) but this client does not ask it yet, so
// "subscribed" is this browser's last successful subscribe, stored per browser (knWebPush). The brain does
// not send pushes yet. Native builds: no Push API (web_push_supported() is 0, v10's unsupported state).
typedef void (*WebPushChangedFn)(void);

int         web_push_supported(void);   // $isWebPushSupported: the web build with the Push API (secure context)
int         web_push_denied(void);      // $webPushPermission === "denied"
int         web_push_subscribed(void);  // $isWebPushSubscribed
int         web_push_loading(void);     // $webPushLoading: a subscribe / unsubscribe in flight
const char *web_push_error(void);       // $webPushError (NULL: none)
void        web_push_subscribe(void);   // the Enable Notifications button
void        web_push_unsubscribe(void); // the Disable button
void        web_push_set_listener(WebPushChangedFn fn); // called when a store changes (NULL: none)
