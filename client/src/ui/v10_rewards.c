// Krunker v10.0.2 rewards on the v10 page (ui/, soon ui/): the signup rewards, the
// Challenges window and the Battle Pass entry points, driven by kn_brain's real data.
//
// Sources (the bundle, ~/.cache/kn-v10/v10.js; references "captured from the offline v10 sandbox"):
//   _39408  the Signup Rewards popup (window.showSignupRewards: genericPopup(_, "signupRewardsPop"),
//           #signupRewardsButton in #signedOutHeaderBar; REGISTER = clearPops + loginOrRegister(true))
//           and the end screen's #matchEndSignupIncentives (props.origin "match_end")
//   _39370  its "Unclaimed Rewards" box: a guest's $krAccumulated / $scoreAccumulated (GET
//           /guest/profile); KrunkNative has no guest profiles, so the box stays empty (as v10
//           shows it to a guest who earned nothing)
//   _66775  "Welcome to the Family!" (POPUP_REGISTER_REWARDS "prr" skinRewards, premiumRewards:
//           genericPopup(_, "registerRewardsPop"); Customize -> clearPops, showWindow(53)); kn_brain
//           sends "prr" once per account after the registration (services/brain/rewards.c)
//   windows[32]  the Challenges window (showWindow(33)): openHome (guest: loginOrRegister),
//           openChallengeCat / closeChallengeCat, gen (the map / weapon / nightmare sections and
//           v10's Ranked Challenges block, #challRankedHeadr) and genChallengeCard
//   _52122  the sidebar's Battle Pass item (.bpItem in #menuItemContainer, signed in only):
//           the pass name, "LVL n" and its bar (_52111), or MAX; click = openBattlePass()
//
// The Battle Pass window itself (windows[5], showWindow(6)) is battlepass_ui.c's.
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <map_catalog.h>
#include <ruleset.h>
#include <season.h>
#include <weapons.h>
#include <v10_menu.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../viewer/viewer_store.h"

#define CHAL_WINDOW 33
#define MIN_CHAL_LEVEL 1 // config.minChalLevel

static char g_chal_typ[12]; // windows[32].openChallenge.typ ("" = home)
static int g_chal_ind;      // windows[32].openChallenge.ind
static char g_bp_item_key[160]; // what the sidebar item shows (redrawn when it changes)


/* ------------------------------------------------------------------ popups */

