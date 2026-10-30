// The placeholder ad's flow (client/include/kn_ad_flow.h)
#include <kn_ad_flow.h>

int kn_ad_flow_start(KnAdFlow *f, const int seconds, const int allow_skip) {
    if (!f || f->phase != KN_AD_IDLE) return 0; // iìïiïïï: one ad at a time
    f->seconds_left = seconds > 0 ? seconds : 0;
    f->phase = f->seconds_left > 0 ? KN_AD_COUNTING : KN_AD_READY;
    f->allow_skip = allow_skip != 0;
    return 1;
}

int kn_ad_flow_tick(KnAdFlow *f) {
    if (!f || f->phase != KN_AD_COUNTING) return 0;
    if (--f->seconds_left <= 0) {
        f->seconds_left = 0;
        f->phase = KN_AD_READY;
    }
    return f->seconds_left;
}

int kn_ad_flow_running(const KnAdFlow *f) { return f && f->phase != KN_AD_IDLE; }

int kn_ad_flow_end(KnAdFlow *f, const int completed) {
    if (!f || f->phase == KN_AD_IDLE) return KN_AD_SKIPPED;
    const int done = (completed && f->phase == KN_AD_READY) || f->allow_skip; // COMPLETED, or DELIVERED with allowSkip
    f->phase = KN_AD_IDLE;
    f->seconds_left = 0;
    return done ? KN_AD_COMPLETED : KN_AD_SKIPPED;
}

int kn_ad_flow_video_gate(double *last_ms, const double now_ms) {
    if (!last_ms || now_ms - *last_ms < KN_AD_VIDEO_GAP_MS) return 0; // iïÍîiïï - (iìiíîíi || 0) >= 0x15f90
    *last_ms = now_ms;
    return 1;
}
