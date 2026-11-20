#pragma once
// Krunker v10.0.7 item styles on the player model (KrunkCup Jerseys, the Participation Band): a worn copy's style s > 0
// draws the item with its styled texture "<type><id>_<tex>_s<s>" (generateMeshData, deob-10.0.7.js:173602 / :173611).
// Everyone's styles come from the server (NET_EXT_KN_SKIN_STYLES, sent before each SPAWN of a player with styled
// copies); you outside a game (the menu's preview), your own account's copies (account_skin_style).

// The styles player `uid` wears, replaced as a whole (a packet lists every styled worn cosmetic; count 0 clears them).
void skin_style_set(int uid, int count, const int *skins, const int *styles);
// The style player `uid` wears on store item `skin`: -1 unknown (no packet for that player), 0 none, else the style.
int skin_style_of(int uid, int skin);
void skin_style_reset(void); // a new connection: every player's styles are gone

// The texture key of `base` (e.g. "body/body_190_40") in style `style` of an item with `styles` styles: "<base>_s<style>"
// when 0 < style < styles (generateMeshData's rule), else base.
void skin_style_texture_key(char *out, unsigned long n, const char *base, int style, int styles);
// The item's ordinary preview with a _sN suffix and the alternate styleExt, when selected.
void skin_style_preview_path(char *out, unsigned long n, const char *base, int style, int styles, const char *style_ext);
// The picker shows a separate preview for every option, including index 0 (_s0).
void skin_style_picker_preview_path(char *out, unsigned long n, const char *base, int style, int styles, const char *style_ext);
