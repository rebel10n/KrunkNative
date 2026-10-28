// Privacy & Age (Krunker v10.0.4): the k-ID section of the Account Settings tab (kid_settings.h).
//
// The answer is GET /account/kid on the gapi (deob-10.0.4.js:26519-26523, the kid API of
// gapiURL; deob-10.0.4.js:19722). The section is the component at deob-10.0.4.js:97480-97760:
//   gate        status !== "none" (97622): no session, no section
//   header      kid.settings.header "Privacy & Age" (97628), info (97631)
//   age, region kid.settings.ageCategory / kid.settings.region under the info (97638-97661)
//   status      kid.settings.status.<status> unless active (97661-97668)
//   rows        one per permission (97675-97750): its name (kid.settings.perm.<name>), its state
//               (97690-97735) and its description (97742-97745)
// The Svelte page's switches, "Ask a Parent" / "Show Code" buttons and the parent-email flow
// (97546-97620, the kid.settings.request.* dialog) are not ported yet: the rows show the state only.
#include <kid_settings.h>
#include <krunker_account.h>
#include <i18n.h>
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    KID_IDLE,    // never asked, or the last ask could not be sent (retried on the next render)
    KID_LOADING, // GET /account/kid in flight
    KID_LOADED,  // data holds the answer
    KID_FAILED,  // the answer was an error: not asked again this session
} KidLoad;

static struct {
    KidLoad load;
    cJSON *data;                 // the /account/kid answer's "data" object
    void (*on_change)(void);     // re-renders the Account Settings tab
} g_kid;

static void on_answer(void *user, const int status, const cJSON *res) {
    (void) user;
    cJSON_Delete(g_kid.data);
    g_kid.data = NULL;
    g_kid.load = KID_FAILED;
    const cJSON *data = status == 200 && res ? cJSON_GetObjectItem(res, "data") : NULL;
    if (data && cJSON_IsObject(data)) {
        g_kid.data = cJSON_Duplicate(data, 1);
        if (g_kid.data) g_kid.load = KID_LOADED;
    }
    if (g_kid.on_change) g_kid.on_change();
}

void kid_settings_refresh(void (*on_change)(void)) {
    g_kid.on_change = on_change;
    if (g_kid.load == KID_LOADING || g_kid.load == KID_LOADED || g_kid.load == KID_FAILED) return;
    if (!krunker_account_signed_in()) return;
    g_kid.load = KID_LOADING;
    if (!krunker_account_api("GET", KRUNKER_SVC_API, "/account/kid", NULL, on_answer, NULL)) g_kid.load = KID_IDLE;
}

void kid_settings_clear(void) {
    cJSON_Delete(g_kid.data);
    g_kid.data = NULL;
    g_kid.load = KID_IDLE;
}

// ---------------------------------------------------------------- the section

