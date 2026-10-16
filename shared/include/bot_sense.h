#pragma once
// Bot perception and combat (shared/src/bot_sense.c): what a NextBot player sees, hears and
// remembers, and how it aims and fires. Ported from Krunker v7.1.4 bot/nextbot/vision.ts
// (KrunkerVision: known threats with last-known positions, the recognize gate, heard / hit
// stimuli, forget after KNOWN_THREAT_FORGET_MS, threat scores) and combat.ts (aim parts, fire
// modes) on top of game.canSee (shared/src/ai.c), with the aim-focus model of krunkerConfig.ts
// (an error that decays while locked on) and KrunkNative additions documented in bot_sense.c:
// peripheral vision, footsteps, target leading, recoil-aware bursts and a last-known-position
// search. Runs on the authority only, like bots.c. Times are ms, angles radians (Krunker look
// convention: yaw 0 faces -Z, pitch up positive), distances Krunker units.
#include <shared.h>

#define BOT_SENSE_THREATS 8            // known threats per bot (the closest ones win a full table)
#define BOT_SENSE_SCAN_MS 100.0f       // VISION_SCAN_INTERVAL_MS 60 (LOS rays: 100 here, like bots.c)
#define BOT_SENSE_FORGET_MS 10000.0f   // KNOWN_THREAT_FORGET_MS (the max profile; lower tiers forget sooner)
#define BOT_SENSE_VISIBLE_BONUS 250.0f // KNOWN_THREAT_VISIBLE_BONUS
#define BOT_SENSE_SWITCH_MARGIN 350.0f // TARGET_SWITCH_SCORE_MARGIN
#define BOT_SENSE_SWITCH_HOT_MS 1500.0f // a switch this soon after the last engagement is a flick (a third of the reaction)
#define BOT_SENSE_RECOGNIZE_MS 120.0f  // VISION_MIN_RECOGNIZE_MS (divided by the skill)
#define BOT_SENSE_PREDICT_MS 800.0f    // how far a lost target's last velocity is extrapolated
#define BOT_SENSE_SEARCH_STEP_MS 2500.0f // one search spot is looked at this long at most
#define BOT_SENSE_SEARCH_REACHED 40.0f // a search spot / predicted position counts as reached within it
#define BOT_SENSE_SEARCH_AREA 250.0f // a search spot's time (BOT_SENSE_SEARCH_STEP_MS) runs within this of the predicted position

// combat.ts AimPart (aimPointForPart): points on a player a ray is traced to, best first.
typedef enum { BOT_AIM_GUT = 0, BOT_AIM_CHEST, BOT_AIM_HEAD, BOT_AIM_FEET, BOT_AIM_ORIGIN, BOT_AIM_PARTS } BotAimPart;

// How a threat is known: the stimulus of its last known position.
typedef enum { BOT_STIMULUS_NONE = 0, BOT_STIMULUS_SEEN, BOT_STIMULUS_HEARD, BOT_STIMULUS_HIT } BotStimulus;

// vision.ts KnownThreat
typedef struct {
    int uid;                  // -1 = free slot
    vec3 known;               // lastKnownPosition (feet)
    vec3 velocity;            // units/s when last seen (0 when heard / hit)
    vec3 sample_pos;          // position at the previous scan (velocity sampling)
    float sample_at;          // ms of that sample, -1 none
    float known_at;           // lastKnownAt (sense time, ms)
    float visible_at;         // lastVisibleAt, -1 never
    float visible_since;      // -1 while not visible
    float acquired_at;
    float aware_at;           // becameAwareAt, -1 not yet (the recognize gate)
    float reacquired_at;      // -1 none: visible again after a gap (TARGET_REACQUIRE_DELAY_MS)
    unsigned int shots;       // the player's shot serial at the last scan (hearing)
    unsigned char visible:1;
    unsigned char peripheral:1;   // visible only in the peripheral band (recognized slower)
    unsigned char known_seen:1;   // hasLastKnownPositionBeenSeen: the spot is in view and empty
    unsigned char stimulus:2;     // BotStimulus of `known`
    unsigned char part:3;         // the BotAimPart the last ray reached
    unsigned char given_up:1;     // the chase of a never-seen threat ran out: not re-picked until seen or hit
    float hit_at;                 // the last time this threat hit the bot (sense time, ms), -1 never
} BotThreat;

