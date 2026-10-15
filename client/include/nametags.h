#pragma once
// World-anchored items of Krunker v3.1.1's 2D overlay canvas (#game-overlay,
// src/libs/game-overlay.js overlay.render) on the client: client/src/nametags.c.
//
//   player nametags   health bar (team / enemy colour, dynamicHP chase, healthNum), name
//                     (streamer mode "Player <sid>"), level box, clan tag and the partner /
//                     premium / featured badges; nametagStyle, nametagOpac, hideNames;
//                     scaled by max(0.3, 1 - distance / 600) like the canvas
//   visibility        index.js gameLoop's nameVisRate (200 ms) pass: teammates always,
//                     enemies with line of sight (Game.canSee), everyone while spectating,
//                     hideNames / mode hideNames / Game.config.nameTags, and the killer's
//                     tag forced on by g1() (menu_view_killer_uid) until the next pass
//   mode markers      the bomb site letters (A, B, ...); v7's flag pings, Hardpoint and
//                     Domination markers are drawn by client/src/ui/hud_objectives.c
//
// Drawn the way the HTML HUD draws the canvas's other items (damage numbers, speedometer):
// as page elements, in a full-viewport layer inserted right after the <canvas id=
// "game-overlay"> (same z-index, under #uiBase), scaled by scaleBase's UI scale exactly like
// the canvas context (ctx.scale(scale)), so text is the page's GameFont / Material Icons on
// both the native engine and the web DOM. Positions are projected through the world camera
// into viewport fractions, so the scaled scene target (resolution / aspectRatio) is irrelevant.
#include <client.h>

// Per frame, after the world camera is posed and the scene rendered (main.c client_tick_game,
// overlay part). Hides everything by itself once it stops being called (no game).
void nametags_tick(Client *client);

// Shared with the other game-overlay items (client/src/ui/hud_objectives.c):
// project a world point through the world camera into viewport fractions (0..1, y down);
// 0 when it is outside the frustum (three.js frustum.containPoint).
int nametags_project(const Camera *camera, vec3 point, double *fx, double *fy);
// Game.canSee(origin, target) == null: nothing the hitscan would stop lies between the origin's eye
// and the target (KPD Vision's test; the --render-scenario kpdCase picks its suspect with it).
int nametags_can_see(const Game *game, const Player *origin, vec3 target);
// scaleBase's UI scale (the canvas context's ctx.scale(scale)) for a vw x vh viewport.
double nametags_ui_scale(float vw, float vh);
