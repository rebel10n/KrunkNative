#ifndef KN_CLAN_DISCORD_H
#define KN_CLAN_DISCORD_H
// windows[12].updateDis (deob-10.0.7.js:220480-220486): the field is sent when
// /discord(?:\.com|app\.com|\.gg)\/(?:invite\/)?([a-zA-Z0-9\-]{2,32})/gim matches.
// The i flag makes the host case-insensitive. 1 when v10 would send the field.
int clan_discord_link_ok(const char *value);
#endif
