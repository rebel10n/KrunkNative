# Krunker v10.0.2 references

`social-*.png`: the v10 Social hub popup and views (client/src/ui/v10_social.c). v10 shows them only to a
signed-in player and its JS is never run here, so each is the hub markup the client emits (rebuilt from the
bundle templates, doc/v10/social.md) rendered by headless Chromium with v10's CSS on the static v10 page:
`python3 -I tools/uidiff/v10_social_ref.py <client_bin>` regenerates them and their regions.
