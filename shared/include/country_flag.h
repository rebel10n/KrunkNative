#pragma once
// The Country Flag list (stats.flg, Krunker v3.1.1 Game.flags): the index a player picks in the
// account window ("flg" over the social socket) and the kill card's flag (NET_EXT_KN_STRIKE_KILL_FLAG).
// The names and ISO codes live in client/src/ui/account_flags.h; that header asserts it has this many.
// kn_brain refuses an index past it (services/brain/db_progress.c); -1 clears the flag.
#define COUNTRY_FLAG_COUNT 240