static void pop_show(const char *html, const char *cls) {
    ui_loadout_clear_pops(); // resetPops()
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_display(holder, "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// _39408 (origin "button" | "match_end")
static char *signup_html(const int match_end) {
    StrBuf sb = {0};
    sb_appendf(&sb,
               "<div class=\"signup-rewards-container %s svelte-1v6b0qm\"><div class=\"signup-rewards-header svelte-1v6b0qm\">Join Krunker today!</div> "
               "<div class=\"signup-rewards-content svelte-1v6b0qm\"><div class=\"signup-rewards-list %s svelte-1v6b0qm\">",
               match_end ? "signup-rewards-container--match-end" : "", match_end ? "signup-rewards-list--match-end" : "");
    static const char *const perks[] = {"Complete challenges", "Spend KR on cosmetics", "Customize your avatar"};
    for (int i = 0; i < 3; i++)
        sb_appendf(&sb, "<div class=\"signup-reward svelte-1v6b0qm\"><i class=\"material-icons\" style=\"color:#3be354; font-size: 29px; "
                        "margin-right: 18px;\">done</i> %s</div> ", perks[i]);
    sb_append(&sb,
              "<div class=\"signup-reward svelte-1v6b0qm\" style=\"margin-left: 88px;\">And much more!</div></div> "
              "<div class=\"rewards-side-panel svelte-1v6b0qm\"><div class=\"rewards-side-panel-title svelte-1v6b0qm\">"
              "<p class=\"svelte-1v6b0qm\">1-time signup perks:</p></div> <div class=\"rewards-side-panel-description svelte-1v6b0qm\">"
              "<p class=\"svelte-1v6b0qm\">Gain 14 Cosmetics +</p> <p style=\"margin-top: 0px;\" class=\"svelte-1v6b0qm\">1 Day premium for free!</p>"
              "</div></div> <div class=\"signup-rewards-guest-earned-collect svelte-1v6b0qm\"></div> "
              "<div id=\"knSignupRegister\" class=\"button bp-generic-btn signup-rewards-register-button svelte-1v6b0qm\" onmouseenter=\"playTick(0.1)\" "
              "onclick=\"playSelect(0.1);knSignupRegister()\">REGISTER</div></div></div>");
    return sb_finish(&sb);
}

// window.showSignupRewards()
void v10_show_signup_rewards(void) {
    if (active_account()) return; // _39408 renders nothing for an account
    char *html = signup_html(0);
    pop_show(html, "signupRewardsPop");
    free(html);
    fprintf(stderr, "[rewards] signup rewards popup\n");
}

// The end screen (endScreen.init's 12th callback): mounts _39408 into #matchEndSignupIncentives
// and shows it; it stays empty for an account.
void v10_rewards_end_screen(void) {
    HtmlElem el = html_dom_by_id("matchEndSignupIncentives");
    if (!el) return;
    if (active_account()) {
        html_dom_set_html(el, "");
        html_dom_set_display(el, "none");
        return;
    }
    char *html = signup_html(1);
    html_dom_set_html(el, html);
    free(html);
    html_dom_set_display(el, "block");
}

// _66775 for the "prr" push
static void register_rewards_popup(const int skins, const int premium) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-box svelte-6dkuug\"><div class=\"io-popup-container\"><div class=\"io-popup-header\">Welcome to the Family!</div> "
                   "<div class=\"gifts-text svelte-6dkuug\">Here's some gifts for you to set you off to a great start</div> "
                   "<div class=\"io-popup-content\"><div class=\"rewards-container svelte-6dkuug\">");
    char url[256];
    if (premium) {
        ui_img_url("img/prem_crd.png", url, sizeof url);
        sb_appendf(&sb, "<div class=\"reward-holder svelte-6dkuug\"><div class=\"premiumReward svelte-6dkuug\" style=\"background-image: url(%s)\">"
                        "24 Hours of Premium!</div></div>", url);
    }
    if (skins > 0) {
        ui_img_url("img/register_rewards_crd.png", url, sizeof url);
        sb_appendf(&sb, " <div class=\"reward-holder svelte-6dkuug\"><div class=\"cosmetics-reward svelte-6dkuug\" style=\"background-image: url(%s)\">"
                        "+%d Cosmetics</div></div>", url, skins);
    }
    sb_append(&sb, "</div> <button class=\"io-button io-button--accept\" onmouseenter=\"playTick(0.1)\" "
                   "onclick=\"clearPops();playSelect(0.1);showWindow(53)\">Customize</button></div></div></div>");
    char *html = sb_finish(&sb);
    pop_show(html, "registerRewardsPop");
    free(html);
}

// kn_brain "prr" (POPUP_REGISTER_REWARDS): [skin...], premium
int v10_rewards_frame(const char *label, const cJSON *frame) {
    if (strcmp(label, "prr") != 0) return 0;
    const cJSON *skins = cJSON_GetArrayItem(frame, 1), *it;
    const int premium = cJSON_IsTrue(cJSON_GetArrayItem(frame, 2));
    int n = 0;
    cJSON_ArrayForEach(it, (cJSON_IsArray(skins) ? skins : NULL)) if (cJSON_IsNumber(it)) n++;
    fprintf(stderr, "[rewards] register rewards: %d cosmetics%s\n", n, premium ? " + premium" : "");
    if (active_account()) register_rewards_popup(n, premium);
    return 1;
}

