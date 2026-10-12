#pragma once
// WP-D (doc/v7/map-logic.md §4.1): the Krunker v7.1.4 mode table indices. Declared by WP-A for
// the packages that read map / config mode lists; WP-D re-indexed shared/include/modes.h
// (GameModeId) to this table, so these names are aliases of the MODE_ID_* ids.
//
// v3.1.1 ids 0-12 and 15-23 keep their index. 13 and 14 were both Ranked in v3.1.1; v7 puts
// Classic FFA and Deposit there. v7 has no ranked mode (ranked = a comp Hardpoint lobby); ids 37
// and 38 stay reserved for ranked (modes.h MODE_ID_RESERVED_RANKED_*, doc/v7/decisions.md).
#include <modes.h>

typedef GameModeId ModeIdV7;

#define MODE_V7_FFA MODE_ID_FFA
#define MODE_V7_TDM MODE_ID_TDM
#define MODE_V7_POINT MODE_ID_POINT
#define MODE_V7_CTF MODE_ID_CTF
#define MODE_V7_BHOP MODE_ID_BHOP
#define MODE_V7_HIDE MODE_ID_HIDE
#define MODE_V7_INFECT MODE_ID_INFECT
#define MODE_V7_RACE MODE_ID_RACE
#define MODE_V7_LMS MODE_ID_LMS
#define MODE_V7_SIMON MODE_ID_SIMON
#define MODE_V7_GUN MODE_ID_GUN
#define MODE_V7_PROP MODE_ID_PROP
#define MODE_V7_BOSS MODE_ID_BOSS
#define MODE_V7_CLAS MODE_ID_CLAS
#define MODE_V7_DEPO MODE_ID_DEPO
#define MODE_V7_STALK MODE_ID_STALK
#define MODE_V7_KING MODE_ID_KING
#define MODE_V7_OINC MODE_ID_OINC
#define MODE_V7_TRADE MODE_ID_TRADE
#define MODE_V7_KC MODE_ID_KC
#define MODE_V7_DEF MODE_ID_DEF
#define MODE_V7_SHRP MODE_ID_SHRP
#define MODE_V7_TRAI MODE_ID_TRAI
#define MODE_V7_RAID MODE_ID_RAID
#define MODE_V7_BLITZ MODE_ID_BLITZ
#define MODE_V7_DOM MODE_ID_DOM
#define MODE_V7_SDM MODE_ID_SDM
#define MODE_V7_KRANK MODE_ID_KRANK
#define MODE_V7_TDF MODE_ID_TDF
#define MODE_V7_DEPOFFA MODE_ID_DEPOFFA
#define MODE_V7_CAR MODE_ID_CAR
#define MODE_V7_AON MODE_ID_AON
#define MODE_V7_MD MODE_ID_MD
#define MODE_V7_CHS MODE_ID_CHS
#define MODE_V7_BHFFA MODE_ID_BHFFA
#define MODE_V7_ZOM MODE_ID_ZOM
#define MODE_V7_IMP MODE_ID_IMP
#define MODE_V7_TABLE_COUNT MODE_V7_TABLE_SIZE  // 37: gameModes.js
#define MODE_V7_COUNT MODE_COUNT

// gameModes.js rotationModes (v3.1.1 [0, 1, 2, 3, 19]); g_rotation_modes holds the same list
#define MODE_V7_ROTATION {MODE_V7_FFA, MODE_V7_POINT, MODE_V7_CTF, MODE_V7_KC, MODE_V7_DOM}
