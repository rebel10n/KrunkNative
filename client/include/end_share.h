// The end-of-match table's share / report buttons and the share popup (Krunker v10.0.7). See client/src/ui/end_share.c.
#ifndef KN_END_SHARE_H
#define KN_END_SHARE_H

#include <html_dom.h> // HtmlArg
#include <stddef.h>

// the row's actions cell content: your own row's share button (not in a custom game), another account's report
// button; empty for a guest viewer
void end_share_actions_html(char *out, size_t size, int is_you, int other_has_account, int custom_game, const char *name);
// shareResult / shareMatch / reportPlayer; -1 when the name is not one of them
int end_share_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

#endif