/* ------------------------------------------------------------------ windows[32] */

static double cnum(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : 0.0;
}

static int chas(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsNumber(v) || cJSON_IsBool(v);
}

static const char *ctype(const cJSON *c) {
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(c, "T"));
    return t ? t : "";
}

static void cap_first(StrBuf *sb, const char *s) { // UTILS.capFirst
    if (!s || !s[0]) return;
    char first[2] = {(char) toupper((unsigned char) s[0]), 0};
    sb_append_esc(sb, first);
    sb_append_esc(sb, s + 1);
}

static void skin_tile(StrBuf *sb, const int index) {
    const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
    sb_append(sb, "<div class=\"chalRewSkn\" style=\"");
    if (skin) {
        char url[256];
        store_skin_preview(index, url, sizeof(url));
        const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
        sb_appendf(sb, "background-image:url(%s);border:2px solid %s;\" onclick=\"viewItem(%d)\"></div>", url, g_viewer_rarities[r].color, index);
    } else {
        sb_append(sb, "\"></div>");
    }
}

// genChallengeCard(challenge, marginTop)
static void chal_card(StrBuf *sb, const cJSON *c, const int margin) {
    const cJSON *dat = cJSON_GetObjectItem(c, "DAT"), *rew = cJSON_GetObjectItem(c, "REW");
    const int boss = cnum(dat, "boss") != 0.0;
    const int done = cnum(c, "PT") != 0.0;
    const char *ky = cJSON_GetStringValue(cJSON_GetObjectItem(dat, "ky"));
    const int time_key = ky && !strcmp(ky, "time");
    const double val = cnum(dat, "val"), pv = cnum(c, "PV");
    // REW.sk (up to 3 shown, last first), then REW.title tiles and an RP tile (v10)
    int skins[3] = {-1, -1, -1}, n = 0;
    const cJSON *sk = cJSON_GetObjectItem(rew, "sk"), *it;
    if (cJSON_IsNumber(sk)) skins[n++] = sk->valueint;
    cJSON_ArrayForEach(it, (cJSON_IsArray(sk) ? sk : NULL)) if (n < 3 && cJSON_IsNumber(it)) skins[n++] = it->valueint;
    char tiles[3][320];
    int tile_count = 0;
    const cJSON *title = cJSON_GetObjectItem(rew, "title");
    if (title && !cJSON_IsNull(title) && tile_count < 3)
        snprintf(tiles[tile_count++], sizeof tiles[0], "<div class=\"chalRewSkn chalRewTile\" style=\"border:2px solid #fff;\" title=\"Title\" "
                                                     "onclick=\"showTitlesWindow()\"><span class=\"material-icons\">workspace_premium</span></div>");
    if (cnum(rew, "rp") > 0 && tile_count < 3)
        snprintf(tiles[tile_count++], sizeof tiles[0], "<div class=\"chalRewSkn chalRewTile chalRewRP\" style=\"color:#ff6a0b;border:2px solid #ff6a0b;\">"
                                                     "%.15g<br>RP</div>", cnum(rew, "rp"));
    char icon[256], path[64], name[160], text[64];
    snprintf(path, sizeof(path), "img/chals/icon_%d.png", (int) cnum(c, "IC"));
    ui_img_url(path, icon, sizeof(icon));
    account_challenge_name(c, name, sizeof(name));
    sb_appendf(sb, "<div class=\"chalCard\" style=\"margin-top:%dpx;%s\"><img class=\"chalImgC\" src=\"%s\" style=\"%s%s\">", margin,
               boss ? "box-shadow: 0 0 14px #ffa000;background:rgb(79 50 0 / 40%);border:4px solid #ff9c00;" : "", icon,
               boss ? "border: 4px solid #ff9c00;" : "", done ? "filter:grayscale(1);brightness(0.8);" : "");
    if (done) sb_append(sb, "<div class=\"challComplB\"><span class=\"material-icons\" style=\"font-size:110px;color:#38cd3a;\">done</span></div>");
    sb_append(sb, "<div class=\"chalRight\">");
    sb_append_esc(sb, name);
    if (cnum(c, "TR") > 0) {
        account_readable_time(text, sizeof(text), cnum(c, "TR"));
        sb_appendf(sb, "<div class=\"chalExpr\">%s left</div>", text);
    }
    if (done) snprintf(text, sizeof(text), "Completed");
    else if (time_key) account_readable_time(text, sizeof(text), val);
    else snprintf(text, sizeof(text), "%.15g/%.15g", pv, val);
    sb_appendf(sb, "<div class=\"chalXPBar\"><div class=\"chalXPVal\">%s</div><div class=\"chalXPBarC\" style=\"width:%.15g%%;\"></div></div></div>", text,
               done ? 100.0 : time_key || val == 0.0 ? 0.0 : (pv / val * 100.0 > 100.0 ? 100.0 : pv / val * 100.0));
    const double kr = cnum(rew, "kr"), jnk = cnum(rew, "jnk");
    sb_appendf(sb, "<div class=\"chalRews\" style=\"%s\"><div class=\"chalRewV\" style=\"%s\"><span style=\"color:white\">+</span>%.15g "
                   "<span style=\"color:#fbc02d\">KR</span></div>",
               boss ? "border-left: 3px solid #ff9c00;" : "", kr ? "" : "filter:grayscale(1);", kr);
    sb_appendf(sb, "<div class=\"chalRewV\" style=\"%s\"><span style=\"color:white\">+</span>%.15g <span class=\"material-icons\" "
                   "style=\"color:#cc51f8;font-size:29px;vertical-align:top;\">plumbing</span></div><div>",
               jnk ? "" : "filter:grayscale(1);", jnk);
    // _71394 = skins.concat(tiles); the three slots right to left
    for (int i = 0; i < 3; i++) {
        const int at = 2 - i;
        if (at < n) skin_tile(sb, skins[at]);
        else if (at - n < tile_count) sb_append(sb, tiles[at - n]);
        else skin_tile(sb, -1);
    }
    sb_append(sb, "</div></div></div>");
}

