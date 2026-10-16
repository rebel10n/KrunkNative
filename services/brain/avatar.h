/*
 * services/brain/avatar.h - profile pictures (avatar.c): upload, AI review, storage, serving.
 * Routes and rules: the comment at the top of avatar.c.
 */
#ifndef KN_BRAIN_AVATAR_H
#define KN_BRAIN_AVATAR_H

#include "brain.h"

int avatar_init(Brain *b);
void avatar_shutdown(Brain *b);

/* The review of one normalized picture (a base64 WebP, the caller keeps it): one moderation call on
 * the moderation provider profile, the strict verdict of avatar_verdict.c, fail closed. Profile
 * pictures and clan logos both go through it. status: "approved" | "rejected" | "error"; categories
 * comma-separated; model: the model that answered. done runs exactly once, on the loop thread (it
 * may run before avatar_review_picture returns when the AI proxy is off). */
typedef struct {
    const char *status;
    const char *reason;
    const char *categories;
    const char *model;
} PictureVerdict;
typedef void (*PictureVerdictFn)(void *ud, const PictureVerdict *v);
void avatar_review_picture(const char *webp_b64, PictureVerdictFn done, void *ud);

#endif
