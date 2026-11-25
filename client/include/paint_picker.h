#ifndef KN_PAINT_PICKER_H
#define KN_PAINT_PICKER_H

#include <html_dom.h> // HtmlEvent, HtmlArg (the inline handlers' arguments)

// The paint picker of an owned skin card (Krunker v10.0.7's paint popup: deob-10.0.7.js:108483-108814, opened from an
// inventory card at :147679-147686; doc/v10/paint-copy.md). The card's paint button (ui_loadout.c owned_card) opens it:
// the item viewer with the picked paint on the item, the store's paints as swatches, and Paint for the card's unit.
//   - the card has the button only for a paintable item (store paintTargets: deob-10.0.7.js:135288) and a signed-in
//     account; a krunker.io account's card needs an instance (v10.0.7's skinId: deob-10.0.7.js:135288, :147902) and its
//     apply is krunker.io's own POST /items/<instance>/paint with the account's Bearer (paint_picker.c);
//   - the swatches: every store paint (type 18 with paintColor or a numeric paintShader), owned first, then by index
//     (:108548); an owned one shows its count, an unowned one is locked and only previewed. A store without paints
//     gives v10's empty state;
//   - the preview: an iframe of /viewer.html?nobg&showcase=<item> with the picked paint (:108675), the native showcase
//     drawn over its box (viewer_embed.h);
//   - apply: POST /items/<sid>/paint {item_id, paint_item_id}, sid = the copy id, or the stack's store index for an
//     unpainted unit (the brain makes it a copy); the answer sets the paint on the owned list and closes the popup
//     with a toast "Painted <item> with <paint>" (:147728, :134249); a failure says why in the popup.

// 1 when the card of store item `ind` gets the paint button (a signed-in account, paintable item); 0 otherwise.
int paint_picker_can_open(int ind);

// Opens the popup for the card's unit: `ind` the store index, `sid` its skinId (copy id, or the store index of a stack).
void paint_picker_open(int ind, int sid);

// The inline handlers paintOpen(ind, sid), paintPick(index), paintScroll(dir), paintWheel(), paintApply(). -1 = not ours.
int paint_picker_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

#endif