#define LIST_MAX 256

// weapons[i].src || weapons[i].challSrc (the Combat Knife's "weapon_k")
static const char *chall_src(const Weapon *w) { return w && w->src ? w->src : w && w->melee ? "weapon_k" : "noimg"; }

// (boss ? 2 : PT) descending, stable
static int sorted(const cJSON **out, const char *type, const char *field, const int value) {
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, account_challenges()) {
        if (n >= LIST_MAX || strcmp(ctype(c), type) != 0) continue;
        const cJSON *dat = cJSON_GetObjectItem(c, "DAT");
        if (field && (!chas(dat, field) || (int) cnum(dat, field) != value)) continue;
        out[n++] = c;
    }
    for (int i = 1; i < n; i++) {
        const cJSON *x = out[i];
        const double kx = cnum(cJSON_GetObjectItem(x, "DAT"), "boss") ? 2.0 : cnum(x, "PT");
        int j = i - 1;
        while (j >= 0 && (cnum(cJSON_GetObjectItem(out[j], "DAT"), "boss") ? 2.0 : cnum(out[j], "PT")) < kx) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = x;
    }
    return n;
}

static void count(const char *type, const char *field, const int value, int *done, int *total) {
    *done = *total = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, account_challenges()) {
        if (strcmp(ctype(c), type) != 0) continue;
        const cJSON *dat = cJSON_GetObjectItem(c, "DAT");
        if (field && (!chas(dat, field) || (int) cnum(dat, field) != value)) continue;
        (*total)++;
        *done += cnum(c, "PT") != 0.0;
    }
}

