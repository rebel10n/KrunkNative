// The placeholder ad's flow (client/include/kn_ad_flow.h): v10's showAd rules (deob-10.0.5.js:228002-228031) and
// forceVideoAd's 90 s gap (:227964-227981).
#include <kn_ad_flow.h>
#include <stdio.h>

static int g_fail, g_checks;
#define CHECK(c, ...)                                             \
    do {                                                          \
        g_checks++;                                               \
        if (!(c)) {                                               \
            g_fail++;                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while (0)

int main(void) {
    KnAdFlow f = {0};
    CHECK(!kn_ad_flow_running(&f), "idle at first");
    CHECK(kn_ad_flow_start(&f, 3, 0), "an ad starts");
    CHECK(!kn_ad_flow_start(&f, 3, 0), "a second ad while one runs is dropped (iïìíììi)");
    CHECK(f.phase == KN_AD_COUNTING && f.seconds_left == 3, "counting 3");
    CHECK(kn_ad_flow_tick(&f) == 2 && kn_ad_flow_tick(&f) == 1, "the countdown");
    CHECK(f.phase == KN_AD_COUNTING, "still counting at 1");
    CHECK(kn_ad_flow_tick(&f) == 0 && f.phase == KN_AD_READY, "ready at 0");
    CHECK(kn_ad_flow_tick(&f) == 0 && f.phase == KN_AD_READY, "a tick once ready changes nothing");
    CHECK(kn_ad_flow_end(&f, 1) == KN_AD_COMPLETED, "Continue once ready completes (COMPLETED -> onComplete)");
    CHECK(!kn_ad_flow_running(&f), "idle after");

    CHECK(kn_ad_flow_start(&f, 5, 0), "another ad");
    kn_ad_flow_tick(&f);
    CHECK(kn_ad_flow_end(&f, 1) == KN_AD_SKIPPED, "Continue before the countdown ends does not complete it");
    CHECK(kn_ad_flow_start(&f, 5, 0) && kn_ad_flow_end(&f, 0) == KN_AD_SKIPPED, "closing it skips it (onError skipped)");
    CHECK(kn_ad_flow_start(&f, 5, 1) && kn_ad_flow_end(&f, 0) == KN_AD_COMPLETED, "allowSkip: a skipped ad still completes");
    CHECK(kn_ad_flow_start(&f, 0, 0) && f.phase == KN_AD_READY && kn_ad_flow_end(&f, 1) == KN_AD_COMPLETED,
          "0 seconds: ready at once (KRUNKNATIVE_AD_SECONDS=0)");
    CHECK(kn_ad_flow_end(&f, 1) == KN_AD_SKIPPED, "ending an idle flow grants nothing");

    double last = 0;
    const double t0 = 1.7e12; // Date.now()
    CHECK(kn_ad_flow_video_gate(&last, t0) && last == t0, "the first video ad passes");
    CHECK(!kn_ad_flow_video_gate(&last, t0 + KN_AD_VIDEO_GAP_MS - 1), "within 90 s: refused");
    CHECK(last == t0, "a refused one keeps the last time");
    CHECK(kn_ad_flow_video_gate(&last, t0 + KN_AD_VIDEO_GAP_MS), "after 90 s: passes");

    printf("kn_ad_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
