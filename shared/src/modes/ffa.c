#include <shared.h>

const GameModeVTable ffa_vtable = {};

FreeForAll *ffa_init() {
    FreeForAll *ffa = calloc(1, sizeof(FreeForAll));
    if (!ffa) return NULL;

    ffa->base.config = g_default_game_mode_config;
    ffa->base.vtable = &ffa_vtable;

    return ffa;
}