// challHeadrI: a section header (or a category's, with the back arrow); complete = chalCardH
static void header(StrBuf *sb, const char *id, const char *image, const int margin, const char *title, const int done, const int total,
                   const int back, const int complete, const char *extra) {
    char url[256], path[96];
    snprintf(path, sizeof(path), "img/chals/%s.png", image);
    ui_img_url(path, url, sizeof(url));
    sb_appendf(sb, "<div %s%s%sclass=\"challHeadrI%s\" style=\"background-image:url(%s);margin-top:%dpx;%s\">", id ? "id=\"" : "", id ? id : "",
               id ? "\" " : "", complete ? " chalCardH" : "", url, margin, extra ? extra : "");
    if (back) sb_append(sb, "<div class=\"challHeadrBck\" onclick=\"windows[32].closeChallengeCat();\"><span class=\"material-icons challBackArr\">arrow_back</span></div>");
    sb_append(sb, "<div class=\"challHeadrT\">");
    sb_append_esc(sb, title);
    sb_appendf(sb, "</div><div class=\"challHeadrS\">%d<span style=\"color:#fff;\">/%d</span></div></div>", done, total);
}

#define ABANDON_MSG "<div class=\"challCrdMsg\" style=\"margin-bottom:25px;font-size:27px;color:#9f9f9f;\">Abandoning a match will result in the loss of challenge progress.</div>"

static void cards(StrBuf *sb, const char *type, const char *field, const int value, const char *empty) {
    const cJSON *list[LIST_MAX];
    const int n = sorted(list, type, field, value);
    sb_append(sb, "<div style=\"margin-top:10px;\">");
    for (int i = 0; i < n; i++) chal_card(sb, list[i], i ? 20 : 0);
    if (!n && empty) sb_appendf(sb, "<div class=\"challCrdMsg\">%s</div>", empty);
    sb_append(sb, "</div>");
}

