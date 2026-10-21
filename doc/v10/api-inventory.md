# v10 API inventory (the official client's routes and KrunkNative's answers)

Generated from the v10.0.x client bundle (`~/.cache/kn-v10/v10.js`, read only; line numbers are its lines). The table lists each HTTP route the client calls on the hosts it uses, its caller, and what the brain answers for it before and after this change. The route list was extracted from the REST client classes (`_9997` api.krunker.io base `_7283`, lines 25894-26152; `_10077` gapi.svc.krunker.io base `_7284`, lines 26153-27524) and from the bases used directly (`_7282` matchmaker, `_7285` social, `_7286` editor, `_7289` community, `_7284 + /data/...` XHRs, the legacy `apiRequest` on `_7283` at 131830-131990).

## States

- **served**: the brain answers the v10 path itself (file:line).
- **stubbed**: the brain answers the v10 path with a placeholder shape (none at the root before this change; KN's own stubs are listed in the notes).
- **tunnel**: no HTTP route; the same request is answered on the social socket (`["gapi", rid, method, path, body]`, doc/v10/social.md). The HTTP path stays a 404.
- **missing**: the brain's not_found fallback (404, public_routes.c:677).

After this change: **implemented** (real KrunkNative data), **alias** (the same KrunkNative handler, served at the v10 path), **empty** (the empty shape the v10 consumer reads; no KrunkNative source), **refusal** (501 NOT_IMPLEMENTED, a write or an account read KrunkNative does not map), **not found** (404 NOT_FOUND for a specific resource), **tunnel** (social socket only), **n/a** (a WebSocket, a link, or not a request).

## Summary

Routes: 218 (api.krunker.io: 21, gapi.svc.krunker.io: 189, matchmaker.krunker.io: 6, social.krunker.io: 1, editor.krunker.io: 1).

Before this change: served 10, stubbed 0, tunnel only 46, missing 162.

After this change: empty 20, refusal 92, alias 44, served 10, missing 0, implemented 6, not found 1, tunnel 45.

| # | host | method | path | v10 caller (v10.js line) | before | after | source (KN table / file) |
|---|---|---|---|---|---|---|---|
| 1 | api | GET | `/match-history/me/season/{s}/gameMode/{m}/isRanked/{all|bool}[/region/{r}]` | matchHistory.getMatchHistory (25940) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 2 | api | GET | `/player-profile/me/season/{s}[/region/{r}]` | playerProfile.getPlayerProfile (25959) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 3 | api | GET | `/leaderboards/rankings/season/{s}[/region/{r}]` | playerProfile.getLeaderboard (25972) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 4 | api | POST | `/player-profile/me/claim-milestone/region/{r}` | playerProfile.claimMilestone (25976) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 5 | api | GET | `/ranked/me/connection` | ranked.getMyConnection (25985) | missing: not_found fallback (public_routes.c:677) | alias | ranked.c |
| 6 | api | GET | `/streams/top` | streams.getTopStreams (25992) | served: streams.c:326 (route2 "/streams/top") | served; EMPTY `{success, data: {streams: []}}` with no Twitch source (was a 503) | streams.c (reply_empty) |
| 7 | api | GET | `/daily-spin/progress` | dailySpin.getProgress (25999) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 8 | api | POST | `/shop/cancel-subscription` | shop.cancelSubscription (26006) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 9 | api | POST | `/iap/checkout` | iap.checkout (26018) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 10 | api | POST | `/iap/claim` | iap.claim (26024) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 11 | api | POST | `/my-api/create-payment/{id}` | paypal (_69700, v10.js 209123) (209123) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 12 | api | POST | `/my-api/execute-payment/{id}` | paypal (_69700, v10.js 209131) (209131) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 13 | api | GET | `/ipinfo` | IP checker (_7290, v10.js 16440) (16440) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 14 | api | GET | `/webhooks/general/maps/list` | legacy apiRequest listMaps (v10.js 131960s) (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 15 | api | GET | `/webhooks/general/maps/search` | legacy apiRequest searchMap (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 16 | api | GET | `/webhooks/general/maps/config` | legacy apiRequest getMapPreset (131960) | missing: not_found fallback (public_routes.c:677) | empty (`{}`: no preset, the menu shows DEFAULT) | v10_empty.c |
| 17 | api | GET | `/webhooks/general/mods/list` | legacy apiRequest listMods (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 18 | api | GET | `/webhooks/general/mods/search` | legacy apiRequest searchMod (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 19 | api | GET | `/webhooks/general/assets/list` | legacy apiRequest listAssets (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 20 | api | GET | `/webhooks/general/assets/search` | legacy apiRequest (searchAsset) (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 21 | api | GET | `/webhooks/general/items/prices` | legacy apiRequest getPrices (131960) | missing: not_found fallback (public_routes.c:677) | alias | creations.c |
| 22 | matchmaker | GET | `/ping` | matchmaker pingFetch (v10.js 147377) (147377) | served: public_routes.c:689 | served (unchanged) |  |
| 23 | matchmaker | GET | `/ping-list` | matchmaker fetchPingList (147377) | served: public_routes.c:691 | served (unchanged) |  |
| 24 | matchmaker | GET | `/game-list` | matchmaker game list (v10.js 107131, 215633) (107131) | served: public_routes.c:693 | served (unchanged) |  |
| 25 | matchmaker | GET | `/game-info` | matchmaker game info (147377) | served: public_routes.c:694 | served (unchanged) |  |
| 26 | matchmaker | GET | `/seek-game` | matchmaker matchmakerSeek (147377) | served: public_routes.c:695 | served (unchanged) |  |
| 27 | matchmaker | GET | `/generate-token` | matchmaker fetchValidationToken (147377) | served: public_routes.c:696 | served (unchanged) |  |
| 28 | social | GET | `/player-count` | social player count (_7285, v10.js 211170) (211170) | missing: KN has it at /mm/... or /social/player-count (public_routes.c:702-704) | alias | public_routes.c |
| 29 | editor | GET | `/player-count` | editor player count (_7286, v10.js 211178) (211178) | missing: KN has it at /mm/... or /social/player-count (public_routes.c:702-704) | alias | public_routes.c |
| 30 | gapi | GET | `/data/skins` | skins XHR (v10.js 28035) (28035) | missing: not_found fallback (public_routes.c:677) | implemented | v10_api.c |
| 31 | gapi | GET | `/data/spins` | spins XHR (v10.js 102764) (102764) | missing: not_found fallback (public_routes.c:677) | implemented | v10_api.c |
| 32 | gapi | POST | `/auth/nonce` | auth.getAuthNonce (26172) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 33 | gapi | POST | `/auth/login/username` | auth.loginUsername (26177) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 34 | gapi | POST | `/auth/login/email` | auth.loginEmail (26190) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 35 | gapi | POST | `/auth/login/email-code` | auth.loginEmailCode (26201) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 36 | gapi | POST | `/auth/code-challenge/{id}` | auth.respondCodeChallenge (26209) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 37 | gapi | POST | `/auth/resend-code-challenge/{id}` | auth.resendCodeChallenge (26217) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 38 | gapi | POST | `/auth/migrate/{id}` | auth.migrateUser (26222) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 39 | gapi | POST | `/auth/2fa/challenge/{id}` | auth.respondTwoFactorChallenge (26230) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 40 | gapi | POST | `/auth/verification/check/{id}` | auth.checkVerification (26238) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 41 | gapi | POST | `/auth/verification/resend/{id}` | auth.resendVerification (26243) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 42 | gapi | POST | `/auth/verification/change-email/{id}` | auth.changeEmail (26248) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 43 | gapi | POST | `/auth/register/email-code` | auth.registerEmailCode (26256) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 44 | gapi | POST | `/auth/username-challenge/{id}` | auth.selectUsername (26264) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 45 | gapi | POST | `/auth/password-challenge/{id}` | auth.updatePassword (26272) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 46 | gapi | POST | `/auth/recover-account` | auth.recoverAccount (26280) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 47 | gapi | POST | `/auth/kid/age-challenge/{id}` | auth.respondKidAgeChallenge (26288) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 48 | gapi | POST | `/auth/kid/parental-email/{id}` | auth.submitKidParentalEmail (26296) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 49 | gapi | GET | `/auth/kid/consent-status/{id}` | auth.checkKidConsentStatus (26304) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 50 | gapi | GET | `/player/me` | player.getMe (26312) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 51 | gapi | GET | `/player/wallet` | player.getWallet (26317) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 52 | gapi | POST | `/account/2fa/enable/start` | account.enable2FAStart (26326) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 53 | gapi | POST | `/account/2fa/enable/verify` | account.enable2FAVerify (26331) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 54 | gapi | POST | `/account/2fa/disable` | account.disable2FA (26339) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 55 | gapi | GET | `/account/usernames` | account.listUsernames (26347) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 56 | gapi | POST | `/account/usernames/reserve` | account.reserveUsername (26352) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 57 | gapi | POST | `/account/usernames/swap` | account.swapUsername (26360) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 58 | gapi | GET | `/guest/profile` | guest.getProfile (26372) | served: guest.c:98 | served (unchanged) |  |
| 59 | gapi | GET | `/ranked/shop` | ranked.getShop (26381) | missing: not_found fallback (public_routes.c:677) | alias | ranked.c |
| 60 | gapi | POST | `/ranked/purchase` | ranked.purchaseSkin (26386) | missing: not_found fallback (public_routes.c:677) | alias | ranked.c |
| 61 | gapi | POST | `/webpush/subscribe` | webpush.subscribe (26396) | missing: not_found fallback (public_routes.c:677) | alias | push.c |
| 62 | gapi | POST | `/webpush/unsubscribe` | webpush.unsubscribe (26402) | missing: not_found fallback (public_routes.c:677) | alias | push.c |
| 63 | gapi | POST | `/webpush/status` | webpush.getStatus (26408) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 64 | gapi | POST | `/webpush/subscription-change` | webpush.subscriptionChange (26414) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 65 | gapi | GET | `/email/preferences` | email.getPreference (26424) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 66 | gapi | POST | `/email/preferences` | email.setPreference (26429) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 67 | gapi | GET | `/discord/status` | discord.getStatus (26439) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 68 | gapi | POST | `/discord/link` | discord.link (26444) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 69 | gapi | POST | `/discord/join` | discord.joinGuild (26452) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 70 | gapi | POST | `/discord/unlink` | discord.unlink (26457) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 71 | gapi | GET | `/skins` | skins.list (26474) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 72 | gapi | GET | `/skins/{id}` | skins.get (26479) | missing: not_found fallback (public_routes.c:677) | not found | v10_empty.c |
| 73 | gapi | POST | `/skins` | skins.create (26484) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 74 | gapi | PUT | `/skins/{id}` | skins.update (26490) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 75 | gapi | DELETE | `/skins/{id}` | skins.delete (26496) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 76 | gapi | POST | `/skins/{id}/upload` | skins.uploadAsset (26505) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 77 | gapi | POST | `/skins/{id}/duplicate` | skins.duplicate (26511) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 78 | gapi | GET | `/shop/displays/{tag}` | shop.displays (v10.js 26520) (26520) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 79 | gapi | GET | `/shop/displays?tag={tag}` | shop.displays (v10.js 26525) (26525) | missing: not_found fallback (public_routes.c:677) | alias | shop.c |
| 80 | gapi | POST | `/shop/purchase` | shop.purchase (26530) | missing: not_found fallback (public_routes.c:677) | alias | shop.c |
| 81 | gapi | GET | `/shop/wild-offer` | shop.getWildOffer (26539) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 82 | gapi | POST | `/shop/wild-offer/purchase` | shop.purchaseWildOffer (26544) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 83 | gapi | POST | `/shop/wild-offer/refresh` | shop.refreshWildOffer (26553) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 84 | gapi | GET | `/lottery/{id}` | lottery.getLottery (26566) | missing: not_found fallback (public_routes.c:677) | alias | lottery.c |
| 85 | gapi | POST | `/lottery/{id}/participate` | lottery.participateInLottery (26571) | missing: not_found fallback (public_routes.c:677) | alias | lottery.c |
| 86 | gapi | GET | `/maps` | maps.getMaps (26581) | missing: not_found fallback (public_routes.c:677) | implemented | v10_api.c |
| 87 | gapi | GET | `/maps/{id}` | maps.getMap (26586) | missing: not_found fallback (public_routes.c:677) | implemented | v10_api.c |
| 88 | gapi | GET | `/maps/by-name/{name}` | maps.getMapByName (26592) | missing: not_found fallback (public_routes.c:677) | implemented | v10_api.c |
| 89 | gapi | GET | `/maps/by-name/{name}/leaderboard` | maps.getMapLeaderboard (26602) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 90 | gapi | POST | `/maps/{id}/vote` | maps.voteMap (26605) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 91 | gapi | POST | `/maps/{id}/feature` | maps.featureMap (26613) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 92 | gapi | DELETE | `/maps/{id}` | maps.deleteMap (26621) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 93 | gapi | GET | `/maps/{id}/versions` | maps.getMapVersions (26626) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 94 | gapi | POST | `/maps/{id}/versions/{v}/revert` | maps.revertMapVersion (26632) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 95 | gapi | POST | `/maps/{id}/funds` | maps.updateMapFunds (26637) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 96 | gapi | POST | `/maps/{id}/leaderboard` | maps.updateMapLeaderboard (26645) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 97 | gapi | GET | `/maps/{id}/data?player={p}` | maps.getMapData (26655) | missing: not_found fallback (public_routes.c:677) | empty | v10_empty.c |
| 98 | gapi | DELETE | `/maps/{id}/data?{q}` | maps.deleteMapData (26664) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 99 | gapi | GET | `/territorial-wars/status` | territorialWars.getStatus (26673) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 100 | gapi | GET | `/territorial-wars/clan/{id}` | territorialWars.getClanStats (26678) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 101 | gapi | GET | `/territorial-wars/clan/{id}/players` | territorialWars.getClanPlayers (26688) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 102 | gapi | GET | `/territorial-wars/clan/{id}/earnings` | territorialWars.getClanEarnings (26693) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 103 | gapi | GET | `/territorial-wars/leaderboards/top-performing-clans` | territorialWars.getTopPerformingClans (26704) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 104 | gapi | GET | `/territorial-wars/leaderboards/top-grossing-clans` | territorialWars.getTopGrossingClans (26713) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 105 | gapi | GET | `/territorial-wars/clan/{id}/payouts` | territorialWars.getClanPayouts (26722) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 106 | gapi | GET | `/territorial-wars/leaderboards/top-performing-players` | territorialWars.getTopPerformingPlayers (26732) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 107 | gapi | GET | `/territorial-wars/player/{id}` | territorialWars.getPlayerProfile (26737) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 108 | gapi | GET | `/territorial-wars/player/{id}/contributions` | territorialWars.getPlayerContributions (26743) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 109 | gapi | GET | `/territorial-wars/player/{id}/earnings` | territorialWars.getPlayerEarnings (26746) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 110 | gapi | POST | `/clans/{id}/logo` | clans.uploadLogo (26753) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 111 | gapi | DELETE | `/clans/{id}/logo` | clans.removeLogo (26759) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 112 | gapi | GET | `/clans/{id}/loot-config` | clans.getLootConfig (26764) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 113 | gapi | PUT | `/clans/{id}/loot-config` | clans.updateLootConfig (26767) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 114 | gapi | GET | `/clans/{id}/motd` | clans.getMotd (26773) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 115 | gapi | PUT | `/clans/{id}/motd` | clans.setMotd (26776) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 116 | gapi | DELETE | `/clans/{id}/motd` | clans.deleteMotd (26784) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 117 | gapi | POST | `/clans/{id}/banner-color` | clans.setBannerColor (26789) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 118 | gapi | GET | `/clans/recruitment` | clans.listRecruitment (26803) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 119 | gapi | GET | `/clans/{id}/recruitment` | clans.getRecruitment (26806) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 120 | gapi | POST | `/clans/{id}/recruitment` | clans.createOrUpdateRecruitment (26809) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 121 | gapi | DELETE | `/clans/{id}/recruitment` | clans.removeRecruitment (26815) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 122 | gapi | GET | `/clans/my-applications` | clans.getMyApplications (26820) | missing: not_found fallback (public_routes.c:677) | alias | turf.c |
| 123 | gapi | POST | `/clans/{id}/apply` | clans.applyToClan (26823) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | alias | turf.c |
| 124 | gapi | GET | `/clans/{id}/profile` | clans.getClanProfile (26828) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 125 | gapi | GET | `/clans/by-name/{name}` | clans.getClanProfileByName (26833) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 126 | gapi | GET | `/market/listings[?{q}]` | market.getListings (26857) | missing: not_found fallback (public_routes.c:677) | empty | v10_market.c |
| 127 | gapi | GET | `/market/inventory` | market.getInventory (26862) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 128 | gapi | GET | `/market/sales` | market.getSales (26867) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 129 | gapi | GET | `/market/stats[?{q}]` | market.getStats (26882) | missing: not_found fallback (public_routes.c:677) | empty | v10_market.c |
| 130 | gapi | GET | `/market/items/{id}/sales` | market.getItemSalesInfo (26887) | missing: not_found fallback (public_routes.c:677) | empty | v10_market.c |
| 131 | gapi | GET | `/market/items/{id}/sales-statistics[?{q}]` | market.getItemSalesStatistics (26895) | missing: not_found fallback (public_routes.c:677) | empty | v10_market.c |
| 132 | gapi | POST | `/market/list` | market.listItem (26900) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 133 | gapi | POST | `/market/purchase` | market.purchaseItem (26906) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 134 | gapi | POST | `/market/unlist` | market.unlistItem (26912) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 135 | gapi | POST | `/market/quick-sell` | market.quickSellItem (26918) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 136 | gapi | GET | `/market/black-market` | market.getBlackMarketItems (26924) | missing: not_found fallback (public_routes.c:677) | empty | v10_market.c |
| 137 | gapi | GET | `/market/black-market/rank` | market.getBlackMarketRank (26929) | missing: not_found fallback (public_routes.c:677) | empty | v10_market.c |
| 138 | gapi | POST | `/market/black-market/purchase` | market.purchaseBlackMarketItem (26934) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 139 | gapi | GET | `/market/history[?{q}]` | market.getHistory (26948) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 140 | gapi | GET | `/items/{id}/view` | items.getView (26957) | missing: not_found fallback (public_routes.c:677) | empty (404 for an id not in skins.json) | v10_market.c |
| 141 | gapi | GET | `/items/{id}/sales?range={r}` | items.getSales (26963) | missing: not_found fallback (public_routes.c:677) | empty (404 for an id not in skins.json) | v10_market.c |
| 142 | gapi | GET | `/items/{id}/instances?sort={s}` | items.getInstances (26969) | missing: not_found fallback (public_routes.c:677) | empty (404 for an id not in skins.json) | v10_market.c |
| 143 | gapi | POST | `/items/{id}/paint` | items.setPaint (26974) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 144 | gapi | POST | `/items/{id}/style` | items.setStyle (26980) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 145 | gapi | GET | `/leaderboards/{type}[?sort&season]` | leaderboards.getLeaderboard (26995) | missing: not_found fallback (public_routes.c:677) | alias | leaderboards.c |
| 146 | gapi | GET | `/leaderboards/raids/{map}[?sort]` | leaderboards.getRaidLeaderboard (27005) | missing: not_found fallback (public_routes.c:677) | alias | leaderboards.c |
| 147 | gapi | GET | `/leaderboards/raids/{map}/monthly[?month&sort]` | leaderboards.getMonthlyRaidLeaderboard (27016) | missing: not_found fallback (public_routes.c:677) | alias | leaderboards.c |
| 148 | gapi | GET | `/leaderboards/krunkcup/teams` | leaderboards.getKrunkCupTeams (27021) | missing: not_found fallback (public_routes.c:677) | alias | leaderboards.c |
| 149 | gapi | GET | `/leaderboards/krunkcup/teams/{id}/contributors` | leaderboards.getKrunkCupContributors (27026) | missing: not_found fallback (public_routes.c:677) | alias | leaderboards.c |
| 150 | gapi | GET | `/leaderboards/krunkcup/players[?{q}]` | leaderboards.getKrunkCupTopPlayers (27032) | missing: not_found fallback (public_routes.c:677) | alias | leaderboards.c |
| 151 | gapi | GET | `/trades/history[?{q}]` | trades.getHistory (27047) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 152 | gapi | POST | `/trades/create` | trades.createTrade (27052) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 153 | gapi | POST | `/trades/{id}/accept` | trades.acceptTrade (27058) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 154 | gapi | POST | `/trades/{id}/cancel` | trades.cancelTrade (27063) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 155 | gapi | POST | `/trades/decline-all` | trades.declineAll (27068) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 156 | gapi | GET | `/trades/inventory/{name}` | trades.getInventory (27073) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 157 | gapi | GET | `/trades/player-search?name={name}` | trades.searchPlayer (27078) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 158 | gapi | GET | `/moderation/chat/{name}` | moderation.getChatBan (27087) | missing: not_found fallback (public_routes.c:677) | implemented | feature_bans (chat); v10_mods.c |
| 159 | gapi | POST | `/moderation/chat` | moderation.chatBan (27092) | missing: not_found fallback (public_routes.c:677) | implemented | feature_bans (chat), moderation_log; v10_mods.c |
| 160 | gapi | POST | `/moderation/flag` | moderation.flagUser (27098) | missing: not_found fallback (public_routes.c:677) | implemented | players.player_hack, moderation_log; v10_mods.c |
| 161 | gapi | GET | `/creators/events` | creators.getUpcomingEvents (27108) | served: community.c:402 | served (featured maps and mods from the DB; polls from the operator file, empty without it) |  |
| 162 | gapi | GET | `/players/{name}` | players.getProfile (27117) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 163 | gapi | GET | `/players/{name}/sales?range={r}` | players.getSales (27123) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 164 | gapi | GET | `/players/{name}/listings` | players.getListings (27128) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 165 | gapi | GET | `/players/search?{q}` | players.search (27137) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 166 | gapi | POST | `/players/{name}/follow` | players.followPlayer (27142) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 167 | gapi | GET | `/players/{name}/followers` | players.getFollowers (27150) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 168 | gapi | GET | `/players/{name}/following` | players.getFollowing (27155) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 169 | gapi | POST | `/players/{name}/verify` | players.verifyPlayer (27160) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 170 | gapi | POST | `/players/{name}/report` | players.reportPlayer (27168) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 171 | gapi | GET | `/players/{name}/reports` | players.getReports (27177) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 172 | gapi | POST | `/players/{name}/gift` | players.giftKR (27182) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_market.c |
| 173 | gapi | POST | `/mods/publish/key` | mods.publishKey (27195) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | none (no mod file storage); v10_mods.c |
| 174 | gapi | GET | `/mods/by-name/{name}` | mods.getModByName (27201) | missing: not_found fallback (public_routes.c:677) | implemented | published_mods, accounts; v10_mods.c |
| 175 | gapi | POST | `/mods/{id}/vote` | mods.voteMod (27204) | missing: not_found fallback (public_routes.c:677) | implemented | creation_votes, published_mods; v10_mods.c |
| 176 | gapi | POST | `/mods/{id}/feature` | mods.featureMod (27212) | missing: not_found fallback (public_routes.c:677) | implemented | published_mods.mod_featured (developers); v10_mods.c |
| 177 | gapi | DELETE | `/mods/{id}` | mods.deleteMod (27220) | missing: not_found fallback (public_routes.c:677) | implemented | published_mods.deleted_at (creator, developers, mod makers); v10_mods.c |
| 178 | gapi | POST | `/mods/publish` | mods.publishUpload (27229) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | none (no mod file storage); v10_mods.c |
| 179 | gapi | GET | `/social/feed[?{q}]` | social.getFeed (27244) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 180 | gapi | GET | `/social/feed?player_id={id}` | social.getFeedByPlayer (27253) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 181 | gapi | GET | `/social/post/{id}` | social.getPost (27258) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 182 | gapi | POST | `/social/post` | social.createPost (27263) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 183 | gapi | POST | `/social/post/{id}/vote` | social.votePost (27269) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 184 | gapi | POST | `/social/post/{id}/comment` | social.commentPost (27277) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 185 | gapi | GET | `/social/news` | social.getNews (27285) | served: community.c:403 | served (EMPTY `{news: []}` until the operator file config/community.json exists) |  |
| 186 | gapi | GET | `/spins/wheels` | spins.getWheels (27294) | missing: not_found fallback (public_routes.c:677) | alias | public_routes.c |
| 187 | gapi | POST | `/spins/execute` | spins.execute (27299) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 188 | gapi | GET | `/krunkcup/wristband` | krunkcup.getWristbandStatus (27309) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 189 | gapi | POST | `/krunkcup/wristband/claim` | krunkcup.claimWristband (27314) | missing: not_found fallback (public_routes.c:677) | refusal (403 NOT_IMPLEMENTED) | v10_empty.c |
| 190 | gapi | GET | `/kpd/demos[?{q}]` | kpd.listDemos (27329) | missing: not_found fallback (public_routes.c:677) | implemented | kpd_demos, kpd_demo_votes (0351); v10_mods.c |
| 191 | gapi | GET | `/kpd/demos/{id}[?{q}]` | kpd.demoInfo (27335) | missing: not_found fallback (public_routes.c:677) | implemented | kpd_demos, kpd_demo_votes (0351); v10_mods.c |
| 192 | gapi | POST | `/kpd/demos/{id}/vote` | kpd.vote (27345) | missing: not_found fallback (public_routes.c:677) | implemented | kpd_demos, kpd_demo_votes (0351); v10_mods.c |
| 193 | gapi | GET | `/kpd/reviewers` | kpd.reviewers (27354) | missing: not_found fallback (public_routes.c:677) | implemented | kpd_calls, accounts (P_KPD_REVIEWERS, db_kpd.c); v10_mods.c |
| 194 | gapi | POST | `/studios` | studios.createStudio (27363) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 195 | gapi | GET | `/studios/mine` | studios.getMyStudios (27371) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 196 | gapi | GET | `/studios/{id}` | studios.getStudio (27377) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 197 | gapi | DELETE | `/studios/{id}` | studios.deleteStudio (27383) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 198 | gapi | POST | `/studios/{id}/members` | studios.addStudioMember (27388) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 199 | gapi | DELETE | `/studios/{id}/invites/{inv}` | studios.cancelStudioInvite (27396) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 200 | gapi | GET | `/studios/invites` | studios.getMyStudioInvites (27401) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 201 | gapi | POST | `/studios/invites/{id}/accept` | studios.acceptStudioInvite (27407) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 202 | gapi | POST | `/studios/invites/{id}/decline` | studios.declineStudioInvite (27412) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 203 | gapi | POST | `/studios/{id}/transfer` | studios.transferStudio (27417) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 204 | gapi | GET | `/studios/{id}/monetization` | studios.getStudioMonetization (27425) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 205 | gapi | POST | `/studios/{id}/monetization` | studios.applyStudioMonetization (27432) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 206 | gapi | DELETE | `/studios/{id}/monetization` | studios.cancelStudioMonetization (27441) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 207 | gapi | DELETE | `/studios/{id}/members/{name}` | studios.removeStudioMember (27446) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 208 | gapi | POST | `/studios/{id}/maps` | studios.addStudioMap (27451) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 209 | gapi | DELETE | `/studios/{id}/maps/{map}` | studios.removeStudioMap (27459) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 210 | gapi | POST | `/studios/{id}/currencies` | studios.createStudioCurrency (27464) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 211 | gapi | GET | `/studios/{id}/analytics` | studios.getStudioAnalytics (27474) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 212 | gapi | GET | `/studios/{id}/skins` | studios.getStudioSkins (27480) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 213 | gapi | POST | `/studios/{id}/skins/import` | studios.importStudioSkin (27486) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 214 | gapi | POST | `/studios/{id}/skins/{sid}/unpublish` | studios.unpublishStudioSkin (27494) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 215 | gapi | POST | `/studios/{id}/skins/{sid}/publish` | studios.publishStudioSkin (27499) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 216 | gapi | POST | `/studios/{id}/currencies/{cid}/unpublish` | studios.unpublishStudioCurrency (27504) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 217 | gapi | POST | `/studios/{id}/currencies/{cid}/publish` | studios.publishStudioCurrency (27509) | tunnel: social socket gapi tunnel: social_gapi.c / clan_v10.c / studios.c (route table in doc/v10/social.md) | tunnel only (no HTTP) | social_gapi.c / clan_v10.c / studios.c |
| 218 | gapi | GET | `/titles` | titles.getTitles (27518) | missing: not_found fallback (public_routes.c:677) | implemented | v10_api.c |

## Stubs the local harness uses (tools/krunker-local)

- `gapi.svc.krunker.io` and `gapi.beta.krunker.io` forward to the brain root (`hostRoutes` in `tools/krunker-local/config.example.json`), so every gapi path above is a brain root path. Before this change all but the 10 served rows answered the brain's 404 (`public_routes.c:677`).
- The probe stubs named in the brief: `/maps` (404 not_found) and `/titles` and `/data/skins` (404) are now answered by `v10_api.c`; `/data/spins` at the root was a 404 and is now the packed form (the client reads it with its unpack).
- `manifest.json` and `img/pwa/icon32x32.png` are 404 in the harness copy: the files are not in KrunkNative's trees (`fillPaths`), not a route.
- `analytics.google.com` is blocked by design (not a brain route).

## Not HTTP (out of scope of this inventory)

- WebSockets: the game frontend's matchmaking queue (`/v1/matchmaking/queue`, v10.js 98438), the social socket (`["gapi", rid, ...]` tunnel, doc/v10/social.md), the event bus (`EVENT_BUS_URI`), and the game server socket.
- Links, not requests: `community.krunker.io/polls` and `/polls/<id>` (v10.js 116976, 196856) are URLs the page opens.

## Deviations from v10.0.x (intentional, documented in code)

- Refusals answer **403 NOT_IMPLEMENTED** with `{"error": {"code", "message"}}`. The v10 gapi wrapper (v10.js 863756 onwards) throws on any status >= 500, so a 501 would show "Server error" and drop the message. 403 is read as a normal refused response.
- A read whose data exists in KrunkNative but only on the social socket (market listings, KPD demos, published mods, the player profile) answers the EMPTY shape over HTTP. The native client gets the real data over the socket; the HTTP answer is empty until a caller-bound HTTP path is added.
- Black-market rank answers `200 null` (v10 runs it in `Promise.all` with the black-market list, v10.js 136255; a refusal would blank the popup).
- Map lists use the 46 official maps (`shared/include/krunker_maps.h`, mapIndex order); each row carries `index`, `name`, `fileName`, `creatorname` and `map_featured`. Map JSON is the file under `assets/maps/official/` unchanged.
- The packed `/data/skins` and `/data/spins` use v10.js's own pack layout (`services/brain/v10_pack.c`). Decoding them with the real v10.js `unpack` (run under node, lines 27744-28027) returns the source JSON deep-equal (8069 skins; the wheels object).

## Still answered EMPTY or REFUSAL, and why

- EMPTY (no KrunkNative source over HTTP): match history, the leaderboard rankings by season, the wild offer (`null`, the no-offer state), the skin list for an account, map versions, map player data, the map leaderboard, market listings, market stats, black-market list and rank, item views and sales and instances (404 for an id not in `assets/skins.json`), KPD demos and reviewers.
- REFUSAL 403 (a write, a payment, an account read KrunkNative does not map over HTTP, or the v10 auth contract): every `/auth/*` route (KrunkNative's accounts use `/auth/login` with a different contract, `auth.c`), `/player/me`, `/player/wallet`, `/account/*`, the email and Discord routes, the payments (`/iap/*`, `/shop/cancel-subscription`, `/my-api/*`), the IP checker, the daily spin, the krunkcup wristband, the player profile and seasonal stats, the web push status and subscription change, all skin and map writes, the wild offer writes, `/spins/execute` (the spin is the social socket's `spin` op), market, items, trades, moderation, mods, KPD votes, player sales, listings, verify, gift.
- `GET /webhooks/general/maps/config` (row 16): EMPTY `{}`. The legacy webhook's body is the preset `{da, fu}` at the top level; KrunkNative has no per-map preset data, and v10 reads an absent `da` as the menu's DEFAULT (presetLoaded, v10.js 212624).

## No-source cases (the brief's examples)

- **Live streams** (row 6): KrunkNative takes the list from Twitch Helix, so with no Twitch source (`streams.enabled` off, or no client id / secret) `/streams/top` answers the empty list `{success: true, data: {streams: []}}` (streams.c reply_empty), not a 503. With a source it answers the real list. The native menu renders the empty list as zero streams, as v10 does for an empty `streams` array (v10.js 145057-145078).
- **Social feed** (rows 179-184): KrunkNative DOES have a source: `feed_posts` (the tabs global, following, hot, giveaways, clan, and the player feed), in `social_gapi.c` (POST_COLS and `social_feed`, lines 242-310). It answers on the authenticated social socket (`["gapi", rid, ...]`, doc/v10/social.md "Transport (deviation)"), so the HTTP path stays 404 by design. Returning the empty feed over HTTP would hide real posts, so no empty stub is used for it.
- **News** (row 185): the operator's file (`brain.community.feed`, default config/community.json), empty `{news: []}` until that file exists. Not invented.
- **Creators events** (row 161): real DB rows (featured maps and mods) plus the operator's polls.
