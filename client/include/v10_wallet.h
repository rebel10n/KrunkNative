#pragma once
// Krunker v10.0.2's Wallet and Inventory modal (client/src/ui/v10_wallet.c): the header's Wallet
// item (v10's _52516) pushes it on the view stack, with the KR, Junk and RP cards and the owned
// skins' grid (the account's inventory, data slot 10), its Catalog button and the Quick Sell / item
// views of the market over it.
#include <cJSON.h>
#include <html_dom.h>
#include <ui_logic.h>

int  v10_wallet_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knV10Wallet*; -1 = not ours
int  v10_wallet_key(int key);                  // Escape with the modal up: the top view goes (1 = handled)
void v10_wallet_tick(void);                    // per menu frame: the account's funds / skins changed, redraw
void v10_wallet_account_changed(void);         // the active account changed: the modal leaves the stack
void v10_wallet_scenario(const cJSON *state);  // --render-scenario state.wallet: {open: true}
