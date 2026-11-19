/*
 * services/brain/oauth.h - "Login with Box Office": kn_brain as the OAuth 2.0 provider of Box Office accounts
 * (Krunker v10.0.7's Login with Krunker and Settings > Connected apps; services/brain/oauth.c,
 * doc/v10/oauth-provider.md, migration 0366_account_links).
 */
#ifndef KN_BRAIN_OAUTH_H
#define KN_BRAIN_OAUTH_H

struct Brain;

int oauth_init(struct Brain *b);
void oauth_shutdown(struct Brain *b);

#endif
