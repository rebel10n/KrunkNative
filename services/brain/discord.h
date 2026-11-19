/*
 * services/brain/discord.h - Krunker v10.0.7's Discord link for Box Office accounts (services/brain/discord.c,
 * migration 0366_account_links): status, link + callback, Join Server, unlink with the deletion-pending state.
 */
#ifndef KN_BRAIN_DISCORD_H
#define KN_BRAIN_DISCORD_H

struct Brain;

int discord_init(struct Brain *b);
void discord_shutdown(struct Brain *b);

#endif
