// Krunker v7.1.4 seasons, the Battle Pass and the challenge contexts (shared/src/season.c,
// shared/src/progression.c): itemdata, levels, the daily BPP cap, the per-match BPP, claims,
// level skips, the season badges, challenge item contexts and v7's default-ADD keys, and the
// challenges' REW.bpp. Prints "Test OK".
#include <season.h>
#include <progression.h>
#include <badges.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } } while (0)

static BattlePass g_bp;

static void setup_pass(void) {
    memset(&g_bp, 0, sizeof g_bp);
    g_bp.id = 2;
    g_bp.season = 7;
    g_bp.bpp_per_level = 200;
    g_bp.item_count = 75;
    g_bp.start_s = 1700000000 - (1700000000 % 86400); // a UTC midnight
    g_bp.end_s = g_bp.start_s + 180 * 86400;
    g_bp.tier1_price_kr = 2000;
    g_bp.skip_price_kr = 200;
    // level 1 free 100, level 1 VIP 200, level 2 VIP 300 (no free card), level 3 free 400 (empty VIP card)
    CHECK(bp_parse_items(&g_bp, "20001,0,0,100,21001,1,0,200,21002,1,1,300,20003,0,2,400,21003,1,2,0,bad,1,2,3") == 5,
          "itemdata: 5 items (a junk group dropped), got %d", g_bp.count);
}

static void test_items(void) {
    char text[256];
    bp_format_items(&g_bp, text, sizeof text);
    CHECK(!strcmp(text, "20001,0,0,100,21001,1,0,200,21002,1,1,300,20003,0,2,400,21003,1,2,0"), "format round trip: %s", text);
    CHECK(bp_item(&g_bp, 21002) && bp_item(&g_bp, 21002)->level == 1 && bp_item(&g_bp, 21002)->tier == 1, "bp_item");
    CHECK(bp_item_at(&g_bp, 0, 2) && bp_item_at(&g_bp, 0, 2)->skin == 400 && !bp_item_at(&g_bp, 0, 1), "bp_item_at");
}

static void test_levels(void) {
    int64_t pts = -1;
    int complete = -1;
    CHECK(bp_level(&g_bp, 0, &pts, &complete) == 1 && pts == 0 && !complete, "bpp 0: level 1, 0 points");
    CHECK(bp_level(&g_bp, 399, &pts, &complete) == 2 && pts == 199, "bpp 399: level 2, 199/200");
    CHECK(bp_max_bpp(&g_bp) == 15000, "75 x 200 = 15000 (the Battle Pass Master threshold)");
    CHECK(bp_level(&g_bp, 14999, &pts, &complete) == 75 && !complete, "bpp 14999: level 75");
    CHECK(bp_level(&g_bp, 15000, &pts, &complete) == 75 && complete && pts == 200, "bpp 15000: complete, capped at 75 with 200/200");
}

// v10.0.2 leveldata / krdata / jnkdata (_52102 / _52111)
static void test_v10_levels(void) {
    BattlePass bp;
    memset(&bp, 0, sizeof bp);
    bp.bpp_per_level = 300;
    bp.item_count = 4;
    bp_parse_items(&bp, "20001,0,0,0,20002,0,1,5,20003,0,2,0");
    bp_parse_levels(&bp, "2:1000,3:0,9:5,x");
    bp_parse_rewards(&bp, "20001:150,20009:5", "20003:2.5");
    CHECK(bp_level_need(&bp, 1) == 300 && bp_level_need(&bp, 2) == 1000 && bp_level_need(&bp, 3) == 0 && bp_level_need(&bp, 4) == 300,
          "level costs: 300, 1000, 0, 300");
    int64_t xp = 0;
    int complete = 0;
    CHECK(bp_level(&bp, 299, &xp, &complete) == 1 && xp == 299, "299: level 1");
    CHECK(bp_level(&bp, 1299, &xp, NULL) == 2 && xp == 999, "1299: level 2 (999/1000)");
    CHECK(bp_level(&bp, 1300, &xp, NULL) == 4 && xp == 0, "1300: level 3 costs nothing, level 4");
    CHECK(bp_level(&bp, 1600, &xp, &complete) == 4 && complete && xp == 300, "1600: maxed");
    CHECK(bp_max_bpp(&bp) == 1600, "the pass total: 1600");
    CHECK(bp_skip_bpp(&bp, 100, 1) == 400 && bp_skip_bpp(&bp, 400, 1) == 1400, "a skip adds the skipped level's cost");
    CHECK(bp_item(&bp, 20001)->kr == 150 && bp_item(&bp, 20003)->jnk == 2.5 && bp_item(&bp, 20002)->kr == 0, "krdata / jnkdata");
    CHECK(bp_claim_check(&bp, 0, 0, "", 20001) == BP_CLAIM_OK && bp_claim_check(&bp, 1600, 0, "", 20003) == BP_CLAIM_OK,
          "a KR card and a junk card are claimable without a skin");
}