// A difficulty tier's senses and marksmanship (bot_sense_profile).
typedef struct {
    float fov_half;           // central vision half-angle
    float periph_half;        // peripheral half-angle (a target there is seen within periph_range only)
    float periph_range;
    float hear_shots;         // gunfire is heard within this distance
    float hear_steps;         // running footsteps within this distance
    float recognize_ms;       // a target must stay in view this long before it is acted on
    float reaction_ms;        // from acting on a fresh target to the first shot
    float memory_ms;          // a threat without a stimulus is forgotten after this
    float aim_error;          // initial tracking error (rad) on a fresh lock
    float aim_error_locked;   // the floor the error decays to while locked on
    float aim_decay;          // 1/s: error = (initial - locked) * exp(-decay * t) + locked
    float aim_interval_ms;    // a new error sample this often (aimFocusInterval)
    float lead;               // 0..1: how well moving targets are led
    float track_rate;         // rad/ms the aim slews at
    int burst_shots;          // automatic fire: shots per burst (0 = hold the trigger)
    float burst_rest_ms;      // rest between bursts
    float recoil_pull;        // 0..1 of the recoil climb pulled back down
    float recoil_settle;      // an uncompensated climb above this pauses the trigger
} BotSenseProfile;

// Per-bot perception state (GameBot.sense).
typedef struct {
    float time;               // visionTime (ms since the spawn)
    float scan_timer;
    BotThreat threats[BOT_SENSE_THREATS];
    // the hunt for a target that broke line of sight
    int search_uid;           // -1 none
    vec3 search_point;
    float search_timer;
    int search_step;
    unsigned char search_active:1;
    // aim focus
    int aim_uid;              // the target the lock timer counts for
    float lock_ms;            // continuous visible time on aim_uid
    float unseen_ms;          // ms since aim_uid was last visible
    float reaction_left;      // ms before the trigger may be pulled
    float engaged_at;         // sense time of the last engagement (bot_sense_engage), with has_engaged
    unsigned char has_engaged;
    float err_yaw, err_pitch; // the current tracking error
    float err_goal_yaw, err_goal_pitch, err_timer;
    float aim_yaw, aim_pitch; // the last true target angles bot_sense_aim saw (angular speed)
    unsigned char aim_valid:1; // aim_yaw / aim_pitch hold a previous sample
    // fire control
    unsigned int shots_serial; // the bot's own shot serial at the last fire decision
    int burst_count;
    float burst_rest;
    unsigned char trigger:1;
} BotSense;

// What a scan treats as an enemy (bots.c bot_is_enemy).
typedef struct {
    unsigned char target_team;  // ignore teams
    unsigned char target_bots;  // may target bots (tfue noBots)
    float max_distance;         // targetDistance
} BotSenseRules;

