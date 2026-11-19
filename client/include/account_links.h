// client/include/account_links.h - Settings > Connections' Discord row and Connected apps (Krunker v10.0.7), in C.
//
// The Discord link component (svelte-17wd35, deob-10.0.7.js:97250-97490) over kn_brain's /discord/* (discord.c) and the
// Connected apps component (svelte-10ho5mo, :97493-97673) over kn_brain's /account/apps (oauth.c, "Login with Box
// Office"). account_ui.c draws the two mounts (#discordLinkMount, #connectedAppsMount); this module fills them and
// redraws them in place when an answer comes. For a krunker.io account the apps list is krunker.io's own
// (GET /account/apps on gapi, the account's Bearer), shown read-only: Box Office makes no writes to krunker.io.
#ifndef KN_ACCOUNT_LINKS_H
#define KN_ACCOUNT_LINKS_H

#include <html_dom.h>
#include <ui_logic.h>

// The mounts' first contents (the cached state; a load starts when there is none or it is old).
void account_links_discord_html(StrBuf *sb);
void account_links_apps_html(StrBuf *sb, int krunker_io);
// The page's handlers: knDiscordLink / knDiscordJoin / knDiscordUnlink / knDiscordCancel, knAppRevoke(id) / knAppActivity(id).
// 1 = handled.
int account_links_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// A social frame label: "discordStatus" (kn_brain: the link changed in the browser) re-reads the status.
void account_links_social(const char *label);
// Sign-out / account switch: forget everything.
void account_links_reset(void);

#endif
