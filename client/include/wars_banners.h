#pragma once
// v10.0.3 Turf Wars map banners (client/src/wars_banners.c): territorialWars.setClanInfo (deob-10.0.3.js:215039-215088)
// puts a banner model on every map SERVER_NODE whose tag starts with "wars_banner", painted in the clan's banner
// colour and carrying its logo (or its name) as a decal, for the clan holding the map ("warsClan", init's
// syncData.warsClan). The kinds are the tag's suffix (iïîìiïi, :214934-214973).
#include <client.h>
#include <net_ext.h>

// "warsClan" (NET_EXT_WARS_CLAN): setClanInfo(clan); NULL or has 0 = setClanInfo(null) (every init, :256999)
void wars_banners_set(const NetWarsClanPacket *clan);
void wars_banners_load(Scene*, const Map*); // client_load_map
void wars_banners_unload(void);             // client_unload_map: the banners go (removeBanners)
void wars_banners_frame(Client*);           // per frame before the world pass: (re)build, a logo that arrived
int wars_banners_count(void);               // tests: banners in the scene

// The tag's banner kind: 0 "wars_banner", 1 "_jap", 2 "_def"; -1 not a banner tag, -2 a "wars_banner*" tag of no
// kind (v10's setClanInfo throws there: no banner after it is put up).
int wars_banner_kind(const char *tag);
