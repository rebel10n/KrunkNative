#pragma once
// Krunker v10.0.2's item tooltips (client/src/ui/ui_tooltip.c): window.createTip,
// createStreakTip, createSpinHistoryTip and cancelTip (g:216872-216907) over the tooltip helper
// (g:216638): one div.tooltip before <body>'s first child, placed 4 px + its height + 5 px above
// the hovered element, left-aligned with it.
#include <html_dom.h>

// createTip(el): the store item of el's data-index (data-nametag / data-wear / data-wear-float /
// data-serial when present): name in its rarity colour, lore, wear, the estimated KR.
void ui_tooltip_item(HtmlElem el);
// createStreakTip(el): the kill streak of el's data-index (name + description).
void ui_tooltip_streak(HtmlElem el);
// createSpinHistoryTip(el): the Spin History row of el's index among its siblings.
void ui_tooltip_spin_history(HtmlElem el);
// tooltip(el, html): the helper itself (any markup).
void ui_tooltip_show(HtmlElem el, const char *html);
void ui_tooltip_cancel(void); // cancelTip: every .tooltip removed
// createTip / createStreakTip / createSpinHistoryTip / cancelTip; -1 = not ours.
int ui_tooltip_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
