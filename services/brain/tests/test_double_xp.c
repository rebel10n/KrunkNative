/* Unit test for services/brain/double_xp.c: v10's schedules (weekend, even ISO week), the per-match
 * offer gate (games % 3, once per games count), the class award and the placeholder provider.
 * Pure: no database; the dates are fixed instants (UTC noon). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "double_xp.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define DAY_MS 86400000LL

static int64_t at(int64_t y, unsigned m, unsigned d) {
    return (dxp_days_from_civil(y, m, d) * DAY_MS) + DAY_MS / 2; /* noon UTC */
}

int main(void) {
    /* civil dates: 1970-01-01 is day 0; 2000-03-01 is 11017 */
    CHECK(dxp_days_from_civil(1970, 1, 1) == 0 && dxp_days_from_civil(2000, 3, 1) == 11017);

    /* weekdays: 2026-10-17 is a Saturday (6), 2026-10-18 a Sunday (0), 2026-10-12 a Monday (1) */
    CHECK(dxp_weekday(at(2026, 10, 17)) == 6 && dxp_weekday(at(2026, 10, 18)) == 0 && dxp_weekday(at(2026, 10, 12)) == 1);
    CHECK(dxp_weekday(0) == 4); /* 1970-01-01, a Thursday */

    /* ISO weeks, checked against Python's isocalendar: the year boundaries are the hard cases */
    CHECK(dxp_iso_week(at(2026, 10, 17)) == 42 && dxp_iso_week(at(2026, 10, 18)) == 42);
    CHECK(dxp_iso_week(at(2026, 10, 10)) == 41 && dxp_iso_week(at(2026, 10, 12)) == 42);
    CHECK(dxp_iso_week(at(2021, 1, 2)) == 53 && dxp_iso_week(at(2021, 1, 3)) == 53); /* 2020-W53 */
    CHECK(dxp_iso_week(at(2020, 12, 26)) == 52 && dxp_iso_week(at(2026, 1, 1)) == 1);
    CHECK(dxp_iso_week(at(2027, 1, 1)) == 53); /* 2027-01-01 belongs to 2026-W53 */
    CHECK(dxp_iso_week(at(2024, 12, 28)) == 52);

    /* schedules: class XP is Saturday and Sunday of an even ISO week; raid drops every weekend day */
    DxpSchedule s = dxp_schedule(at(2026, 10, 17));
    CHECK(s.double_class_xp == 1 && s.double_raid_drops == 1);
    s = dxp_schedule(at(2026, 10, 18));
    CHECK(s.double_class_xp == 1 && s.double_raid_drops == 1);
    s = dxp_schedule(at(2026, 10, 10)); /* odd week 41 */
    CHECK(s.double_class_xp == 0 && s.double_raid_drops == 1);
    s = dxp_schedule(at(2026, 10, 12)); /* Monday */
    CHECK(s.double_class_xp == 0 && s.double_raid_drops == 0);
    s = dxp_schedule(at(2021, 1, 2)); /* Saturday, week 53 (odd) */
    CHECK(s.double_class_xp == 0 && s.double_raid_drops == 1);
    s = dxp_schedule(at(2020, 12, 26)); /* Saturday, week 52 (even) */
    CHECK(s.double_class_xp == 1 && s.double_raid_drops == 1);

    /* the class award: the XP itself, doubled while doubleClassXP applies; never over the column */
    CHECK(dxp_class_xp(100, 0) == 100 && dxp_class_xp(100, 1) == 200);
    CHECK(dxp_class_xp(-5, 1) == 0 && dxp_class_xp(0, 1) == 0);
    CHECK(dxp_class_xp(DXP_XP_MAX, 1) == DXP_XP_MAX);
    CHECK(dxp_match_xp(-1) == 0 && dxp_match_xp(42) == 42 && dxp_match_xp(DXP_XP_MAX + 9) == DXP_XP_MAX);

    /* the offer gate: games % 3 == 0 and above zero; the offer is the account's last match; once */
    CHECK(dxp_offer_claimable(1, 3, 3, 0));
    CHECK(dxp_offer_claimable(1, 6, 6, 3));           /* last claim was 3 games ago */
    CHECK(!dxp_offer_claimable(1, 1, 1, 0));          /* not a third match */
    CHECK(!dxp_offer_claimable(1, 4, 4, 0));
    CHECK(!dxp_offer_claimable(1, 0, 0, 0));          /* no game yet */
    CHECK(!dxp_offer_claimable(0, 3, 3, 0));          /* no offer (custom, comp, partial) */
    CHECK(!dxp_offer_claimable(1, 3, 2, 0));          /* the offer is an older match */
    CHECK(!dxp_offer_claimable(1, 3, 3, 3));          /* already claimed for these games */

    /* the placeholder: granted at once, no proof; off refuses; unknown names are off */
    CHECK(dxp_provider_parse("placeholder") == DXP_PROVIDER_PLACEHOLDER);
    CHECK(dxp_provider_parse("off") == DXP_PROVIDER_OFF && dxp_provider_parse("") == DXP_PROVIDER_OFF);
    CHECK(dxp_provider_parse(NULL) == DXP_PROVIDER_OFF && dxp_provider_parse("adsdk") == DXP_PROVIDER_OFF);
    CHECK(dxp_provider_verify(DXP_PROVIDER_PLACEHOLDER, NULL) == DXP_VERDICT_GRANTED);
    CHECK(dxp_provider_verify(DXP_PROVIDER_PLACEHOLDER, "x") == DXP_VERDICT_GRANTED);
    CHECK(dxp_provider_verify(DXP_PROVIDER_OFF, NULL) == DXP_VERDICT_REFUSED);
    CHECK(!strcmp(dxp_provider_name(DXP_PROVIDER_PLACEHOLDER), "placeholder") && !strcmp(dxp_provider_name(DXP_PROVIDER_OFF), "off"));

    if (g_fail) {
        fprintf(stderr, "double_xp test: FAILED\n");
        return 1;
    }
    printf("double_xp test: ok\n");
    return 0;
}
