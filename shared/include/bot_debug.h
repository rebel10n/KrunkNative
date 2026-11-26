#pragma once
// The DEV row's Bot Debugging popup (v10.0.7 botsPopup / debugBots, deob-10.0.7.js:224128-224157): debugBots' codes,
// the toggles in NET_EXT_BOT_CONFIG bit order and each one's v10 BotConfig key. No dependencies, so the krunker.io
// link (BOTS_POPUP's config object) and the tests that build it alone can use it.
#include <stddef.h>

enum {
    BOT_DEBUG_FLAG_FOLLOW_TARGET, BOT_DEBUG_FLAG_WATCH_TARGET,  BOT_DEBUG_FLAG_MIMIC_TARGET,    BOT_DEBUG_FLAG_ATTACK_TARGET,
    BOT_DEBUG_FLAG_TARGET_TEAM,   BOT_DEBUG_FLAG_TARGET_BOTS,   BOT_DEBUG_FLAG_SLIDE_HOP,       BOT_DEBUG_FLAG_FORCE_CROUCH,
    BOT_DEBUG_FLAG_NOD_YES,       BOT_DEBUG_FLAG_ROAM,          BOT_DEBUG_FLAG_CAN_RESPAWN,     BOT_DEBUG_FLAG_USE_SPAWN_DELAY,
    BOT_DEBUG_FLAG_JOIN_MESSAGE,  BOT_DEBUG_FLAG_LEAVE_MESSAGE, BOT_DEBUG_FLAG_REMOVE_ON_DEATH, BOT_DEBUG_FLAG_VOICE_CHAT,
    BOT_DEBUG_FLAG_GOD_MODE,      BOT_DEBUG_FLAG_COUNT
};
// the actions among debugBots' codes (the others toggle a flag)
enum { BOT_DEBUG_CLEAR = 0, BOT_DEBUG_SPAWN_1 = 1, BOT_DEBUG_BRING = 2, BOT_DEBUG_JUGGERNAUT = 20, BOT_DEBUG_SPAWN_7 = 21 };

static const struct {
    int code;
    const char *key;
} k_bot_debug[BOT_DEBUG_FLAG_COUNT] = {
    {3, "followTarget"},  {4, "watchTarget"},    {6, "mimicTarget"},  {9, "attackTarget"},   {7, "targetTeam"},
    {8, "targetBots"},    {5, "slideHop"},       {10, "forceCrouch"}, {17, "nodYes"},        {19, "roam"},
    {11, "canRespawn"},   {12, "useSpawnDelay"}, {14, "joinMessage"}, {15, "leaveMessage"},  {13, "removeOnDeath"},
    {16, "voiceChat"},    {18, "godMode"},
};

// the flag a debugBots code toggles, -1 for an action or an unknown code
static inline int bot_debug_flag(const int code) {
    for (int i = 0; i < BOT_DEBUG_FLAG_COUNT; i++)
        if (k_bot_debug[i].code == code) return i;
    return -1;
}

// the flag's v10 config key ("followTarget"), NULL out of range
static inline const char *bot_debug_key(const int flag) { return flag >= 0 && flag < BOT_DEBUG_FLAG_COUNT ? k_bot_debug[flag].key : NULL; }
