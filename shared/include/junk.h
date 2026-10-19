#pragma once
// Krunker v10.0.2 Junk, the second currency, kept to two decimal places (doc/v10/junk.md). The
// wallet stores whole hundredths (12.34 Junk = 1234): kn_brain's players.player_junk, the ledger,
// the game servers' "acct" ops and the account JSON's "junk" text. Floats appear only where a
// Junk amount is set or shown (the trigger's constant, a package's amount, the navbar's text).
#include <stddef.h>
#include <stdint.h>

#define JUNK_CREDIT_MAX 100000000 // the largest credit or spend of one call: 1,000,000 Junk, in hundredths

// v7 parseFloat(amount).toFixed(2) rounded to the wallet's unit: llround(amount * 100). NaN, infinities,
// zero and negative amounts are 0 (a credit of nothing).
int64_t junk_hundredths(double amount);

// "12.34" (two places, always; a negative balance reads as 0.00). out has room for 16 bytes or more.
void junk_format(int64_t hundredths, char *out, size_t size);

// Reads a decimal amount ("12", "12.3", "12.34", "12.345": rounded to hundredths) into hundredths.
// NULL, empty and non-numeric text are 0, like parseFloat(x || 0) with its NaN dropped.
int64_t junk_parse(const char *text);

// hundredths as a Junk amount (the navbar and the Account window take doubles).
double junk_value(int64_t hundredths);