static const char *jstr(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static int jtrue(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return v && cJSON_IsTrue(v);
}

// `${prefix}${name}` as an i18n key, looked up with the English text as the fallback.
static const char *keyed(char *buf, const size_t n, const char *prefix, const char *name, const char *english) {
    snprintf(buf, n, "%s%s", prefix, name);
    return I18N(buf, english);
}

// v10's state line of one permission (97688-97735): the player's own switch is on, a guardian's approval
// is on or still wanted, and everything else is not available for this age and region.
static const char *state_text(const cJSON *perm, const char *pending) {
    const char *managed = jstr(perm, "managedBy");
    const char *name = jstr(perm, "name");
    const int enabled = jtrue(perm, "enabled");
    if (managed && !strcmp(managed, "PLAYER")) return enabled ? I18N("kid.settings.state.on", "On") : NULL;
    if (managed && !strcmp(managed, "GUARDIAN")) {
        if (enabled) return I18N("kid.settings.state.parentOn", "On (approved by a parent)");
        if (pending && name && !strcmp(pending, name)) return I18N("kid.settings.state.waiting", "Waiting for a parent");
        return I18N("kid.settings.state.needsParent", "Needs a parent's approval");
    }
    return enabled ? NULL : I18N("kid.settings.state.prohibited", "Not available for your age in your region");
}

static void info_row(StrBuf *sb, const cJSON *d) {
    char key[160];
    const char *age = jstr(d, "ageCategory");
    const char *region = jstr(d, "jurisdiction");
    sb_append(sb, "<div class='kidInfo'>");
    sb_append_esc(sb, I18N("kid.settings.info", "Your account is age-verified with k-ID. These settings control which features you can use."));
    if (age || region) {
        sb_append(sb, "<div class='kidMeta'>");
        if (age) {
            sb_append_esc(sb, I18N("kid.settings.ageCategory", "Age group"));
            sb_append(sb, ": <span class='kidValue'>");
            sb_append_esc(sb, keyed(key, sizeof(key), "kid.settings.age.", age, age));
            sb_append(sb, "</span>");
        }
        if (age && region) sb_append(sb, " &middot; ");
        if (region) {
            sb_append_esc(sb, I18N("kid.settings.region", "Region"));
            sb_append(sb, ": <span class='kidValue'>");
            sb_append_esc(sb, region);
            sb_append(sb, "</span>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

static void status_row(StrBuf *sb, const char *status) {
    // kid.settings.status.<status> (97661); the active state has none
    if (!strcmp(status, "pending"))
        sb_append_esc(sb, I18N("kid.settings.status.pending", "Your account is waiting for a parent to approve it. You can change these settings once they do."));
    else if (!strcmp(status, "inactive"))
        sb_append_esc(sb, I18N("kid.settings.status.inactive", "Your k-ID settings can't be changed right now."));
    else if (!strcmp(status, "prohibited"))
        sb_append_esc(sb, I18N("kid.settings.status.prohibited", "Some features aren't available for your age where you live."));
}

static void permission_row(StrBuf *sb, const cJSON *perm, const char *pending) {
    char key[160];
    const char *name = jstr(perm, "name");
    if (!name) return;
    const char *label = keyed(key, sizeof(key), "kid.settings.perm.", name, name);
    const char *state = state_text(perm, pending);
    sb_append(sb, "<div class='settName'>");
    sb_append_esc(sb, label);
    if (state) {
        sb_append(sb, jtrue(perm, "enabled") ? "<span class='kidState'>" : "<span class='kidState kidOff'>");
        sb_append_esc(sb, state);
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</div>");
    char dkey[200];
    snprintf(dkey, sizeof(dkey), "kid.settings.perm.%s.desc", name);
    const char *desc = I18N(dkey, "");
    if (desc[0]) {
        sb_append(sb, "<div class='kidDesc'>");
        sb_append_esc(sb, desc);
        sb_append(sb, "</div>");
    }
}

void kid_settings_html(StrBuf *sb) {
    if (!g_kid.data) return;
    const char *status = jstr(g_kid.data, "status");
    if (!status || !strcmp(status, "none")) return;
    sb_append(sb, "<div class='setHedS'>");
    sb_append_esc(sb, I18N("kid.settings.header", "Privacy & Age"));
    sb_append(sb, "</div><div class='setBodH' style='padding-bottom:10px;'>");
    info_row(sb, g_kid.data);
    if (strcmp(status, "active")) {
        sb_append(sb, "<div class='kidMsg'>");
        status_row(sb, status);
        sb_append(sb, "</div>");
    }
    const cJSON *perms = cJSON_GetObjectItem(g_kid.data, "permissions");
    const cJSON *pending = cJSON_GetObjectItem(g_kid.data, "pending");
    const char *pending_perm = pending ? jstr(pending, "permission") : NULL;
    if (perms && cJSON_IsArray(perms)) {
        const int count = cJSON_GetArraySize(perms);
        for (int i = 0; i < count; i++) permission_row(sb, cJSON_GetArrayItem(perms, i), pending_perm);
    }
    sb_append(sb, "</div>");
}
