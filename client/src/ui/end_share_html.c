#include <stdio.h>

// #shareTxt (deob-10.0.7.js:227052) has no maxlength. The note is sent as typed.
void end_share_popup_html(char *out, const size_t n, const char *title, const char *placeholder, const char *button) {
    snprintf(out, n,
             "<div style='color:#fff;font-size:20px;margin-bottom:15px'>%s</div>"
             "<textarea id='shareTxt' rows='5' style='width:100%%' placeholder='%s'></textarea>"
             "<div id='confirmBtn' style='width:calc(100%% - 20px)' onmouseover='playTick(0.1)' onclick='shareMatch()'>%s</div>",
             title ? title : "", placeholder ? placeholder : "", button ? button : "");
}
