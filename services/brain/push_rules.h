/*
 * services/brain/push_rules.h - the Web Push subscription rules (pure, no database): what a
 * subscription's endpoint and keys must look like before the brain stores them (push.c). Shared by the
 * subscribe / unsubscribe routes and tests/test_push_validation.c.
 *
 * An endpoint is the push service's https:// URL of the browser subscription, at most 512 bytes, with no
 * space or control byte. The keys are the browser's PushSubscription keys, base64url without padding:
 * p256dh is the client's uncompressed P-256 point (65 bytes, 87 characters), auth the 16-byte secret
 * (22 characters).
 */
#ifndef KN_BRAIN_PUSH_RULES_H
#define KN_BRAIN_PUSH_RULES_H

#include <stddef.h>

#define PUSH_ENDPOINT_MAX 512     /* bytes of the endpoint URL */
#define PUSH_P256DH_BYTES 65      /* uncompressed P-256 point */
#define PUSH_AUTH_BYTES 16        /* the subscription's auth secret */
#define PUSH_P256DH_LEN 87        /* base64url(65 bytes), unpadded */
#define PUSH_AUTH_LEN 22          /* base64url(16 bytes), unpadded */
#define PUSH_MAX_PER_ACCOUNT 5    /* subscriptions kept per account (the oldest are dropped) */

/* 1 when endpoint is https://<host>... within PUSH_ENDPOINT_MAX bytes, printable ASCII only. */
int push_endpoint_ok(const char *endpoint);

/* 1 when s is the unpadded base64url text of `bytes` bytes: the exact length and the alphabet
 * A-Z a-z 0-9 - _ only. */
int push_b64url_ok(const char *s, size_t bytes);

#endif
