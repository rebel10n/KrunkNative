#ifndef KN_PAINT_PICKER_H
#define KN_PAINT_PICKER_H

#include <html_dom.h> // HtmlEvent, HtmlArg (the inline handlers' arguments)

// The paint picker of an owned skin card (Krunker v10.0.3's paint popup: deob-10.0.3.js:121985-122180, opened from an
// inventory card at 149062-149077; doc/v10/paint-copy.md). The card's paint button (ui_loadout.c owned_card) opens it:
// a popup listing the owned paints with their counts; Paint applies the picked one to the card's unit.
//   - the card has the button only for a paintable item (store paintTargets: deob:150226) and a KrunkNative account
//     (krunker.io's paints are not applied here: v10_gapi sends no writes for that account);
//   - the list: the store paints (type 18 with paintColor or a numeric paintShader) that the account owns, with the
//     count of its owned entries of that index; owned first, then by index (deob:122041). No owned paint: v10's empty state;
//   - apply: POST /items/<sid>/paint {item_id, paint_item_id}, sid = the copy id, or the stack's store index for an
//     unpainted unit (the brain makes it a copy); the answer sets the paint on the owned list and closes the popup
//     with a toast "Painted <item> with <paint>" (deob:149073); a failure says why in the popup.

// 1 when the card of store item `ind` gets the paint button (a KrunkNative account, paintable item); 0 otherwise.
int paint_picker_can_open(int ind);

// Opens the popup for the card's unit: `ind` the store index, `sid` its skinId (copy id, or the store index of a stack).
void paint_picker_open(int ind, int sid);

// The inline handlers paintOpen(ind, sid), paintPick(index), paintApply(). -1 = not ours.
int paint_picker_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

#endif