// windows[32].gen() for a signed-in account (v10.0.2 index.js:214406). v10's Event Challenges
// need an active config.events entry (UTILS.hasActiveEvents); KrunkNative has none, so the
// section never shows (as in v10 between events).
static char *gen_challenges(const AccountInfo *acc) {
    if (acc->level < MIN_CHAL_LEVEL) return NULL; // the "for Challenges" requirement card (account_ui.c)
    const cJSON *list = account_challenges();
    if (!list) {
        account_challenges_request();
        return strdup(I18N("generic.loading.simple", "Loading"));
    }
    if (cJSON_GetArraySize(list) == 0) return strdup("<span style=\"color: #fff;\">No active challenges.</span>");
    StrBuf sb = {0};
    int done, total;
    char title[128];
    {
        int all = 0, completed = 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, list) {
            all++;
            completed += cnum(c, "PT") != 0.0;
        }
        fprintf(stderr, "[challenges] window %s%s: %d active, %d completed\n", g_chal_typ[0] ? g_chal_typ : "home", g_chal_typ[0] ? "" : "", all, completed);
    }
    if (!strcmp(g_chal_typ, "map")) {
        const int catalog = g_chal_ind >= 0 && g_chal_ind < MAP_CATALOG_OFFICIAL_COUNT ? map_catalog_official_index(g_chal_ind) : -1;
        const MapCatalogEntry *m = catalog >= 0 ? map_catalog_at((size_t) catalog) : NULL;
        snprintf(title, sizeof(title), "%s Challenges", m ? m->name : "");
        count("map", "mp", g_chal_ind, &done, &total);
        header(&sb, NULL, "hed_1", 0, title, done, total, 1, 0, NULL);
        sb_append(&sb, ABANDON_MSG);
        cards(&sb, "map", "mp", g_chal_ind, "Check back later for new challenges!");
        return sb_finish(&sb);
    }
    if (!strcmp(g_chal_typ, "weapon")) {
        const Weapon *w = g_chal_ind >= 0 && g_chal_ind < WEAPON_COUNT ? rules_weapon(g_chal_ind) : NULL;
        snprintf(title, sizeof(title), "%s Challenges", w && w->name ? w->name : "");
        count("weapon", "wep", g_chal_ind, &done, &total);
        header(&sb, NULL, chall_src(w), 0, title, done, total, 1, 0, "filter: saturate(0.8);");
        sb_append(&sb, ABANDON_MSG);
        cards(&sb, "weapon", "wep", g_chal_ind, "Check back later for new challenges!");
        return sb_finish(&sb);
    }
    // home: Map Challenges
    count("map", NULL, 0, &done, &total);
    header(&sb, NULL, "hed_1", 0, "Map Challenges", done, total, 0, done >= total, NULL);
    sb_append(&sb, "<div style=\"margin-top:10px;text-align:center;\">");
    for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) { // i: Krunker's mapIndex (the challenge's id)
        const int catalog = map_catalog_official_index(i);
        const MapCatalogEntry *m = catalog >= 0 ? map_catalog_at((size_t) catalog) : NULL;
        int d, t;
        count("map", "mp", i, &d, &t);
        if (!m || !t) continue;
        char path[64], url[256];
        ui_img_url(map_catalog_thumb(m, path, sizeof(path)), url, sizeof(url));
        sb_appendf(&sb, "<div class=\"challMapCrd%s\" style=\"background-image:url(%s)\" onmouseenter=\"playTick()\" "
                        "onclick=\"windows[32].openChallengeCat('map',%d);\"><div class=\"challMapCvr\"></div><div class=\"challMapName blackShad\">",
                   d >= t ? " chalCardH" : "", url, i);
        cap_first(&sb, m->name);
        sb_appendf(&sb, "</div><div class=\"challMapPrg\">%d<span style=\"color:#fff;\">/%d</span></div></div>", d, t);
    }
    sb_append(&sb, "</div>");
    // map challenges with no map (DAT.mp unset): their own cards
    const cJSON *c;
    cJSON_ArrayForEach(c, list) {
        if (strcmp(ctype(c), "map") != 0 || chas(cJSON_GetObjectItem(c, "DAT"), "mp")) continue;
        sb_append(&sb, "<div style=\"margin-top:20px;\">");
        chal_card(&sb, c, 0);
        sb_append(&sb, "</div>");
    }
    count("weapon", NULL, 0, &done, &total);
    header(&sb, NULL, "hed_2", 30, "Weapon Challenges", done, total, 0, done >= total, NULL);
    sb_append(&sb, "<div style=\"margin-top:10px;text-align:center;\">");
    for (int i = 0; i < WEAPON_COUNT; i++) {
        const Weapon *w = rules_weapon(i);
        int d, t;
        count("weapon", "wep", i, &d, &t);
        if (!w || w->type == 3 || w->custom_only || !t) continue; // weaponTypes.EQUIPMENT, customOnly
        char path[96], url[256];
        snprintf(path, sizeof(path), "img/chals/%s.png", chall_src(w));
        ui_img_url(path, url, sizeof(url));
        sb_appendf(&sb, "<div class=\"challMapCrd%s\" onmouseenter=\"playTick()\" onclick=\"windows[32].openChallengeCat('weapon',%d);\">"
                        "<img class=\"challRotImg\" style=\"background-image:url(%s);background-size:contain;background-repeat:no-repeat;\">"
                        "<div class=\"challMapCvr\"></div><div class=\"challMapName blackShad\">",
                   d >= t ? " chalCardH" : "", i, url);
        cap_first(&sb, w->name);
        sb_appendf(&sb, "</div><div class=\"challMapPrg\">%d<span style=\"color:#fff;\">/%d</span></div></div>", d, t);
    }
    sb_append(&sb, "</div>");
    // Nightmare Challenges (the cards follow the header)
    count("nightmare", NULL, 0, &done, &total);
    header(&sb, NULL, "hed_4", 30, "Nightmare Challenges", done, total, 0, total && done >= total, NULL);
    cards(&sb, "nightmare", NULL, 0, "Check back later for new nightmare challanges!"); // sic (v10)
    // v10: Ranked Challenges (only progress in ranked matches)
    count("ranked", NULL, 0, &done, &total);
    header(&sb, "challRankedHeadr", "hed_3", 30, I18N("windows.chall.ranked", "Ranked Challenges"), done, total, 0, total && done >= total, NULL);
    const cJSON *rl[LIST_MAX];
    const int rn = sorted(rl, "ranked", NULL, 0);
    sb_append(&sb, "<div style=\"margin-top:10px;\">");
    for (int i = 0; i < rn; i++) chal_card(&sb, rl[i], i ? 20 : 0);
    sb_appendf(&sb, "<div class=\"challCrdMsg\" style=\"margin-top:%dpx;\">%s</div></div>", rn ? 20 : 0,
               rn ? I18N("windows.chall.rankedNote", "Ranked challenges only progress in ranked matches.")
                  : I18N("windows.chall.rankedEmpty", "Check back later for new ranked challenges!"));
    return sb_finish(&sb);
}