// endGame's per-match BPP (v10.0.3 deob 221757-221776): public matches only; min(20, floor(score *
// (rewardMlt || 1) * 0.01)) and the play term, which is off because isEmptyLobby is never false.
static void test_match_points(void) {
    const double clock = 240000.0; // a 4 minute clock (gameTime, ms)
    BpMatchInput in = {.score = 1500, .reward_mlt = 1, .pub_type = 1, .empty_lobby = 1, .play_ms = clock, .elapsed_ms = clock};
    CHECK(bp_match_points(&in) == 15, "public, all of the clock played: the score part only (15), got %lld", (long long) bp_match_points(&in));
    in.play_ms = 0.6 * clock;
    CHECK(bp_match_points(&in) == 15, "public, 60%% played: still 15 (the play term is off), got %lld", (long long) bp_match_points(&in));
    in.score = 9000;
    CHECK(bp_match_points(&in) == 20, "min(20, ..): 20");
    in.score = 900;
    in.reward_mlt = 2;
    CHECK(bp_match_points(&in) == 18, "rewardMlt 2: floor(900 * 2 * 0.01) = 18");
    in.reward_mlt = 0;
    CHECK(bp_match_points(&in) == 9, "rewardMlt 0 counts as 1: 9");

    // ranked and custom (hosted) matches are not public: no BPP at all
    BpMatchInput hosted = {.score = 9000, .reward_mlt = 1, .pub_type = 0, .empty_lobby = 1, .play_ms = clock, .elapsed_ms = clock};
    CHECK(bp_match_points(&hosted) == 0, "ranked / custom: no match BPP");
    hosted.empty_lobby = 0;
    CHECK(bp_match_points(&hosted) == 0, "ranked / custom with the play term on: still none");

    // the play term, as the formula reads (empty_lobby 0 is not what 10.0.3 does; it documents the term)
    BpMatchInput term = {.score = 1500, .reward_mlt = 1, .pub_type = 1, .empty_lobby = 0, .play_ms = 0.6 * clock, .elapsed_ms = clock};
    CHECK(bp_match_points(&term) == 15 + 18, "60%%: floor(0.6 * 10) / 10 = 0.6 x 30 = 18, got %lld", (long long) bp_match_points(&term));
    term.play_ms = 0.25 * clock;
    CHECK(bp_match_points(&term) == 15 + 6, "25%%: floor(2.5) / 10 = 0.2 x 30 = 6, got %lld", (long long) bp_match_points(&term));
    term.play_ms = 1.5 * clock;
    CHECK(bp_match_points(&term) == 15 + 30, "over the clock: capped at 30");
    term.raid = 1;
    CHECK(bp_match_points(&term) == 15, "raid: no play term");
    term.raid = 0;
    term.play_ms = 0;
    term.elapsed_ms = 0;
    CHECK(bp_match_points(&term) == 15, "0 over a 0 clock: the NaN term counts as 0");
}

static void test_cap(void) {
    const int64_t day0 = g_bp.start_s + 3600, day2 = g_bp.start_s + 2 * 86400 + 5;
    CHECK(BP_CAP_DAY == 500 && bp_cap_total(&g_bp, day0) == BP_CAP_DAY, "day 1: 500 (v10.0.2)");
    CHECK(bp_cap_total(&g_bp, day2) == 3 * BP_CAP_DAY, "day 3: 1500");
    CHECK(bp_cap_total(&g_bp, g_bp.start_s - 10) == 0, "before the start: 0");
    CHECK(bp_available(&g_bp, 450, day0) == 50 && bp_available(&g_bp, 600, day0) == 0, "Available BPP");
    BpAward a = bp_validate_progress(&g_bp, 460, 50, day0);
    CHECK(a.award == 40 && a.remaining == 0, "validateProgress: capped at the day's 500 (award %lld rem %lld)", (long long) a.award, (long long) a.remaining);
    a = bp_validate_progress(&g_bp, 100, 50, day2);
    CHECK(a.award == 50 && a.remaining == 1350, "validateProgress on day 3 (award %lld rem %lld)", (long long) a.award, (long long) a.remaining);
    a = bp_validate_progress(&g_bp, 14990, 50, g_bp.start_s + 100 * 86400);
    CHECK(a.award == 10 && a.remaining == 0, "the pass total caps too");
    test_match_points();
}