void bot_sense_reset(BotSense*);
// BOT_PROFILE_PRESETS by profileForDifficulty, the config accuracy (0..100) scaling the aim error.
BotSenseProfile bot_sense_profile(float difficulty, float accuracy);
// combat.ts aimPointForPart
vec3 bot_sense_aim_part(const Player*, BotAimPart);
// game.canSee from self's eye to the first visible aim part of `other` (-1 = none visible), the
// field of view ignored (KrunkerVision.VisiblePointOnThreat).
int bot_sense_los(const Game*, const Player *self, const Player *other);
// Line of sight from self's eye to a point.
int bot_sense_los_point(const Game*, const Player *self, vec3 point);
// One tick of perception: every BOT_SENSE_SCAN_MS (and at once when `hurt`) the enemies in view
// (FOV + peripheral band, line of sight), gunfire and footsteps in earshot and whoever hurt us
// refresh the known threats; stale ones are forgotten. yaw: the bot's look yaw.
void bot_sense_update(Game*, BotSense*, const Player *self, float yaw, const BotSenseProfile*, const BotSenseRules*, int hurt, float dt);
// Seed / refresh a threat from a non-visual stimulus at `position` (SeedKnownThreat). A hit re-opens
// a threat given up (bot_sense_give_up); a sound does not.
void bot_sense_seed(BotSense*, const Player *other, vec3 position, BotStimulus);
// The chase of a threat the bot never saw ran out: its memory stays for the search, but
// bot_sense_select will not pick it again until it is seen or hits the bot. Without this, gunfire in
// earshot keeps refreshing a memory nobody has seen, and a fight never ends for the bot.
void bot_sense_give_up(BotSense*, int uid);
// Whether a shot from self's eye along the look (yaw / pitch, radians) over `distance` reaches that
// far without a wall in the way: the centre ray and the eight rays at the edges and corners of the
// square `spread` radians either side of it in yaw and pitch (player.c's per-axis spread), game.canSee
// to each. The line of fire, for a shot the aim, recoil and spread may place anywhere in that square.
// A ray's far end never drops below `floor_y` (the target's feet): the ground the target stands on
// is where a spread miss lands, not cover in the way.
int bot_sense_clear_shot(const Game*, const Player *self, float yaw, float pitch, float distance, float spread, float floor_y);
// The widest per-axis angle (radians) a ray of self's weapon may leave off its look on a shot fired
// `dt` ms from now (the next tick): player.c combat's (spread + inaccuracy) * spreadAdjustment with
// the recoil spread that tick adds, the projectile spreads' largest factor, or the weapon's custom
// spread pattern at its largest scale (0 for no weapon / melee / noSpread).
float bot_sense_shot_spread(const Player *self, float dt);
// The recoil climb (radians, added to the pitch) a shot fired `dt` ms from now leaves with: player.c
// moves recoilAnimY by the recoil force and its recovery before the shot of that tick.
float bot_sense_shot_climb(const Player *self, float dt);
const BotThreat *bot_sense_threat(const BotSense*, int uid);
int bot_sense_visible(const BotSense*, int uid);
// Ms since `uid` last hit the bot (-1: never, or not known).
float bot_sense_hit_age(const BotSense*, int uid);
// The threat to engage (SelectKnownThreat): the best score (distance + age * 0.05 - visible
// bonus) among the aware threats within max_distance; the current one keeps a 350 margin; the
// ignored uid is skipped. NULL = none known.
Player *bot_sense_select(Game*, const BotSense*, const Player *self, const BotSenseRules*, int current_uid, int ignore_uid);
// Where a known threat probably is now: the last known position, extrapolated along its last
// seen velocity for up to BOT_SENSE_PREDICT_MS of the age. 0 = not known.
int bot_sense_predict(const BotSense*, int uid, vec3 *out);
// The spot to look for a lost target at: its predicted position first, then (once there, or once
// the last known spot is in view and empty) spots fanning out from it along the way it was going,
// a new one every BOT_SENSE_SEARCH_STEP_MS or once `self` reaches it (`arrived`). 0 = forgotten.
int bot_sense_search(Game*, BotSense*, int uid, const Player *self, int arrived, float dt, vec3 *out);
// The aim: where to look to hit `target` (its visible part, or `fallback` when not visible) with
// target leading, the tracking error of the lock state and the recoil pull. Writes the look
// angles wanted; the caller slews toward them at its turn rate.
void bot_sense_aim(BotSense*, const BotSenseProfile*, const Player *self, const Player *target, int visible, vec3 fallback, float dt, float *want_yaw, float *want_pitch);
// The tracking error magnitude (rad) after lock_ms on a target (the aim-focus curve).
float bot_sense_aim_error(const BotSenseProfile*, float lock_ms);
// aim point led by the target's velocity: projectile flight time, or the tracking lag of hitscan.
vec3 bot_sense_lead(const BotSenseProfile*, vec3 eye, vec3 point, vec3 velocity, const Weapon*);
// Start the reaction clock for a target the bot just decided to engage (AttackState::OnEnter);
// `self` syncs the fire control with the bot's own shot count.
void bot_sense_engage(BotSense*, const BotSenseProfile*, const Player *self, int uid, float extra_ms);
// The trigger for this tick: 1 to fire. `aimed`: the look is within tolerance; `visible`: the
// target is in sight; `in_range`: within the weapon's range. Reaction time, bursts with rests,
// pauses while the recoil climb settles, semi-automatic pacing by the weapon's rate.
int bot_sense_fire(BotSense*, const BotSenseProfile*, const Player *self, int aimed, int visible, int in_range, float dt);
