// The flavor gameplay gates (shared/include/flavor_rules.h): a flavor without rules (FLAVOR_BASE)
// gets the base game's own answer from every helper; Krunker Strike reads FRVR's data
// (shared/include/strike_rules.h).
#include <flavor_rules.h>
#include <flavor.h>
#include <modes.h>
#include <strike_rules.h>

static const StrikeRules *rules(void) { return flavor_current()->rules; }

int flavor_class_pick(const int class_index) {
    const StrikeRules *r = rules();
    if (!r || strike_class(r, class_index)) return class_index;
    return r->default_class;
}

int flavor_class_allowed(const int class_index) {
    const StrikeRules *r = rules();
    return !r || strike_class(r, class_index) != NULL;
}

int flavor_mode_allowed(const int mode_id) {
    const StrikeRules *r = rules();
    return !r || strike_mode(r, mode_id) != NULL;
}

size_t flavor_rotation_modes(const GameFlavor flavor, const int **out) {
    const StrikeRules *r = flavor_config(flavor)->rules;
    if (r && r->rotation_mode_count) {
        if (out) *out = r->rotation_modes;
        return r->rotation_mode_count;
    }
    if (out) *out = g_rotation_modes;
    return sizeof(g_rotation_modes) / sizeof(g_rotation_modes[0]);
}

const GameConfig *flavor_default_game_config(void) {
    return rules() ? strike_game_config() : &g_default_game_config;
}

int flavor_config_default(const char *var, float *def) {
    const StrikeRules *r = rules();
    const StrikeConfigDefault *d = r ? strike_config_default(r, var) : NULL;
    if (!d) return 0;
    if (def) *def = (float) d->def;
    return 1;
}

int flavor_max_players(void) {
    const StrikeRules *r = rules();
    return r ? r->max_players : 0;
}