static void test_rewards_and_claims(void) {
    const BattlePassItem *re = bp_next_reward(&g_bp, 0); // level 1 -> the next column is 1 (level 2)
    CHECK(re && re->id == 21002, "next reward: level 2's VIP card");
    re = bp_next_reward(&g_bp, 200); // level 2 -> column 2: VIP card empty, the free one
    CHECK(re && re->id == 20003, "next reward: the free card when the VIP one is empty");
    CHECK(!bp_next_reward(&g_bp, 15000), "complete: none");
    CHECK(bp_claim_check(&g_bp, 0, 0, "", 20001) == BP_CLAIM_OK, "level 1 free: claimable at bpp 0");
    CHECK(bp_claim_check(&g_bp, 0, 0, "", 21001) == BP_CLAIM_TIER, "VIP card without the tier");
    CHECK(bp_claim_check(&g_bp, 0, 1, "", 21001) == BP_CLAIM_OK, "VIP card with the tier");
    CHECK(bp_claim_check(&g_bp, 199, 1, "", 21002) == BP_CLAIM_LEVEL, "level 2 at bpp 199");
    CHECK(bp_claim_check(&g_bp, 200, 1, "", 21002) == BP_CLAIM_OK, "level 2 at bpp 200");
    CHECK(bp_claim_check(&g_bp, 0, 0, "5,20001", 20001) == BP_CLAIM_CLAIMED, "claimed");
    CHECK(bp_claim_check(&g_bp, 999, 1, "", 21003) == BP_CLAIM_NOT_FOUND && bp_claim_check(&g_bp, 999, 1, "", 7) == BP_CLAIM_NOT_FOUND,
          "empty card / unknown id");
    CHECK(bp_is_claimed("200011,20001", 20001) && !bp_is_claimed("200011", 20001) && !bp_is_claimed("", 1), "items_claimed split");
    char next[64];
    bp_add_claimed("", 7, next, sizeof next);
    CHECK(!strcmp(next, "7"), "first claim");
    bp_add_claimed("7", 9, next, sizeof next);
    CHECK(!strcmp(next, "7,9"), "next claim");
    CHECK(bp_skip_max(&g_bp, 0) == 74 && bp_skip_max(&g_bp, 14999) == 1 && bp_skip_max(&g_bp, 15000) == 1, "increaseSkip limit");
    CHECK(bp_skip_bpp(&g_bp, 150, 3) == 750 && bp_skip_bpp(&g_bp, 14900, 5) == 15000, "skipped levels add bpp_perlevel each, capped");
}

static int g_owned_skins;
static int has_skin(void *ud, const int skin) {
    (void) ud;
    return g_owned_skins && skin >= 5237 && skin <= 5253;
}

static void test_badges(void) {
    CHECK(BADGE_COUNT == 29 && !strcmp(g_badges[26].name, "Season 7 Founder") && !strcmp(g_badges[28].name, "S7 Badge Master"), "badges.js order");
    int owned[4] = {0};
    SeasonBadgeAccount a = {.season = 7, .now_s = g_bp.start_s + 86400, .bp_start_s = g_bp.start_s, .badges = owned, .badge_count = 0};
    CHECK(season_badge_rule(26, &a) == 0, "Founder: no VIP tier");
    a.bp_tier = 1;
    a.bp_tier_at_s = g_bp.start_s + 6 * 86400;
    CHECK(season_badge_rule(26, &a) == 1, "Founder: VIP bought on day 7");
    a.bp_tier_at_s = g_bp.start_s + 8 * 86400;
    CHECK(season_badge_rule(26, &a) == 0, "Founder: VIP bought on day 9");
    CHECK(season_badge_rule(22, &a) == 0 && season_badge_rule(24, &a) == 0, "Season 6 badges in season 7");
    a.bpp = 14999;
    CHECK(season_badge_rule(27, &a) == 0, "S7 Battle Pass Master: 14999");
    a.bpp = BP_MASTER_BPP;
    CHECK(season_badge_rule(27, &a) == 1 && badge_rule_met(27, NULL, &a) == 1, "S7 Battle Pass Master: VIP + 15000");
    a.bp_tier = 0;
    CHECK(season_badge_rule(27, &a) == 0, "S7 Battle Pass Master needs the VIP tier");
    owned[0] = 26;
    owned[1] = 27;
    a.badge_count = 2;
    CHECK(season_badge_rule(28, &a) == 1, "S7 Badge Master: 26 + 27");
    a.badge_count = 1;
    CHECK(season_badge_rule(28, &a) == 0, "S7 Badge Master: 26 only");
    SeasonBadgeAccount s6 = {.season = 6, .has_skin = has_skin};
    CHECK(season_badge_rule(23, &s6) == 0, "S6 Map Master without the skins");
    g_owned_skins = 1;
    CHECK(season_badge_rule(23, &s6) == 1, "S6 Map Master with the 17 map reward skins");
    CHECK(season_badge_rule(11, &a) == -1 && !badge_requirement_met(27, NULL), "not season badges / no account facts");
    cJSON *stats = cJSON_Parse("{\"n\":510}");
    CHECK(badge_rule_met(11, stats, &a) == 1, "Nuclear I through badge_rule_met");
    cJSON_Delete(stats);
}

