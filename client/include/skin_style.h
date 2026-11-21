#pragma once
// Krunker v10.0.7 item styles on the player model (KrunkCup Jerseys, the Participation Band): a worn copy's style s > 0
// draws the item with its styled texture "<type><id>_<tex>_s<s>" (generateMeshData, deob-10.0.7.js:173602 / :173611).
// Everyone's styles come from the server (NET_EXT_KN_SKIN_STYLES, sent before each SPAWN of a player with styled
// copies); you outside a game (the menu's preview), your own account's copies (account_skin_style).

struct ViewerSkin;

// The styles player `uid` wears, replaced as a whole (a packet lists every styled worn cosmetic; count 0 clears them).
void skin_style_set(int uid, int count, const int *skins, const int *styles);
// The style player `uid` wears on store item `skin`: -1 unknown (no packet for that player), 0 none, else the style.
int skin_style_of(int uid, int skin);
void skin_style_reset(void); // a new connection: every player's styles are gone

// The texture key of `base` (e.g. "body/body_190_40") in style `style` of an item with `styles` styles: "<base>_s<style>"
// when 0 < style < styles (generateMeshData's rule), else base.
void skin_style_texture_key(char *out, unsigned long n, const char *base, int style, int styles);
// getStylePreview(item, style) (deob-10.0.7.js:24387-24389), as an asset path for html_dom_asset_url():
// "textures/previews/cosmetics/<assetType || type>_<id>[_c<pat> | _<tex>]_s<style>.<styleExt || png>". Callers apply
// the range rule of their site (0 <= style < styles.length on the cards and the picker: deob-10.0.7.js:108869,
// :110719, :223502).
void skin_style_preview(const struct ViewerSkin *item, int style, char *out, unsigned long n);
