/*
 * services/brain/streams.h - the Live Streams list of the v10 menu (GET /streams/top).
 *
 * v10's overlay reads krunker.io's /streams/top ({streams, promo, dropCampaigns}, v10:145083 and
 * the card fields at v10:146200-146260). KrunkNative answers the same shape from Twitch, where
 * the Krunker streamers are (v10 links twitch.tv/<login> and the Krunker category): the brain
 * asks Twitch Helix for the live streams of the game, with an app access token it gets from the
 * client credentials. The credentials live in config/config.json (streams.twitch.*) or the
 * KN_STREAMS_TWITCH_CLIENT_ID / KN_STREAMS_TWITCH_CLIENT_SECRET environment; they are never
 * logged or sent to a client.
 */
#ifndef KN_BRAIN_STREAMS_H
#define KN_BRAIN_STREAMS_H

#include <stddef.h>

#include "knsvc/json.h"

struct Brain;

int streams_init(struct Brain *b);
void streams_shutdown(struct Brain *b);

/* Pure parts (streams_norm.c; tests/test_streams.c feeds them canned Helix answers). */

/* A Helix GET /streams "data" array -> v10's /streams/top object: {streams:[...], promo:false,
 * dropCampaigns:[]}. Only type "live" rows are kept, logins that are not Twitch logins are
 * dropped, the thumbnail's {width}x{height} template is filled in, the list is sorted by viewers
 * (descending, stable). NULL data gives an empty list. The caller frees the result. */
cJSON *streams_normalise(const cJSON *data);

/* The app access token of POST https://id.twitch.tv/oauth2/token: 1 = read (token, lifetime in
 * seconds), 0 = not an answer with an access_token. */
int streams_parse_token(const char *body, char *token, size_t cap, long *expires_in);

/* The id of the first game of GET /helix/games?name=: 1 = read, 0 = no such game. */
int streams_parse_game_id(const char *body, char *id, size_t cap);

#endif
