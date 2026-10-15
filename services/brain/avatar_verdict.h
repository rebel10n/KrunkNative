/*
 * services/brain/avatar_verdict.h - the moderation prompt and the strict verdict parser for
 * profile pictures (avatar.c). Pure: no database, no network (tests/test_avatar.c).
 *
 * Port of abulxt's avatar-moderation.ts (the same categories, the same fail-closed rules): the
 * model must answer with exactly one JSON object carrying this request's nonce. Only an exact,
 * self-consistent "allowed": true verdict approves a picture. A clear "allowed": false rejects it
 * (the categories it names are kept when they are known). Everything else is an error, which
 * hides the picture just like a rejection.
 */
#ifndef KN_BRAIN_AVATAR_VERDICT_H
#define KN_BRAIN_AVATAR_VERDICT_H

#include <stddef.h>

enum { AVATAR_VERDICT_APPROVED = 0, AVATAR_VERDICT_REJECTED = 1, AVATAR_VERDICT_ERROR = 2 };

typedef struct {
    char categories[256];   /* comma-separated, known categories only ("" when approved) */
    char reason[256];       /* one neutral sentence for the owner */
} AvatarVerdict;

/* 18 lowercase hex characters from the system RNG (out >= 19 bytes). 0 ok. */
int avatar_verdict_nonce(char *out, size_t n);

/* The system prompt for one review: the nonce is part of it. Returns 0 ok, -1 when out is too small. */
int avatar_verdict_prompt(char *out, size_t n, const char *nonce);

/* The question sent next to the image. */
const char *avatar_verdict_question(void);

/* Parses the model's answer (raw text, may be fenced). Fills v; returns AVATAR_VERDICT_*. */
int avatar_verdict_parse(const char *raw, const char *nonce, AvatarVerdict *v);

#endif
