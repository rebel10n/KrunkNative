// The HUD's number text: String(n) of the values hud_ui.c prints (the damage numbers, the health bar's
// widths, the K/D ratio). GL- and DOM-free so client/tests/hud_format_test.c can check it.
#pragma once

#include <stddef.h>

// String(n) for the HUD: a whole number with no fraction ("23"), anything else rounded to the nearest
// hundredth with trailing zeros dropped ("23.4", "0.01", "9.3", "0"). Never an exponent, never more than
// two decimals: a double's noise ("9.300000000000001") and a tiny damage ("0.000000023891") round away.
void hud_format_number(double value, char *out, size_t size);
