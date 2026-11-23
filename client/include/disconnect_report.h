#pragma once
// Krunker v10.0.7's disconnect report (client/src/ui/disconnect_report.c): window.disconnectPopup(code, reason) and
// window.getDCExport(el, code, reason) (deob-10.0.7.js:229972-229997). The popup asks the player to copy the error
// data for support; Copy Data puts v10's JSON diagnostics blob on the clipboard and the button reads "Copied" for
// 1250 ms. 10.0.7 defines both but draws no caller (its Lost Connection screen, :229053, has no Report button), so
// Box Office registers the two handlers and draws no caller either.
#include <html_dom.h>
#include <stddef.h>

// disconnectPopup / getDCExport / window.getDCExport; -1 = not ours.
int disconnect_report_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// getDCExport's blob: JSON.stringify({ua, savedRegion, connectedRegion, connectedHost, ping, v, steam, client, mobile,
// browser, code, reason, time}, null, 2). malloc'd.
char *disconnect_report_json(const char *code, const char *reason);
