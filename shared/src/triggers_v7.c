// STUB (WP-A): shared/include/triggers_v7.h bodies until WP-C implements the v7 trigger runtime
// (doc/v7/map-logic.md §7 WP-C). WP-C replaces this file; the other packages only call it.
// Until then the v3.1.1-shaped runtime in shared/src/triggers.c runs tas[0] of every trigger.
#include <triggers_v7.h>

void trigger_object_event(Game *game, Object *object, const int event, Player *player) {
    (void) game;
    (void) object;
    (void) event;
    (void) player;
}

void trigger_player_event(Game *game, const int event, Player *player) {
    (void) game;
    (void) event;
    (void) player;
}

void trigger_multi_event(Game *game, const int event, Player *player) {
    (void) game;
    (void) event;
    (void) player;
}

void trigger_queue_update(Game *game, const float delta) {
    (void) game;
    (void) delta;
}

void trigger_round_reset(Game *game) { (void) game; }

// (real body, kept by WP-C) the table lives as long as the caller says (KrunkScript: process lifetime)
void triggers_set_script_hooks(Game *game, const TriggerScriptHooks *hooks) {
    if (game) game->script_hooks = hooks;
}

double trigger_global_value_get(const Game *game, const char *key) {
    (void) game;
    (void) key;
    return 0.0;
}

void trigger_global_value_set(Game *game, const char *key, const double value) {
    (void) game;
    (void) key;
    (void) value;
}

double player_custom_value_get(const Player *player, const char *key) {
    (void) player;
    (void) key;
    return 0.0;
}

void player_custom_value_set(Game *game, Player *player, const char *key, const double value) {
    (void) game;
    (void) player;
    (void) key;
    (void) value;
}
