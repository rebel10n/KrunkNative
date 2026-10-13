#pragma once
// Geometry of KrunkNative's draggable / resizable HTML windows (client/src/ui/win_geom.c): the
// developer console (ui_console.c: titlebar drag, grip resize, saved as krk_dev_console_geom),
// the editor's Quick GUI (editor_quickgui.c: titlebar drag) and the editor's side panels
// (editor.c: .resize handles). Every box they restore, drag or resize goes through these, so
// none can end up outside the window after a smaller monitor / resolution, F11, an F4 or F5
// restart, or a drag past an edge.
//
// Boxes are outer boxes (offsetWidth / offsetHeight) in CSS px of the viewport
// (html_dom_viewport). Pure math: no DOM, no GL (client/tests/win_geom_test.c).

typedef struct {
    float x, y, w, h;
} WinGeom;

typedef struct {
    float min_w, min_h;   // the window's own minimum outer size (CSS min-width / min-height)
    float keep_w, keep_h; // at least this much of the box stays in the viewport; <= 0: all of it
} WinGeomRules;

// The least of a box a "keep" rule leaves on screen: a titlebar strip to grab it again.
#define WIN_GEOM_KEEP_W 120.0f
#define WIN_GEOM_KEEP_H 24.0f

// Fits `g` into a vw x vh viewport: the size shrinks to the viewport (never below the minimum),
// then the box moves until `keep_w` x `keep_h` of it (all of it when <= 0) is inside, its top
// edge (the titlebar) never above the viewport. Non-finite values count as 0. A viewport
// smaller than the kept part pins the box to the top-left corner. Returns 1 when `g` changed.
int win_geom_fit(WinGeom *g, float vw, float vh, const WinGeomRules *rules);

// A resize from the bottom-right corner (a grip): the size first stops at the viewport's right /
// bottom edge (the box does not move to make room), then win_geom_fit. Returns 1 when it changed.
int win_geom_fit_resize(WinGeom *g, float vw, float vh, const WinGeomRules *rules);
