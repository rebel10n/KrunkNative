#pragma once

typedef struct {
    unsigned char real_movement:1;
    unsigned char no_weapons:1;
    unsigned char no_regen:1;
    unsigned char no_reloads:1;
    unsigned char no_pickups:1;
    unsigned char teams:1;
    unsigned char dmg_team:1;
    unsigned char site:1;
    unsigned char flags:1;
    unsigned char convert_team:1;
    unsigned char kill_on_convert:1;
    unsigned char friendly:1;
    unsigned char clan_war:1;
    int *starting_loadout;
    int starting_loadout_size;
    int team_class[3];
    float speed_mlt[3];
    int force_class;
    int health;
    int lives;
    int ammo_limit;
    float hitbox_pad;
} GameModeConfig;

extern const GameModeConfig g_default_game_mode_config;

typedef struct {

} GameModeVTable;

typedef struct {
    const GameModeVTable *vtable;
    GameModeConfig config;
} GameMode;

extern const GameModeVTable ffa_vtable;

typedef struct {
    GameMode base;
} FreeForAll;

GameMode *mode_init(int);
void mode_fini(GameMode*);

FreeForAll *ffa_init();