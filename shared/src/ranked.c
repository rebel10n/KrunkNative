// Krunker v7.1.4 ranked rating rules (shared/include/ranked.h).
#include <ranked.h>
#include <math.h>

const int g_ranked_classes[RANKED_CLASS_COUNT] = {0, 1, 2, 3, 4, 5, 6, 8, 12, 13, 15};

int ranked_class_allowed(const int class_index) {
    for (int i = 0; i < RANKED_CLASS_COUNT; i++) {
        if (g_ranked_classes[i] == class_index) return 1;
    }
    return 0;
}

double ranked_expected(const double rating, const double opponents) {
    return 1.0 / (1.0 + pow(10.0, (opponents - rating) / RANKED_ELO_DIVISOR));
}

double ranked_delta(const double rating, const double opponents, const double score, const int abandoned, const double k) {
    // checkLeave: an abandoner is out of players.list when updateELO runs, so it only loses
    // rankedDeduction
    double delta = abandoned ? -RANKED_DEDUCTION : k * (score - ranked_expected(rating, opponents));
    if (rating + delta < 0.0) delta = -rating;                             // floored at 0
    return delta;
}

int ranked_rank_index(const double rating) {
    int level = (int) floor((rating > 0.0 ? rating : 0.0) / RANKED_ELO_PER);
    if (level > RANKED_MAX_LEVEL) level = RANKED_MAX_LEVEL;
    if (level < 1) level = 1;
    return level - 1;
}

int ranked_rank_number(const double rating) {
    return ranked_rank_index(rating) + 1;
}