static void test_challenge_contexts(void) {
    CHECK(challenge_key_kind("dym") == CHALLENGE_KEY_ADD && challenge_key_kind("gifts") == CHALLENGE_KEY_ADD &&
          challenge_key_kind("myMapKey") == CHALLENGE_KEY_ADD && challenge_key_kind("") == CHALLENGE_KEY_UNKNOWN &&
          challenge_key_kind(NULL) == CHALLENGE_KEY_UNKNOWN && challenge_key_kind("uwk") == CHALLENGE_KEY_MAX,
          "v7 checkChallenge: keys outside the lists accumulate");
    cJSON *active = cJSON_Parse(
        "[{\"CI\":1,\"T\":\"daily\",\"DAT\":{\"ky\":\"spry\",\"val\":2,\"item\":55},\"REW\":{\"bpp\":40,\"kr\":5},\"PV\":0,\"PT\":0},"
        " {\"CI\":2,\"T\":\"daily\",\"DAT\":{\"ky\":\"find\",\"val\":1,\"item\":6254},\"REW\":{\"bpp\":25},\"PV\":0,\"PT\":0},"
        " {\"CI\":3,\"T\":\"weapon\",\"DAT\":{\"ky\":\"uwk\",\"val\":3},\"REW\":{\"kr\":10},\"PV\":0,\"PT\":0},"
        " {\"CI\":4,\"T\":\"event\",\"DAT\":{\"ky\":\"gifts\",\"val\":2,\"evt\":3},\"REW\":{},\"PV\":0,\"PT\":0},"
        " {\"CI\":5,\"T\":\"daily\",\"DAT\":{\"ky\":\"spry\",\"val\":1},\"REW\":{},\"PV\":0,\"PT\":0}]");
    cJSON *events = cJSON_Parse(
        "[[\"spry\",1,-1,-1,55],[\"spry\",1,-1,-1,56],[\"spry\",1,-1,-1,55],[\"find\",1,-1,-1,6000],"
        " [\"uwk\",2,-1,-1,-1],[\"uwk\",3,-1,-1,-1],[\"gifts\",1,-1,-1],[\"gifts\",1,-1,-1,-1]]");
    cJSON *results = cJSON_CreateArray(), *completed = cJSON_CreateArray(), *reward = cJSON_CreateObject();
    const ChallengeMatch match = {.map_index = 0, .map_id = -1};
    challenges_finalize(active, events, &match, results, completed, reward);
    char *text = cJSON_PrintUnformatted(results);
    // 1: spry with spray 55 twice (done); 2: find 6000 is not 6254 (untouched); 3: uwk max 3 (done);
    // 4: "gifts" accumulates (2 rows, one without the item column; done); 5: spry with any spray: 3 >= 1
    CHECK(text && !strcmp(text, "[1,1,2,1,3,3,1,4,2,1,5,3]"), "contexts: %s", text ? text : "");
    free(text);
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(reward, "bpp")) == 40 && cJSON_GetNumberValue(cJSON_GetObjectItem(reward, "kr")) == 15,
          "REW.bpp summed (40), KR 15");
    cJSON_Delete(active);
    cJSON_Delete(events);
    cJSON_Delete(results);
    cJSON_Delete(completed);
    cJSON_Delete(reward);
}

int main(void) {
    setup_pass();
    test_items();
    test_levels();
    test_cap();
    test_v10_levels();
    test_rewards_and_claims();
    test_badges();
    test_challenge_contexts();
    printf("Test OK\n");
    return 0;
}
