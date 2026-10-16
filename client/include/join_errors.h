#ifndef KN_JOIN_ERRORS_H
#define KN_JOIN_ERRORS_H

#include <stddef.h>

// The text a failed join shows, for the matchmaker's error word (the seek answer's "error", or the
// krunker.io matchmaker's; NULL when nothing answered at all). Where Krunker v10.0.2 has the same
// case, its matchmaker.* / error.* text (ui/i18n/en.json, v10.js:147675-147692 and the seek catch at
// v10.js:222076-222090) with its <br><small> markup flattened to plain text; the rest keep KN's own
// kn.matchmaker.* keys. Plain text, written to out (NUL-terminated, n bytes).
void join_error_text(const char *code, char *out, size_t n);

// Where a failed join shows (the decision only, the UI is in servers_ui.c / krunker_net.c):
//   JOIN_ERROR_SCREEN  the Lost Connection screen (v10's error screen, #instructionsUpdate): the
//                      join never started, so the menu is the place to say why;
//   JOIN_ERROR_POPUP   v10's error popup (genericPopup "responsePop", the red clear icon). Not used for a
//                      failed seek (v10 shows its error screen for those, join_error_place).
typedef enum {
    JOIN_ERROR_SCREEN = 0,
    JOIN_ERROR_POPUP = 1,
} JoinErrorPlace;

JoinErrorPlace join_error_place(int in_game);

#endif
