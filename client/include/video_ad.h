#pragma once
// forceVideoAd in a game (client/src/net/video_ad.c): the placeholder ad (kn_ad.h), its CUSTOM_VIDEO_AD states to the game
// server (an official game's "vad", a Box Office / offline game's NET_EXT_KN_VIDEO_AD). 0 = not played (one is on, or the
// 90 s gap since the last one has not passed).
int client_video_ad_force(void);
