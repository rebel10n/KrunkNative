#pragma once
// Bot player locomotion (shared/src/bot_move.c): slide hopping ("bhop") and the jump / crouch /
// slide gating of the input frames shared/src/bots.c builds. The AI decides where to walk and
// look; bot_move_apply finishes the frame: hops where the way ahead is clear, refuses jumps
// into ceilings, walls, ledges and hazards, crouches for the landing slide.
#include <shared.h>

typedef struct GameBot_t GameBot;

// Why the last hop was refused (BotMove.reason; bot_move_reason_name)
typedef enum {
    BOT_MOVE_OK = 0,
    BOT_MOVE_NO_PATH_KIND,     // the route segment ahead is a jump / drop / ladder link
    BOT_MOVE_NO_PATH_FLAG,     // it is flagged ledge / low ceiling / hazard
    BOT_MOVE_NO_PATH_HEADROOM, // its headroom is under BOT_PATH_HOP_HEADROOM
    BOT_MOVE_NO_CEILING,       // the swept arc meets a solid above the body
    BOT_MOVE_NO_WALL,          // the swept arc meets a solid beside the body
    BOT_MOVE_NO_GROUND,        // nothing to land on (a ledge / the void)
    BOT_MOVE_NO_HAZARD,        // a jump pad / kill volume under the landing
    BOT_MOVE_NO_CORNER,        // the route turns or ends before the landing
    BOT_MOVE_NO_SPEED,         // standing still: nothing to carry
    BOT_MOVE_NO_CHANCE,        // the difficulty's cadence skipped this landing
} BotMoveReason;

// Bunny hop skill, one per bot (bot_move_skill): rolled from the bot's personality seed (the
// FNV-1a of its name and uid that bot_chat.c's personality also uses, rehashed so the two are
// independent) against thresholds that move with the difficulty, so a bot keeps its rank among
// the others when bot_difficulty changes.
typedef enum {
    BOT_HOP_NONE = 0,     // never hops: walks, jumps where the AI asks
    BOT_HOP_LEARNING = 1, // mistimed: waits on the ground, forgets the slide, hesitates, skips landings
    BOT_HOP_CLEAN = 2,    // the next frame after every clear landing, always slide hops
} BotHopSkill;
#define BOT_HOP_SKILLS 3

// Per-bot locomotion state (GameBot.move). Times in ms.
typedef struct {
    unsigned char skill;        // BotHopSkill (bot_move_skill, refreshed every tick)
    unsigned char hopping:1;    // airborne after one of our hops (or a jump the AI asked for)
    unsigned char guard:1;      // our own hop: the move key is kept to a clear rest of the flight
    unsigned char slide_land:1; // crouch on the way down: the run past the landing is clear
    unsigned char rolled:1;     // the cadence roll of this grounded spell was made
    unsigned char strafing:1;   // the last air frame used an air strafe key
    float ground_ms;            // ms grounded since the landing (0 in the air)
    float air_ms;               // ms airborne (0 on the ground)
    float retry_ms;             // the hop gate said no: ms before it is asked again
    float prev_yaw;             // the look yaw of the last frame (air strafe)
    float hop_travel;           // the planned landing distance of the current hop
    float hop_apex;             // its rise
    float hop_floor;            // the floor it took off from (the landing height of the plan)
    float wait_ms;              // learning: ms to stay on the ground after this landing before hopping
    float hesitate_ms;          // learning: ms of standing still left (a hesitation after a landing)
    int reason;                 // BotMoveReason of the last refusal
    // counters since the spawn (tests, bot_list)
    int hops, vetoes, bumps, landings, ai_jumps_refused;
    int hesitations, slides_forgotten; // learning hoppers' mistakes
    int air_holds;              // air frames whose key (or strafe) was dropped: it steered into a solid
    int refused[BOT_MOVE_NO_CHANCE + 1]; // vetoes by BotMoveReason
} BotMove;

// The planned hop of bot_move_plan (units, ms)
typedef struct {
    float travel;               // horizontal distance from the feet to the landing
    float apex;                 // rise of the arc
    float air_ms;               // time in the air
    float land_y;               // feet height of the landing
    int slide_clear;            // the ground past the landing is free for the slide's length
    int reason;                 // BOT_MOVE_OK or why it must not happen
} BotMoveHop;

void bot_move_reset(BotMove *m);
const char *bot_move_reason_name(int reason);

// Simulate a jump of `self` from its velocity (+ jumpPush along the look `look_yaw`) holding the
// move key of world yaw `key_yaw` (NAN: no key) in the air (crouched: the crouched jump height),
// and sweep the player's box along the arc against the map's solids; run_limit >= 0 caps the
// landing distance (a path corner). *out gets the arc and the verdict; returns 1 when it is safe.
int bot_move_plan(const Game *game, const Player *self, float look_yaw, float key_yaw, int crouched, float run_limit, BotMoveHop *out);

// Finish a bot's input frame for this tick (dt ms): `move` (KrunkNative move code, -1 none) is
// where the AI walks, `look_yaw` where it looks, *jump / *crouch what it asked for. Hops when
// the bot travels and the way is clear, releases / holds the crouch for the slide, refuses
// jumps that would hit a ceiling. *jump_late: press the jump on the tick's second frame (the
// first one releases the key after a landing).
void bot_move_apply(Game *game, GameBot *bot, Player *self, float dt, float look_yaw, int *move, unsigned char *jump, unsigned char *crouch, unsigned char *jump_late);

// 1 when `bot` slide hops at all: config slideHop (NextBots by default) and a skill above none
int bot_move_hops(const GameBot *bot);

// The personality seed of a bot (FNV-1a over the name, then the uid; bot_chat_personality_of)
uint32_t bot_move_seed(const char *name, int uid);
// The BotHopSkill a seed gets at `difficulty`. Mix (share of bots): easy 75% none / 25% learning,
// normal 30 / 35 / 35, hard 15% learning / 85% clean, max all clean.
int bot_move_skill_roll(uint32_t seed, float difficulty);
int bot_move_skill(const GameBot *bot);          // of this bot now (its seed and difficulty)
const char *bot_move_skill_name(int skill);       // "none" / "learning" / "clean"
