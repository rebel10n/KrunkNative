#pragma once
// WP-D (doc/v7/map-logic.md §4.1): the Krunker v7.1.4 mode table indices (gameModes.js; the
// "init" modeIndex, map "modes" and "config.modes" values). Declared by WP-A for the packages
// that read map / config mode lists; WP-D re-indexes shared/include/modes.h (GameModeId) to this
// table and then folds this header into it.
//
// v3.1.1 ids 0-12 and 15-23 keep their index. 13 and 14 were both Ranked in v3.1.1; v7 puts
// Classic FFA and Deposit there, and KN keeps Ranked on new ids 37+ (doc/v7/decisions.md
// "Ranked"; not hostable, ranked matchmaking only).
typedef enum {
    MODE_V7_FFA = 0, MODE_V7_TDM, MODE_V7_POINT, MODE_V7_CTF, MODE_V7_BHOP, MODE_V7_HIDE, MODE_V7_INFECT,
    MODE_V7_RACE, MODE_V7_LMS, MODE_V7_SIMON, MODE_V7_GUN, MODE_V7_PROP, MODE_V7_BOSS,
    MODE_V7_CLAS = 13,      // Classic FFA (v3.1.1 13: Ranked objective variant)
    MODE_V7_DEPO = 14,      // Deposit (v3.1.1 14: Ranked)
    MODE_V7_STALK, MODE_V7_KING, MODE_V7_OINC, MODE_V7_TRADE, MODE_V7_KC, MODE_V7_DEF, MODE_V7_SHRP,
    MODE_V7_TRAI, MODE_V7_RAID,
    MODE_V7_BLITZ = 24, MODE_V7_DOM, MODE_V7_SDM, MODE_V7_KRANK, MODE_V7_TDF, MODE_V7_DEPOFFA, MODE_V7_CAR,
    MODE_V7_AON, MODE_V7_MD, MODE_V7_CHS, MODE_V7_BHFFA, MODE_V7_ZOM, MODE_V7_IMP,
    MODE_V7_TABLE_COUNT,    // 37: gameModes.js
    MODE_V7_RANKED_POINT = MODE_V7_TABLE_COUNT, // 37: KN Ranked (objective variant; v3.1.1 modes[13])
    MODE_V7_RANKED,         // 38: KN Ranked (first to 10 kills; v3.1.1 modes[14])
    MODE_V7_COUNT
} ModeIdV7;

// gameModes.js rotationModes (v3.1.1 [0, 1, 2, 3, 19])
#define MODE_V7_ROTATION {MODE_V7_FFA, MODE_V7_POINT, MODE_V7_CTF, MODE_V7_KC, MODE_V7_DOM}
