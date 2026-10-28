// Krunker v10.0.4 Candy Heist (shared/include/candy_heist.h). Every value cites deob-10.0.4.js.
#include <candy_heist.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// The event's description lines (longDesc, :19493): a list, '\n'-joined into the LiveEvent's text
static const char k_long_desc[] = "Trick-or-treat across Hollowmere\n"
                                  "Knock on lit doors for candy and bank it at the Cauldron\n"
                                  "Stay out of the Hollowmen lantern light\n"
                                  "Raid the manor hoard in the Witching Hour";

// rewards.skins (:19499) and rewards.weights (:19500): the store's skins and the rarity weights (0x37 0x23 0xa)
static const int k_reward_skins[CANDY_HEIST_SKIN_COUNT] = {0x1f85, 0x1f86, 0x1f87, 0x1f88, 0x1f8a, 0x1f8b};
static const double k_reward_weights[CANDY_HEIST_LOOT_SLOTS] = {0, 0, 55, 35, 10, 0, 0};

// The map the event pays on (eligibleMaps :19498)
static const int k_eligible_maps[1] = {CANDY_HEIST_MAP};

static const CandyHeistMode k_mode = {
    .key = CANDY_HEIST_MODE_KEY,
    .name = CANDY_HEIST_MODE_NAME,
    .alias = CANDY_HEIST_MODE_KEY,
    .description = "Trick-or-treat across Hollowmere: knock on lit doors for candy and bank it at the Cauldron. Dodge the Hollowmen.",
    .score_stat = "candy",
    .score_label = "Candy",
    .leader_stats = {"candy"},
    .end_stats = {"sid", "name", "candy", "reward"},
    .end_stat_count = 4,
    .game_time = CANDY_HEIST_GAME_TIME,
    .force_class = CANDY_HEIST_CLASS,
    .no_pickups = 1,
    .no_score_limit = 1,
    .no_score_xp = 1,
    .no_profile_stats = 1,
    .no_winner = 1,
    .prevent_custom = 1,
    .event = CANDY_HEIST_EVENT_ID,
};

const CandyHeistMode *candy_heist_mode(void) { return &k_mode; }

int candy_heist_is_mode(const char *key) { return key && !strcmp(key, CANDY_HEIST_MODE_KEY); }

int candy_heist_candy(const double score) {
    // score || 0: NaN counts 0 too (an absent stat). Math.floor for negative scores rounds down
    const double s = isfinite(score) ? score : 0.0;
    return (int) floor(s / CANDY_HEIST_SCORE_PER_CANDY);
}

void candy_heist_event_defaults(LiveEvent *out) {
    memset(out, 0, sizeof(*out));
    out->id = CANDY_HEIST_EVENT_ID;
    snprintf(out->name, sizeof(out->name), "%s", CANDY_HEIST_MODE_NAME);
    out->start_ms = CANDY_HEIST_START_MS;
    out->end_ms = CANDY_HEIST_END_MS;
    snprintf(out->long_desc, sizeof(out->long_desc), "%s", k_long_desc);
    out->long_desc_list = 1;
    out->limited_time_mode = 1;                         // limitedTimeMode (:19496)
    out->has_reward_skins = 1;
    out->reward_skin_count = CANDY_HEIST_SKIN_COUNT;
    for (int i = 0; i < CANDY_HEIST_SKIN_COUNT; i++) out->reward_skins[i] = k_reward_skins[i];
    out->has_weights = 1;
    for (int r = 0; r < LIVE_EVENT_RARITIES; r++) out->reward_weights[r] = k_reward_weights[r];
    out->reward_chance = CANDY_HEIST_REWARD_CHANCE;
    out->has_eligible_maps = 1;
    out->eligible_map_count = 1;
    out->eligible_maps[0] = k_eligible_maps[0];
}

int candy_heist_reward_skins(int *out, const int max) {
    int n = 0;
    for (int i = 0; i < CANDY_HEIST_SKIN_COUNT && n < max; i++) out[n++] = k_reward_skins[i];
    return n;
}

void candy_heist_reward_weights(double out[CANDY_HEIST_LOOT_SLOTS]) {
    for (int r = 0; r < CANDY_HEIST_LOOT_SLOTS; r++) out[r] = k_reward_weights[r];
}