// windows[32].gen() for a guest: _71155(null, "to complete challenges!", true)
static char *gen_challenges_guest(void) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"reqW login\" onclick=\"showWindow(5)\"><i class=\"material-icons reqWIcon\">clear</i>"
                    "<div class=\"reqWLvl\">LOGIN REQUIRED</div><div class=\"reqWTxt\">%s</div></div>",
               I18N("windows.chall.login", "to complete challenges!"));
    return sb_finish(&sb);
}

// The v10 window bodies this file writes (NULL: not ours)
char *v10_rewards_window_gen(const int id) {
    if (id != CHAL_WINDOW) return NULL;
    const AccountInfo *acc = active_account();
    return acc ? gen_challenges(acc) : gen_challenges_guest();
}

void v10_rewards_challenges_changed(void) {
    menu_rerender_if_open(CHAL_WINDOW);
}

/* ------------------------------------------------------------------ the sidebar's Battle Pass item */

// _52122: shown for any account (v10:146916, {#if !guest}), first in #menuItemContainer, before
// the Daily Spin item. Until windows[5].data ("bpdl") is there v10 falls back to "Battlepass",
// LVL 1 and a full bar (`maxed || !need ? 100`).
static void bp_item_update(void) {
    HtmlElem box = html_dom_by_id("menuItemContainer");
    if (!box) return;
    const int signed_in = active_account() != NULL;
    if (signed_in) battlepass_ui_request();
    const BattlePass *bp = signed_in ? battlepass_ui_pass() : NULL;
    char key[sizeof g_bp_item_key];
    int64_t xp = 0;
    int complete = 0, level = 1, need = 0;
    if (bp) {
        level = bp_level(bp, battlepass_ui_bpp(), &xp, &complete);
        need = bp_level_need(bp, level);
        snprintf(key, sizeof key, "%d|%d|%lld|%d|%s", bp->id, level, (long long) xp, complete, bp->name);
    } else {
        snprintf(key, sizeof key, "%s", signed_in ? "-" : "");
    }
    HtmlElem item = html_dom_by_id("knBpItem");
    if (!strcmp(key, g_bp_item_key) && (item != 0) == (key[0] != 0)) return;
    snprintf(g_bp_item_key, sizeof g_bp_item_key, "%s", key);
    if (!signed_in) {
        if (item) html_dom_remove(item);
        return;
    }
    const double pct = complete || need <= 0 ? 100.0 : (double) xp / need * 100.0 > 100.0 ? 100.0 : (double) xp / need * 100.0;
    StrBuf sb = {0};
    sb_append(&sb, "<img class=\"bpLogo svelte-159x57l\" src=\"./img/battlepass/logo_0.png\" alt=\"Battlepass\"> <div class=\"bpInfo svelte-159x57l\">"
                   "<div class=\"menuItemTitle svelte-159x57l\" id=\"menuBtnBattlepass\" style=\"color: #fff;\">");
    sb_append_esc(&sb, bp && bp->name[0] ? bp->name : "Battlepass");
    sb_appendf(&sb, "</div> <div class=\"bpMeta svelte-159x57l\" style=\"color: rgba(255,255,255,0.6);\"><span class=\"bpLevel svelte-159x57l\" "
                    "style=\"color: #fff;\">LVL %d</span> ", level);
    if (complete) sb_append(&sb, "<span class=\"bpComplete svelte-159x57l\" style=\"color: #ffe540;\">MAX</span>");
    else sb_appendf(&sb, "<div class=\"bpBar svelte-159x57l\"><div class=\"bpBarFill svelte-159x57l\" style=\"width: %.15g%%\"></div></div>", pct);
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    if (!item) {
        html_dom_insert_html(box, HTML_INSERT_AFTER_BEGIN, "<div id=\"knBpItem\" class=\"menuItem bpItem svelte-159x57l\" "
                                                          "onmouseenter=\"playTick()\" onclick=\"playSelect(0.1);openBattlePass()\"></div>");
        item = html_dom_by_id("knBpItem");
    }
    if (item) html_dom_set_html(item, html);
    free(html);
}

