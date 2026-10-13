#pragma once
// The server's billboard ad (server.cfg billboard / billboard_image -> NET_PACKET_BILLBOARD and
// NET_PACKET_BILLBOARD_IMAGE, sent before every INIT) and Krunker's ad canvas.
//
// Krunker v7.1.4 has one billboard / ad surface: the BILLBOARD prefab (map.js addFixedPlane with
// src "pubs/b_<bb>", the six config.billboards posters). Its server no longer sends ads (v3.1.1's
// init "bill" -> game.updateBillboards was removed with the "sad" key), but utils.js still ships
// createCanvasAD / drawMultilineText, the v3.1.1 ad canvas. KrunkNative: while the server has a
// billboard, every BILLBOARD of every map shows it (the image when it loads, else the text drawn
// by createCanvasAD); without one the maps keep their posters, as in v7 (doc/v7/decisions.md).
// SIGN, LEADERBOARD and textured planes are map content, not ad slots, and are never replaced.
#include <shared.h>
#include <canvas2d.h>

#define BILLBOARD_AD_WIDTH 2048 // createCanvasAD(text, txtCol, bgCol, scale 1): 2048 x 1024
#define BILLBOARD_AD_HEIGHT 1024
#define BILLBOARD_AD_PAD 80.0 // drawMultilineText: (scale || 1) * 80
#define BILLBOARD_AD_MAX_LINES 64

typedef struct {
    char txt[64];      // "" = no text ad
    char txt_col[8];   // "#rrggbb" (default #e3e3e3)
    char bg_col[8];    // default #000000
    char image[NET_BILLBOARD_IMAGE_MAX]; // asset path under assets/, "" = none
} BillboardAd;

// Packets, then INIT: an INIT applies what arrived since the previous INIT; an INIT without any
// (another server, offline, a server without a billboard) clears the ad.
void billboard_ad_on_packet(const NetBillboardPacket *packet);
void billboard_ad_on_image_packet(const NetBillboardImagePacket *packet);
void billboard_ad_on_init(void);

// The active ad, NULL when the maps keep their own billboards.
const BillboardAd *billboard_ad_active(void);
// Whether a map object is a billboard / ad surface (the BILLBOARD prefab).
int billboard_ad_is_surface(const Object *object);
// The ad a map object shows instead of its own texture, NULL to keep it.
const BillboardAd *billboard_ad_for_object(const Object *object);

// drawMultilineText's layout of `text` on a width x height canvas: the font size it settles on,
// the size whose fit failed (it positions the lines), the lines and their pen positions.
typedef struct {
    int font_px;
    int layout_px;
    int line_count;
    char lines[BILLBOARD_AD_MAX_LINES][64];
    double x[BILLBOARD_AD_MAX_LINES];
    double y[BILLBOARD_AD_MAX_LINES];
} BillboardAdLayout;
void billboard_ad_layout(Canvas2D *c, const char *text, int width, int height, BillboardAdLayout *out);

// createCanvasAD: fills the (BILLBOARD_AD_WIDTH x BILLBOARD_AD_HEIGHT) canvas with bg_col and
// draws txt in GameFont, txt_col with a rgba(0,0,0,0.5) 1 px outline. The canvas must be sized.
void billboard_ad_draw(Canvas2D *c, const BillboardAd *ad);
