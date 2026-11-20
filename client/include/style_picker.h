#ifndef KN_STYLE_PICKER_H
#define KN_STYLE_PICKER_H

#include <html_dom.h>

// v10.0.7 items.setStyle (deob-10.0.7.js:27102-27106, :108817-108850).
int style_picker_can_open(int index);
int style_picker_account_style(int index); // first owned instance's selected style, for local preview
void style_picker_open(int index, int sid);
int style_picker_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

#endif
