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
//   mode markers      CTF flag pings (flag_r / flag_b), the active Hardpoint marker
//                     (objective_1) and the bomb site letters (A, B, ...)
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
