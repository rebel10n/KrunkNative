#pragma once
// The placeholder ad's flow (client/include/kn_ad.h), pure so client/tests/kn_ad_test.c drives it without a page.
// v10's window.showAd (deob-10.0.4.js:226713-226742): one ad at a time (iìïiïïï: a show while one runs is dropped);
// FRVR.ads.show resolves COMPLETED -> onShow, onComplete; DELIVERED (skipped) -> onShow, onError("skipped") unless
// allowSkip; anything else -> onError("ad-failed"); onExit always runs last (the finally). KrunkNative has no ad network
// (doc/v10/ads.md): the placeholder counts down, then Continue completes it; closing it before that skips it.

enum { KN_AD_IDLE = 0, KN_AD_COUNTING = 1, KN_AD_READY = 2 };
enum { KN_AD_COMPLETED = 1, KN_AD_SKIPPED = 0 };

// forceVideoAd's minimum gap between two video ads (0x15f90, deob-10.0.4.js:226679)
#define KN_AD_VIDEO_GAP_MS 90000.0
// The placeholder's countdown: v10's ads are FRVR's (no length in the bundle); KRUNKNATIVE_AD_SECONDS overrides it
#define KN_AD_DEFAULT_SECONDS 5

typedef struct {
    int phase;        // KN_AD_IDLE / _COUNTING / _READY
    int seconds_left; // the countdown while COUNTING
    int allow_skip;   // showAd's allowSkip: a skipped ad still completes
} KnAdFlow;

// Starts an ad of `seconds` (0: ready at once). 0 = one is already running (v10 drops it).
int kn_ad_flow_start(KnAdFlow *f, int seconds, int allow_skip);
// One second passed; returns the seconds left (0 once READY).
int kn_ad_flow_tick(KnAdFlow *f);
int kn_ad_flow_running(const KnAdFlow *f);
// Ends it: completed = the Continue after the countdown. KN_AD_COMPLETED when the feature's onComplete runs
// (completed, or skipped with allow_skip), else KN_AD_SKIPPED. Back to IDLE either way.
int kn_ad_flow_end(KnAdFlow *f, int completed);
// forceVideoAd's gate: 1 = a video ad may start at now_ms (the last one at *last_ms, 0 none), and *last_ms is set.
int kn_ad_flow_video_gate(double *last_ms, double now_ms);
