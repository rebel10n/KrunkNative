/*
 * services/brain/push.h - Web Push subscriptions (push.c): the subscribe routes' module hooks.
 * The rules are push_rules.h; the SQL is db_push.c.
 */
#ifndef KN_BRAIN_PUSH_H
#define KN_BRAIN_PUSH_H

#include "brain.h"
#include "knsvc/http.h"

/* BrainModule hook (main.c k_brain_modules): registers /api/push/vapid-key, /subscribe, /unsubscribe. */
int push_init(Brain *b);

/* v10 POST /webpush/status and /webpush/subscription-change (v10.js 26408, 26414; v10_account.c routes them):
 * the caller's own subscription rows, answered with the v10 envelope */
void push_v10_status(kn_http_req *req);
void push_v10_subscription_change(kn_http_req *req);

#endif
