# Ads (v10.0.5) and KrunkNative's placeholder ad

Krunker plays FRVR ads through `window.showAd` (deob-10.0.5.js:228002-228031; 10.0.4 :226713-226742), the rewarded
wrapper `window.showRewardedAd(cb)` (:227993-228000: `cb` runs on exit, watched or not) and the in-game
`window.forceVideoAd` (:227964-227981). KrunkNative loads no ad network. Where v10 plays an ad, KrunkNative shows a
placeholder card, "[insert FRVR ad here]" (`client/src/ui/kn_ad.c`, flow `client/src/ui/kn_ad_flow.c`), that counts
down (`KN_AD_DEFAULT_SECONDS` = 5, `KRUNKNATIVE_AD_SECONDS` overrides it), then Continue grants exactly what the
watched ad grants (user, 2026-10-30: "grant entitlements as if the user saw an ad for whatever is gated"). Closing it
early is v10's "skipped". The bundle's rules kept: one ad at a time (a second `showAd` is dropped), onShow, then
onComplete or onError, then onExit last.

| Feature | v10.0.5 | After the ad | krunker.io account | Box Office (kn_brain / server_bin) |
| --- | --- | --- | --- | --- |
| Free KR wheel (the store's Free KR card) | card `window.claimReward()` :129955 (v10 no longer defines it: v7.1.4 index.js:3168-3192); spin `AD_FINISHED` :222644, :222655; `config.rewardTime` 1 h :19957 | "sfkr" before the ad, "lfkr" on show, "fkrf" on error; complete opens the wheel, whose spin sends "fkr" -> "spinKR" | game socket (`krunker_net_send_account_frame`), "spinKR" back through `account_ui.c` kio frames | social socket; the brain takes sfkr / lfkr / fkrf as no-ops and pays "fkr" once per `accounts.reward_time_s` (now 3600) |
| End screen Double XP | `showDoubleXPRewardedAd` :227983-227991 -> `showRewardedAd` -> rqdxp when games % 3 == 0 (:227961-227963) | "rqdxp" -> "rrqdxp" (RECIEVE_DOUBLE_XP :227958) | game socket; "rrqdxp" to `double_xp_offer_answer` | social socket "rqdxp" (unchanged) |
| Battle pass free card (no VIP) | `freeclaimItem` :217588-217593 -> `showRewardedAd` -> "bpclm" [id] | "bpclm" -> "bpclmr" (claimItemRes) | game socket (claims were refused before; now sent) | social socket (unchanged) |
| Map / script video ad | trigger action 67 "Play Video Ad" -> S2C FORCE_VIDEO_AD "vad" (S2C table :227440) and client script `GAME.ADS.playVideo()` :187152-187155 -> `forceVideoAd`: at most every 90 s, CUSTOM_VIDEO_AD "vad" 1 asked, 2 shown, 3 completed (200 ms later), -1 failed | the server script's `onAdFinished(playerID, success)` | official: "vad" in, "vad" [state] out on the game socket | `NET_EXT_KN_VIDEO_AD` both ways (net_ext.h); server_bin fires `onAdFinished` (`ks_game_on_ad_finished`) |
| Krunker Strike's "Watch an ad for Double XP" banner | FRVR Strike | the claim | n/a | "rqdxp" after the placeholder |

Not gated, nothing to grant (no placeholder):

- `showRegisterVideoAd` (:215389-215395): the register-gift popup's ad sets a flag nothing reads, and the popup only shows
  with `window.canShowRegisterPopupAd`, which krunker.io's page sets false (index.html).
- Survey bundles (`selectBundle(.., survey = true)` -> SURVEY ad -> "svyb", :214196-214209): every caller passes false
  (:214137, :214177), so no survey bundle can be opened.
- The guests' interstitial at a match end (:224976): an ad with no reward (then `accountCreatePopup`).
- Store products priced `"ads"` (:25894): v10 only labels them "Watch Ad" and buys them like a free product, no ad.

Not verified live: the official server's answers to the ad frames ("spinKR", "rrqdxp", "bpclmr", "vad") were not
exercised against krunker.io; the frames and shapes are the bundle's.
