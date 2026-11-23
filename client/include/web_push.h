#pragma once
// Web Push for the menu's Notifications panel (v10_menu.c, Krunker v10's webpush component svelte-5kyah8,
// deob-10.0.5.js:146795-146806, :148622-148860): v10's stores $isWebPushSupported, $webPushPermission,
// $isWebPushSubscribed, $webPushLoading and $webPushError over the browser's subscription (html_dom.h, the
// page's service worker ui/sw.js) and the account's record of it:
//   krunker.io account: v10.0.7's gapi POST /webpush/subscribe {subscription}, /unsubscribe {registration_id,
//     endpoint}, /status {endpoint} (asked when the panel mounts) and /subscription-change {old_subscription,
//     new_subscription} for the service worker's pushsubscriptionchange (deob-10.0.7.js:26455-26483,
//     148910-149027), with krunker.io's VAPID key (:19502).
//   Box Office: the brain's /api/push/subscribe, /unsubscribe (services/brain/push.c, KrunkNative's stand-in for
//     those routes), its VAPID key from the brain.
//
// KrunkNative differences (intentional): the brain records a subscription for a signed-in account only, so a
// guest's subscribe ends as v10's does when its server refuses one (the browser lets go, no error shown). On
// Box Office "subscribed" is this browser's last successful subscribe, stored per browser (knWebPush). v10 asks
// /webpush/status on mount, on focus and every 30 min; this client on mount only. The brain sends no pushes yet.
// Native builds: no Push API (web_push_supported() is 0, v10's unsupported state).
typedef void (*WebPushChangedFn)(void);

int         web_push_supported(void);   // $isWebPushSupported: the web build with the Push API (secure context)
int         web_push_denied(void);      // $webPushPermission === "denied"
int         web_push_subscribed(void);  // $isWebPushSubscribed
int         web_push_loading(void);     // $webPushLoading: a subscribe / unsubscribe in flight
const char *web_push_error(void);       // $webPushError (NULL: none)
void        web_push_subscribe(void);   // the Enable Notifications button
void        web_push_unsubscribe(void); // the Disable button
void        web_push_set_listener(WebPushChangedFn fn); // called when a store changes (NULL: none)
