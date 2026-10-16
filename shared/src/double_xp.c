/*
 * shared/src/double_xp.c - Krunker v10.0.2's double XP rules (double_xp.h).
 */
#include "double_xp.h"

#include <string.h>

#define MS_PER_DAY 86400000LL

/* Howard Hinnant's civil-date algorithms (days since 1970-01-01, proleptic Gregorian). */
int64_t dxp_days_from_civil(int64_t y, const unsigned m, const unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t) doe - 719468;
}

static int64_t year_of_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned) (z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return (int64_t) yoe + era * 400 + (m <= 2);
}

static int64_t floor_days(const int64_t ms) {
    return ms >= 0 ? ms / MS_PER_DAY : -((-ms + MS_PER_DAY - 1) / MS_PER_DAY);
}

int dxp_weekday(const int64_t now_ms) {
    /* 1970-01-01 was a Thursday (4) */
    const int64_t d = floor_days(now_ms);
    return (int) (((d + 4) % 7 + 7) % 7);
}

int dxp_iso_week(const int64_t now_ms) {
    /* the Thursday of this ISO week decides the year; the week is its ordinal in that year */
    const int64_t d = floor_days(now_ms);
    const int64_t mon0 = ((d + 3) % 7 + 7) % 7; /* Monday = 0 */
    const int64_t thursday = d - mon0 + 3;
    const int64_t year = year_of_days(thursday);
    const int64_t jan1 = dxp_days_from_civil(year, 1, 1);
    return (int) ((thursday - jan1) / 7 + 1);
}

DxpSchedule dxp_schedule(const int64_t now_ms) {
    DxpSchedule s;
    const int wd = dxp_weekday(now_ms);
    const int weekend = wd == 0 || wd == 6;
    s.double_raid_drops = weekend;
    s.double_class_xp = weekend && dxp_iso_week(now_ms) % 2 == 0;
    return s;
}

int64_t dxp_match_xp(const int64_t xp) {
    return xp < 0 ? 0 : xp > DXP_XP_MAX ? DXP_XP_MAX : xp;
}

int64_t dxp_class_xp(const int64_t xp, const int double_class_xp) {
    const int64_t base = dxp_match_xp(xp);
    return double_class_xp ? dxp_match_xp(base * 2) : base;
}

int dxp_offer_claimable(const int offered, const int64_t games, const int64_t offer_games, const int64_t claimed_games) {
    return offered && games > 0 && games % DXP_CLAIM_EVERY == 0 && offer_games == games && claimed_games != games;
}

DxpProvider dxp_provider_parse(const char *name) {
    return name && !strcmp(name, "placeholder") ? DXP_PROVIDER_PLACEHOLDER : DXP_PROVIDER_OFF;
}

const char *dxp_provider_name(const DxpProvider provider) {
    return provider == DXP_PROVIDER_PLACEHOLDER ? "placeholder" : "off";
}

DxpVerdict dxp_provider_verify(const DxpProvider provider, const char *ad_ref) {
    (void) ad_ref; /* the placeholder watches nothing: there is no proof to check */
    return provider == DXP_PROVIDER_PLACEHOLDER ? DXP_VERDICT_GRANTED : DXP_VERDICT_REFUSED;
}
