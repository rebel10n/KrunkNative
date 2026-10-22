// The account split (account_split.h, doc/v10/account-split.md). Every rule about which side's login a request, card,
// row or stream may carry lives here; callers ask these functions and do not compare backends themselves.
#include <account_split.h>
#include <alt_store.h>
#include <krunker_account.h>

#include <ctype.h>
#include <stddef.h>

// krunker.io's and Box Office's logins are both case-blind (krunker.io's answer may differ in case from the one typed).
static int same_login(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return 0;
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return 0;
    }
    return *a == *b;
}

// Bit 1: the name is a krunker.io identity this client knows; bit 2: a Box Office one.
enum { SIDE_KRUNKER = 1, SIDE_KRUNKIO = 2 };

static unsigned sides_of(const char *login) {
    unsigned sides = 0;
    if (!login || !login[0]) return 0;
    if (krunker_account_signed_in() && same_login(krunker_account_name(), login)) sides |= SIDE_KRUNKER;
    const AccountInfo *kn = krunkio_account();
    if (kn && same_login(kn->name, login)) sides |= SIDE_KRUNKIO;
    const AltVault *v = alt_store_vault();
    for (int i = 0; v && i < v->count; i++) {
        if (!same_login(v->profiles[i].label, login)) continue;
        sides |= v->profiles[i].kind == ALT_KIND_KRUNKER ? SIDE_KRUNKER : SIDE_KRUNKIO;
    }
    return sides;
}

int account_split_side_of(const char *login) {
    const unsigned sides = sides_of(login);
    if (sides == SIDE_KRUNKER) return ACCOUNT_BACKEND_KRUNKER;
    if (sides == SIDE_KRUNKIO) return ACCOUNT_BACKEND_KRUNKIO;
    return -1; // neither known, or both (a name on both sides: the card's own server decides by the password)
}

int account_split_refuses(const char *login) {
    const unsigned sides = sides_of(login);
    const unsigned own = account_backend() == ACCOUNT_BACKEND_KRUNKER ? SIDE_KRUNKER : SIDE_KRUNKIO;
    const unsigned other = own == SIDE_KRUNKER ? SIDE_KRUNKIO : SIDE_KRUNKER;
    return (sides & other) && !(sides & own);
}

int account_split_row_usable(const int krunker_row) {
    return (krunker_row != 0) == (account_backend() == ACCOUNT_BACKEND_KRUNKER);
}

int account_split_krunker_identity(void) {
    return account_backend() == ACCOUNT_BACKEND_KRUNKER && krunker_account_signed_in();
}

int account_split_signed_in(void) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return krunker_account_signed_in();
    return active_account() != NULL && account_session_token()[0] != 0;
}
