// client/include/follower_counts.h (v10_social.c followers_moved): a follow moves the author's follower
// count on every post by them and on their profile, so a post and the profile agree ("follower counts
// on posts not matching the profile", Krunker v10.0.3).
//   cmake-build-debug/follower_counts_test
#include <follower_counts.h>

#include <stdio.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double flw(const cJSON *p) {
    const cJSON *f = cJSON_GetObjectItem(p, "flw");
    return cJSON_IsNumber(f) ? f->valuedouble : -1;
}

int main(void) {
    // a feed: two posts by "ann" (flw 10), one by "bob" (flw 3)
    cJSON *feed = cJSON_Parse("[{\"pln\":\"ann\",\"flw\":10},{\"pln\":\"bob\",\"flw\":3},{\"pln\":\"ann\",\"flw\":10}]");
    CHECK(follower_count_bump_posts(feed, "ann", 1) == 2, "two posts by ann changed");
    cJSON *a0 = cJSON_GetArrayItem(feed, 0), *b = cJSON_GetArrayItem(feed, 1), *a1 = cJSON_GetArrayItem(feed, 2);
    CHECK(flw(a0) == 11 && flw(a1) == 11, "ann's posts now 11 (got %g, %g)", flw(a0), flw(a1));
    CHECK(flw(b) == 3, "bob's post is untouched (got %g)", flw(b));

    // a single post view's post
    cJSON *one = cJSON_Parse("{\"pln\":\"ann\",\"flw\":10}");
    CHECK(follower_count_bump_post(one, "ann", -1) == 1 && flw(one) == 9, "unfollow takes one off (got %g)", flw(one));
    CHECK(follower_count_bump_post(one, "bob", 1) == 0 && flw(one) == 9, "a post by someone else is not changed");
    CHECK(follower_count_bump_post(NULL, "ann", 1) == 0, "NULL post is ignored");
    cJSON_Delete(one);

    // the profile view: its player_followed follows the follow
    cJSON *profile = cJSON_Parse("{\"player_name\":\"ann\",\"player_followed\":40,\"follow_state\":1}");
    CHECK(follower_count_bump_profile(profile, "ann", 1) == 1, "profile is ann's");
    const cJSON *pf = cJSON_GetObjectItem(profile, "player_followed");
    CHECK(pf && pf->valuedouble == 41, "profile count 41 (got %g)", pf ? pf->valuedouble : -1);
    CHECK(follower_count_bump_profile(profile, "bob", 1) == 0, "another player's profile is not changed");
    cJSON_Delete(profile);

    // never below zero (a stale count of 0 unfollowed)
    cJSON *zero = cJSON_Parse("{\"player_name\":\"ann\",\"player_followed\":0}");
    follower_count_bump_profile(zero, "ann", -1);
    CHECK(cJSON_GetObjectItem(zero, "player_followed")->valuedouble == 0, "count does not go below 0");
    cJSON_Delete(zero);

    // a missing feed or profile is a no-op
    CHECK(follower_count_bump_posts(NULL, "ann", 1) == 0, "NULL feed is a no-op");
    CHECK(follower_count_bump_profile(NULL, "ann", 1) == 0, "NULL profile is a no-op");

    cJSON_Delete(feed);
    printf("follower_counts_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
