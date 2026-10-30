#pragma once
// KrunkNative's placeholder ad: what plays where v10 plays an FRVR ad (window.showAd / showRewardedAd / forceVideoAd,
// deob-10.0.4.js:226675-226742). No ad network is loaded: a "[insert FRVR ad here]" card counts down
// (KN_AD_DEFAULT_SECONDS), then Continue grants what a watched ad grants. Every ad-gated feature: doc/v10/ads.md.
// Main thread only. The card is #genericPop ("knAdPop"); its handlers knAdContinue / knAdClose (kn_ad_handle).
#include <html_dom.h>
#include <kn_ad_flow.h>

typedef struct {
    void (*on_show)(void *user);                     // the ad is on screen (v10's onShow)
    void (*on_complete)(void *user);                 // watched: grant the feature (onComplete)
    void (*on_error)(void *user, const char *error); // "skipped" (closed early) (onError)
    void (*on_exit)(void *user);                     // always, last (onExit, the finally)
    void *user;
    int allow_skip;
} KnAdRequest;

// window.showAd(opts): 0 = an ad already runs (dropped, as v10's)
int kn_ad_show(const KnAdRequest *req);
// window.showRewardedAd(cb): cb on exit, watched or not (deob-10.0.4.js:226704-226711)
int kn_ad_show_rewarded(void (*cb)(void *user), void *user);
int kn_ad_running(void);

// window.forceVideoAd (deob-10.0.4.js:226675-226692): at most every KN_AD_VIDEO_GAP_MS; state(1) asked, state(2) shown,
// state(3) completed (200 ms later) or state(-1) failed: CUSTOM_VIDEO_AD "vad" [state] to the game server.
typedef void (*KnAdVideoStateFn)(int state);
int kn_ad_force_video(KnAdVideoStateFn send_state);

int kn_ad_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knAdContinue / knAdClose; -1 = not ours
