# Krunker v10.0.2 references

`social-*.png`: the v10 Social hub popup and views (client/src/ui/v10_social.c). v10 shows them only to a
signed-in player and its JS is never run here, so each is the hub markup the client emits (rebuilt from the
bundle templates, doc/v10/social.md) rendered by headless Chromium with v10's CSS on the static v10 page:
`python3 -I tools/uidiff/v10_social_ref.py <client_bin>` regenerates them and their regions.

`main-menu-nowplaying-long.png`: `main-menu.png`'s state with a long "Now Playing: <mode> on <map>" and a
region (the match-info bar's content-sized segments). No JS runs: the capture's own DOM
(~/.cache/kn-wt/v10-stage/v10-capture) with v10's two texts changed, laid out by headless Chromium with v10's
CSS and scripts disabled: `node tools/uidiff/capture-v10-nowplaying.js` regenerates it (`--as-captured`
renders the unchanged capture, which matches `main-menu.png`).
