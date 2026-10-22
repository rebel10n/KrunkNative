#pragma once
// v10's Junkyard tab (svelte-1vqzycn, the crafting page of the Market & Trading popup): the
// recipe cards, the search and sort bar, the craft confirmation, the result and the error popups.
// v10_market.c's TAB_JUNKYARD renders through here; the protocol is kn_brain's craft.c:
//   ["recps"]      -> ["recps", rows, junk]    (rows: v10's recps rows; null when logged out)
//   ["crft", SID]  -> ["crftr", SID, ok, msg]
#include <cJSON.h>
#include <html_ui.h>
#include <ui_logic.h>

// the tab opened: the recipes are requested again when logged in (and the listener starts)
void v10_junkyard_enter(void);
// the account changed: the recipes are dropped (the next v10_junkyard_ensure asks again)
void v10_junkyard_account_changed(void);
// the tab's render: a logged-in tab with no recipes asked for yet (or failed and not retried) asks now
void v10_junkyard_ensure(void);
// the tab's markup (inside v10_market's content)
void v10_junkyard_render(StrBuf *sb);
// the v10jy.* named handlers (search, sort, craft, cgo, cclose, rok, errok, item, stop); 1 = handled
int v10_junkyard_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

// provided by v10_market.c: re-renders the exchange's content when its Junkyard tab is showing
void v10_market_junkyard_refresh(void);