/* ------------------------------------------------------------------ handlers / setup */

int v10_rewards_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!strcmp(name, "showSignupRewards")) {
        v10_show_signup_rewards();
        return 1;
    }
    if (!strcmp(name, "knRegisterRewardsPop")) { // --render-scenario: the "prr" popup (n cosmetics, premium)
        register_rewards_popup(argc > 0 ? (int) args[0].num : 0, argc > 1 && args[1].num != 0.0);
        return 1;
    }
    if (!strcmp(name, "knSignupRegister")) { // REGISTER: clearPops(); loginOrRegister(true)
        ui_loadout_clear_pops();
        static const HtmlArg yes = {.kind = HTML_ARG_BOOL, .num = 1};
        html_ui_dispatch("loginOrRegister", ev, &yes, 1);
        fprintf(stderr, "[rewards] register from the signup rewards\n");
        return 1;
    }
    if (!strcmp(name, "windows.32.openHome")) { // account ? showWindow(33) : loginOrRegister()
        g_chal_typ[0] = 0;
        if (active_account()) menu_show_window(CHAL_WINDOW, 0);
        else html_ui_dispatch("loginOrRegister", ev, NULL, 0);
        return 1;
    }
    if (!strcmp(name, "windows.32.openChallengeCat")) { // (typ, ind)
        snprintf(g_chal_typ, sizeof g_chal_typ, "%s", argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? args[0].str : "");
        g_chal_ind = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0;
        menu_show_window(CHAL_WINDOW, 1); // menuWindow.scrollTop = 0; updateWindow(33)
        return 1;
    }
    if (!strcmp(name, "windows.32.closeChallengeCat")) {
        g_chal_typ[0] = 0;
        menu_show_window(CHAL_WINDOW, 1);
        return 1;
    }
    if (!strcmp(name, "openBattlePass")) { // account ? showWindow(6) : loginOrRegister()
        if (active_account()) menu_show_window(6, 0);
        else html_ui_dispatch("loginOrRegister", ev, NULL, 0);
        return 1;
    }
    return -1;
}

// Page ready: a fresh page has no sidebar item and the Challenges window starts at home. (The
// entry points, #signupRewardsButton -> showSignupRewards() and the Challenges item ->
// windows[32].openHome(), are wired by v10_menu.c; the handlers are here.)
void v10_rewards_dom_init(void) {
    g_bp_item_key[0] = 0;
    g_chal_typ[0] = 0;
}

void v10_rewards_tick(void) {
    if (!html_ui_available()) return;
    bp_item_update();
    v10_menu_tick(); // the menu's idle state (v10_menu.c)
}
