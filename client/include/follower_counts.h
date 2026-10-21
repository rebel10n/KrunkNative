#pragma once
#include <cJSON.h>
#include <string.h>

// A follow changes the target's follower count on the server, so the copies the Social hub keeps
// move with it: each post by that player ("flw", the count under the name) and the player's profile
// ("player_followed"). Without it a post and the profile disagree until the profile is reopened
// ("follower counts on posts not matching the profile", Krunker v10.0.3). v10_social.c applies
// these to its feeds and views on a successful follow.

// the count `delta` away from `value`, never below 0
static inline double follower_count_moved(const double value, const int delta) {
    const double v = value + delta;
    return v < 0 ? 0 : v;
}

// one post by `name`: its "flw" moves by `delta`. Returns 1 when the post is theirs.
static inline int follower_count_bump_post(cJSON *p, const char *name, const int delta) {
    const cJSON *pln = p ? cJSON_GetObjectItem(p, "pln") : NULL;
    if (!cJSON_IsString(pln) || !pln->valuestring || strcmp(pln->valuestring, name)) return 0;
    const cJSON *f = cJSON_GetObjectItem(p, "flw");
    const double now = follower_count_moved(cJSON_IsNumber(f) ? f->valuedouble : 0, delta);
    cJSON_DeleteItemFromObject(p, "flw");
    cJSON_AddNumberToObject(p, "flw", now);
    return 1;
}

// every post in `posts` (a feed array) by `name`. Returns the posts changed.
static inline int follower_count_bump_posts(cJSON *posts, const char *name, const int delta) {
    int changed = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, posts) changed += follower_count_bump_post(p, name, delta);
    return changed;
}

// the profile `data` of `name` (a view's player data): its "player_followed" moves by `delta`.
// Returns 1 when the profile is that player's.
static inline int follower_count_bump_profile(cJSON *data, const char *name, const int delta) {
    const cJSON *pn = data ? cJSON_GetObjectItem(data, "player_name") : NULL;
    if (!cJSON_IsString(pn) || !pn->valuestring || strcmp(pn->valuestring, name)) return 0;
    const cJSON *f = cJSON_GetObjectItem(data, "player_followed");
    const double now = follower_count_moved(cJSON_IsNumber(f) ? f->valuedouble : 0, delta);
    cJSON_DeleteItemFromObject(data, "player_followed");
    cJSON_AddNumberToObject(data, "player_followed", now);
    return 1;
}
