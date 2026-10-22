// Krunker Strike's reward flow (strike_ui.h): FRVR 1.37.2's Mithril views, transcribed from
// assets/index-Cz2uYXJQ.js (read as text; no sandbox capture of them exists, so they are not
// pixel-verified). Templates: ui/strike/screens/REWARD.html, REWARD_CHOICE.html, INPUT_BLOCKER.html,
// STORE_CONFIRM_PURCHASE.html.
//   VC (Es)  REWARD          one reward after another (rewardsLeft / nextItemOrPop): the rarity's lights
//                            and colour (computeLightsColor), the stars of an epic or better item (oO),
//                            the item (r1 in its 2D form, build2DReward: the thumbnail), the title
//                            (buildTitle) and the buttons (buildButtons: Equip now! / Okay, Select now!,
//                            Use Now!, Okay); Pt by aue.showRewardScreen (a spin), Vw (a purchase) and UU (a
//                            Rewards Track claim), its onPop the caller's StrikeRewardDone
//   NB (QC)  REWARD_CHOICE   a pick-one level's options; Submit = onChoice(selected) then kt()
//   ws       INPUT_BLOCKER   "Purchasing..." while $r waits for the shop
//   U2       the product popup in the POPUP frame (sU's market item: Pt(In, {builder: U2}))
//   battlepass  a reward of FRVR's gi.BATTLEPASS: the ticket image (reward-image--bp), no title, and Awesome!
//            (kr(_C)): this reward goes and BATTLEPASS_SCREEN opens. No brain answer carries one yet; the
//            developer console's `strike_reward battlepass` shows a sample (strike_reward_sample)
// Differences (documented): r1 is the 2D viewer (the "reward_viewer" feature's 3D mode loads FRVR's item
// meshes; KrunkNative shows the item's store thumbnail, as build2DReward does); a class reward's character
// viewer (yc) is not drawn (the native class preview shows the selected class only); a Dye shows its
// thumbnail (FRVR: a square of the dye's shirt colour, which the catalogue does not carry); KrunkNative's
// catalogue has rarities past FRVR's legendary (Krunker's relic, contraband, unobtainable): those take the
// legendary lights and power; the golden spin that FRVR offers after a heroic spin (cwe) is not opened.
#include <strike_ui.h>
#include "klog.h"
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h>   // store_skin_preview
#include <strike_econ.h>  // the equip of Equip now!
#include <spin_wheel.h>   // SpinStarsDraw
#include <audio.h>        // SOUND.play
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHOICE_MAX 8
#define UNCOMMON 0 // Fn
#define RARE 1
#define EPIC 2
#define LEGENDARY 3
#define STARS_COUNT 50 // Kre

static struct {
    Client *client;
    // VC: rewardsLeft (owned), the origin, heroicSpinsTriggered, this.equipped
    cJSON *rewards;
    char origin[32];
    int heroic, equipped;
    StrikeRewardDone done;
    void *done_user;
    // NB: the options (item ids), this.selected, this.loading
    char options[CHOICE_MAX][32];
    int option_count, selected, loading;
    StrikeChoiceFn on_choice;
    void *choice_user;
    char blocker[64];
    // oO: the star list of the shown REWARD (regenerated on a new reward or a resize)
    SpinStarsDraw stars;
    int stars_rarity;
    float stars_w, stars_h;
} R = {.selected = -1};

// ---------------------------------------------------------------- the reward objects

static const char *jstr(const cJSON *o, const char *k) { return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k)); }

static const cJSON *current(void) { return cJSON_GetArrayItem(R.rewards, 0); }

// the item id of a reward: item {itemId | _id} (an answer's), item "<id>" (the catalogue's), itemDefId
static const char *reward_item_id(const cJSON *r) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(r, "item");
    if (cJSON_IsString(item)) return item->valuestring;
    if (cJSON_IsObject(item)) return jstr(item, "itemId") ? jstr(item, "itemId") : jstr(item, "_id");
    return jstr(r, "itemDefId");
}

static const StrikeItem *reward_item(const cJSON *r) {
    const char *id = reward_item_id(r);
    return id ? strike_item(id) : NULL;
}

static int is_type(const cJSON *r, const char *type) {
    const char *t = jstr(r, "type");
    return t && !strcmp(t, type);
}

static int is_currency(const cJSON *r, const char *currency) {
    const char *c = jstr(r, "currency");
    return is_type(r, "currency") && c && !strcmp(c, currency);
}

// the item's store thumbnail (itemDef.thumbnail: the CDN preview, image-rendering pixelated)
static void item_thumbnail(const StrikeItem *it, char *out, const size_t n) {
    out[0] = 0;
    if (!it) return;
    if (it->skin >= 0) store_skin_preview(it->skin, out, n);
    else if (it->type == STRIKE_ITEM_CLASS && it->class_id >= 0) snprintf(out, n, "./cdn/textures/classes/icon_%d.png", it->class_id);
}

// FRVR's rarities stop at legendary (Fn); KrunkNative's catalogue (Krunker's store) goes on
static int frvr_rarity(const int rarity) { return rarity < 0 ? UNCOMMON : rarity > LEGENDARY ? LEGENDARY : rarity; }

// Oo(itemDef): the item power of a weapon / waist / back / chest / hat with a rarity: +(yI - 1) % damage for
// a weapon skin, +bI % health for the rest. 0 = none.
static int item_power(const StrikeItem *it, char *bonus, const size_t n, const char **img) {
    if (!it || it->rarity < 0) return 0;
    const int t = it->type;
    if (t != STRIKE_ITEM_WEAPON && t != STRIKE_ITEM_WAIST && t != STRIKE_ITEM_BACK && t != STRIKE_ITEM_CHEST && t != STRIKE_ITEM_HAT) return 0;
    static const double YI[4] = {1.05, 1.1, 1.15, 1.2}, BI[4] = {0.013, 0.025, 0.038, 0.05};
    const int r = frvr_rarity(it->rarity);
    const double v = t == STRIKE_ITEM_WEAPON ? (YI[r] - 1.0) * 100.0 : BI[r] * 100.0;
    snprintf(bonus, n, "%.1f", v);
    *img = t == STRIKE_ITEM_WEAPON ? "./img/item-power/damage.svg" : "./img/item-power/health.svg"; // SO(dps / health)
    return 1;
}

int strike_item_power(const char *item_id, char *bonus, const size_t n, const char **img) {
    return item_id && item_power(strike_item(item_id), bonus, n, img);
}

// OU(itemDef): the item beats the one equipped in its slot (the green arrow)
static int beats_equipped(const StrikeItem *it) {
    char slot[16], a[16], b[16];
    const char *img;
    if (!it || !strike_item_slot(it, slot, sizeof(slot)) || !item_power(it, a, sizeof(a), &img)) return 0;
    const char *eq = strike_econ_equipped(slot);
    const StrikeItem *cur = eq ? strike_item(eq) : NULL;
    if (!cur || !item_power(cur, b, sizeof(b), &img)) return 1; // hue(): nothing equipped
    return atof(b) < atof(a);
}

// N.getItemDesc: the type's market label (Ay: social.market.*), a weapon skin its weapon's name
static const char *item_desc(const StrikeItem *it) {
    static const char *const AY[STRIKE_ITEM_TYPE_COUNT] = {
        NULL, "Head Item", "Back Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item",
        "Pet Item", "Collectible Item", "Wrist Item", "Charm Item", "Ticket Item", "Chest Item", NULL,
    };
    if (!it) return "";
    if (it->type > 0 && it->type < STRIKE_ITEM_TYPE_COUNT && AY[it->type]) return AY[it->type];
    const StrikeEconomy *e = strike_economy();
    return it->weapon >= 0 && it->weapon < e->weapon_count ? e->weapon_names[it->weapon] : "";
}

static const char *rarity_name(const int rarity) {
    const StrikeRarity *r = strike_rarity(rarity);
    return r && r->name ? r->name : "";
}

static char *part(const char *file, const char *name, const StrikeVar *vars, const int count) {
    char *s = strike_template_part(file, name, vars, count);
    return s ? s : calloc(1, 1);
}

static void sb_part(StrBuf *sb, const char *file, const char *name, const StrikeVar *vars, const int count) {
    char *s = part(file, name, vars, count);
    sb_append(sb, s);
    free(s);
}

// ---------------------------------------------------------------- VC: REWARD

// computeLightsColor(isItem, rarity)
static const char *lights(const cJSON *r) {
    if (!is_type(r, "item") && !is_type(r, "class")) return "non-item";
    const StrikeItem *it = reward_item(r);
    static const char *const L[4] = {"uncommon-item", "rare-item", "epic-item", "legendary-item"};
    return L[frvr_rarity(it ? it->rarity : UNCOMMON)];
}

// buildReward
static void build_reward(StrBuf *sb, const cJSON *r) {
    char image[512];
    if (is_type(r, "item")) { // r1 {winItem, fullscreen, showStars}: oO, then the 2D viewer
        const StrikeItem *it = reward_item(r);
        item_thumbnail(it, image, sizeof(image));
        sb_part(sb, "REWARD", "stars", NULL, 0);
        const StrikeVar v[] = {{"image", image}, {"rendering", "pixelated"}};
        sb_part(sb, "REWARD", "viewer", v, 2);
    } else if (is_type(r, "class")) { // .reward-class: oO {itemRarity} and the class's character (yc)
        sb_part(sb, "REWARD", "class", NULL, 0);
    } else if (is_type(r, "currency")) {
        const char *cur = jstr(r, "currency");
        const int amount = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "amount"));
        const char *cls = NULL;
        if (cur && !strcmp(cur, "spin_ticket")) {
            if (amount >= 1 && amount <= 3) snprintf(image, sizeof(image), "./img/frvr-ui/spin-ticket-%d.png", amount);
            else snprintf(image, sizeof(image), "./img/frvr-ui/spin-ticket-many.png");
            cls = "reward-image reward-image--ticket";
        } else if (cur && !strcmp(cur, "xp")) {
            snprintf(image, sizeof(image), "./img/frvr-ui/xp-symbol.svg");
            cls = "reward-image reward-image--xp";
        } else if (cur && !strcmp(cur, "kr")) {
            snprintf(image, sizeof(image), "./img/frvr-ui/krunkies.svg");
            cls = "reward-image reward-image--kr reward-float-animation";
        }
        if (cls) {
            const StrikeVar v[] = {{"image", image}, {"class", cls}};
            sb_part(sb, "REWARD", "image", v, 2);
        }
    } else if (is_type(r, "feature")) {
        const char *f = jstr(r, "feature");
        if (f && !strcmp(f, "missions")) {
            const StrikeVar v[] = {{"image", "./img/frvr-ui/missions/missions-icon.svg"}, {"class", "reward-image"}};
            sb_part(sb, "REWARD", "image", v, 2);
        }
    } else if (is_type(r, "battlepass")) { // the pass's ticket (FRVR: img.reward-image.reward-image--bp)
        const StrikeVar v[] = {{"image", "./img/battlepass/ticket.svg"}, {"class", "reward-image reward-image--bp"}};
        sb_part(sb, "REWARD", "image", v, 2);
    }
}

// buildTitle
static void build_title(StrBuf *sb, const cJSON *r) {
    char title[128] = "", sub[256] = "";
    const int amount = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "amount"));
    if (is_type(r, "currency")) {
        const char *cur = jstr(r, "currency");
        const int level_up = !strcmp(R.origin, "levelUp");
        if (cur && !strcmp(cur, "spin_ticket")) {
            const char *a = level_up ? "Level up!" : "Spin Ticket";
            if (amount > 1) snprintf(title, sizeof(title), "%dx %s", amount, a);
            else snprintf(title, sizeof(title), "%s", a);
            snprintf(sub, sizeof(sub), "%s", level_up ? "You received a Spin Ticket. Use it to spin the wheel!"
                                                       : "You can use tickets instead of KRs to spin the wheel.");
        } else if (cur && !strcmp(cur, "xp")) {
            snprintf(title, sizeof(title), "%d XP", amount);
            snprintf(sub, sizeof(sub), "A boost to help you level up!");
        } else if (cur && !strcmp(cur, "kr")) {
            snprintf(title, sizeof(title), "%d KR", amount);
            snprintf(sub, sizeof(sub), "Make it rain!");
        } else if (cur && !strcmp(cur, "seasonal_event_tokens")) {
            snprintf(title, sizeof(title), "%d Seasonal Tokens", amount);
        }
    } else if (is_type(r, "item")) { // name; [rarity, Season N, getItemDesc].join(" - ")
        const StrikeItem *it = reward_item(r);
        if (it) {
            snprintf(title, sizeof(title), "%s", it->name ? it->name : "");
            if (it->rarity >= 0) snprintf(sub, sizeof(sub), "%s - %s", rarity_name(it->rarity), item_desc(it));
            else snprintf(sub, sizeof(sub), "%s", item_desc(it));
        }
    } else if (is_type(r, "class")) { // name; [rarity, "Class"]
        const StrikeItem *it = reward_item(r);
        if (it) {
            snprintf(title, sizeof(title), "%s", it->name ? it->name : "");
            if (it->rarity >= 0) snprintf(sub, sizeof(sub), "%s - Class", rarity_name(it->rarity));
            else snprintf(sub, sizeof(sub), "Class");
        }
    } else if (is_type(r, "feature")) {
        const char *f = jstr(r, "feature");
        if (f && !strcmp(f, "missions")) {
            snprintf(title, sizeof(title), "Daily Missions");
            snprintf(sub, sizeof(sub), "New Feature Unlocked!");
        }
    }
    const StrikeVar v[] = {{"title", title}, {"subtitle", sub}};
    sb_part(sb, "REWARD", "title", v, 2);
}

// buildButtons
static void build_buttons(StrBuf *sb, const cJSON *r) {
    if (is_type(r, "item")) {
        const StrikeItem *it = reward_item(r);
        // o: the last reward of a spin that triggered golden tickets shows the golden banner first
        char *banner = NULL;
        if (cJSON_GetArraySize(R.rewards) <= 1 && R.heroic > 0) {
            char text[96];
            if (R.heroic > 1) snprintf(text, sizeof(text), "You found %d golden spin tickets!", R.heroic);
            else snprintf(text, sizeof(text), "You found a golden spin ticket!");
            const StrikeVar v[] = {{"text", text}};
            banner = part("REWARD", "banner", v, 1);
        }
        char bonus[16];
        const char *img = NULL;
        char *power = NULL;
        if (item_power(it, bonus, sizeof(bonus), &img)) {
            const char *arrow = beats_equipped(it) ? "<img src=\"./img/green-arrow-up.svg\" class=\"arrow\">" : "";
            const StrikeVar v[] = {{"bonus", bonus}, {"img", img}, {"arrow", arrow}};
            power = part("REWARD", "power", v, 3);
        }
        const StrikeVar v[] = {{"banner", banner ? banner : "<div></div>"}, {"power", power ? power : ""},
                               {"disabled", R.equipped ? " is-disabled" : ""}, {"equip", R.equipped ? "Equipped!" : "Equip now!"}};
        sb_part(sb, "REWARD", "buttons-item", v, 4);
        free(banner);
        free(power);
    } else if (is_type(r, "class")) {
        sb_part(sb, "REWARD", "buttons-class", NULL, 0);
    } else if (is_type(r, "battlepass")) { // Awesome! (generic.awesome): kr(_C), the Battle Pass screen
        const StrikeVar v[] = {{"action", "reward-battlepass"}, {"label", "Awesome!"}};
        sb_part(sb, "REWARD", "buttons-one", v, 2);
    } else if (is_currency(r, "spin_ticket")) {
        const StrikeVar v[] = {{"action", "reward-use-ticket"}, {"label", "Use Now!"}};
        sb_part(sb, "REWARD", "buttons-one", v, 2);
    } else {
        const StrikeVar v[] = {{"action", "reward-next"}, {"label", "Okay"}};
        sb_part(sb, "REWARD", "buttons-one", v, 2);
    }
}

static void reward_fill(const HtmlElem root) {
    const cJSON *r = current();
    if (!r) return;
    char src[96];
    const char *l = lights(r);
    const HtmlElem li = html_dom_query(root, ".reward-screen-bg-lights img"), co = html_dom_query(root, ".reward-screen-bg img");
    snprintf(src, sizeof(src), "./img/reward-bg/reward-bg-lights-%s.svg", l);
    if (li) html_dom_set_attr(li, "src", src);
    snprintf(src, sizeof(src), "./img/reward-bg/reward-bg-color-%s.svg", l);
    if (co) html_dom_set_attr(co, "src", src);
    StrBuf sb = {0};
    if (is_currency(r, "spin_ticket")) sb_part(&sb, "REWARD", "controls", NULL, 0); // Ps {onBack, onClose: nextItemOrPop}
    build_reward(&sb, r);
    build_title(&sb, r);
    build_buttons(&sb, r);
    char *html = sb_finish(&sb);
    const HtmlElem screen = html_dom_query(root, ".frvr-reward-screen");
    if (screen) html_dom_set_html(screen, html ? html : "");
    free(html);
    // oO: the stars follow the item's rarity, regenerated for this reward
    const StrikeItem *it = (is_type(r, "item") || is_type(r, "class")) ? reward_item(r) : NULL;
    R.stars_rarity = it ? it->rarity : -1;
    R.stars_w = R.stars_h = 0.0f;
    R.stars.count = 0;
    char *id = it ? (char *) it->id : NULL;
    KN_LOG_VERBOSE2("[strike] REWARD %s %s (%s, %d left)\n", jstr(r, "type") ? jstr(r, "type") : "?", id ? id : jstr(r, "currency") ? jstr(r, "currency") : "",
           l, cJSON_GetArraySize(R.rewards) - 1);
}

// resolveRewardExtras: cheer_1 for a legendary or better item, cheer_0 for a rare one, a spin ticket or tokens
static void reward_sound(const cJSON *r) {
    const StrikeItem *it = (is_type(r, "item") || is_type(r, "class")) ? reward_item(r) : NULL;
    if (it && it->rarity >= LEGENDARY) audio_play("cheer_1", 0.1f, 0, 1.0f);
    else if (it && it->rarity >= RARE) audio_play("cheer_0", 0.1f, 0, 1.0f);
    else if (is_currency(r, "spin_ticket") || is_currency(r, "seasonal_event_tokens")) audio_play("cheer_0", 0.1f, 0, 1.0f);
}

// j2: a feature reward that is not shown (NoAdsUnsolicited: KrunkNative shows no ads) is skipped
static int shown(const cJSON *r) {
    const char *f = jstr(r, "feature");
    return !(is_type(r, "feature") && f && !strcmp(f, "no_ads_unsolicited"));
}

static void finish(const int pop) {
    StrikeRewardDone done = R.done;
    void *user = R.done_user;
    R.done = NULL;
    R.done_user = NULL;
    cJSON_Delete(R.rewards);
    R.rewards = NULL;
    R.stars.visible = 0;
    if (pop && strike_screen_top() == STRIKE_SCREEN_REWARD) strike_screen_pop(); // kt()
    KN_LOG_DEBUG1("[strike] REWARD done\n");
    if (done) done(user); // Pt's onPop
}

// nextItemOrPop
static void next_or_pop(void) {
    while (R.rewards && cJSON_GetArraySize(R.rewards) > 1) {
        cJSON_DeleteItemFromArray(R.rewards, 0);
        R.equipped = 0;
        if (!shown(current())) continue;
        reward_sound(current());
        strike_screen_rerender(STRIKE_SCREEN_REWARD);
        return;
    }
    finish(1);
}

void strike_reward_show(const cJSON *rewards, const char *origin, const int heroic, const StrikeRewardDone done, void *user) {
    if (R.rewards) finish(1); // a flow still up (its caller's done runs first)
    R.rewards = cJSON_IsArray(rewards) ? cJSON_Duplicate(rewards, 1) : cJSON_CreateArray();
    snprintf(R.origin, sizeof(R.origin), "%s", origin ? origin : "");
    R.heroic = heroic;
    R.equipped = 0;
    R.done = done;
    R.done_user = user;
    while (cJSON_GetArraySize(R.rewards) > 0 && !shown(current())) cJSON_DeleteItemFromArray(R.rewards, 0);
    if (cJSON_GetArraySize(R.rewards) == 0) {
        finish(0);
        return;
    }
    reward_sound(current());
    if (strike_screen_shown(STRIKE_SCREEN_REWARD)) strike_screen_rerender(STRIKE_SCREEN_REWARD);
    else strike_screen_push(STRIKE_SCREEN_REWARD);
}

static void on_equipped(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) data;
    const char *msg = jstr(error, "message");
    if (status == 200) {
        R.equipped = 1; // this.equipped
        KN_LOG_VERBOSE2("[strike] reward equipped\n");
        if (strike_screen_root(STRIKE_SCREEN_REWARD)) strike_screen_rerender(STRIKE_SCREEN_REWARD);
    } else {
        printf("[strike] reward equip refused (%d): %s\n", status, msg ? msg : "no answer");
    }
}

// ---------------------------------------------------------------- NB: REWARD_CHOICE

void strike_reward_choice(const char *const *item_ids, const int count, const StrikeChoiceFn on_choice, void *user) {
    R.option_count = 0;
    for (int i = 0; i < count && i < CHOICE_MAX; i++) snprintf(R.options[R.option_count++], sizeof(R.options[0]), "%s", item_ids[i] ? item_ids[i] : "");
    R.selected = -1;
    R.loading = 0;
    R.on_choice = on_choice;
    R.choice_user = user;
    KN_LOG_VERBOSE2("[strike] REWARD_CHOICE %d options\n", R.option_count);
    if (strike_screen_shown(STRIKE_SCREEN_REWARD_CHOICE)) strike_screen_rerender(STRIKE_SCREEN_REWARD_CHOICE);
    else strike_screen_push(STRIKE_SCREEN_REWARD_CHOICE);
}

static void choice_fill(const HtmlElem root) {
    StrBuf sb = {0};
    for (int i = 0; i < R.option_count; i++) {
        const StrikeItem *it = strike_item(R.options[i]);
        char image[512], index[8], bonus[16];
        const char *img = NULL;
        item_thumbnail(it, image, sizeof(image));
        snprintf(index, sizeof(index), "%d", i);
        char *label = NULL;
        if (item_power(it, bonus, sizeof(bonus), &img)) { // due: the item power label
            const StrikeVar lv[] = {{"bonus", bonus}, {"img", img}};
            label = part("REWARD_CHOICE", "label", lv, 2);
        }
        const StrikeVar v[] = {{"selected", R.selected == i ? "reward-choice__item--selected" : ""}, {"index", index},
                               {"image", image}, {"label", label ? label : ""}};
        sb_part(&sb, "REWARD_CHOICE", "item", v, 4);
        free(label);
    }
    char *html = sb_finish(&sb);
    const HtmlElem items = html_dom_query(root, ".reward-choice__items");
    if (items) html_dom_set_html(items, html ? html : "");
    free(html);
    // _t {disabled: selected == -1 || loading}: " is-disabled" and the disabled attribute
    const HtmlElem submit = html_dom_query(root, "#rewardChoiceSubmit");
    const int disabled = R.selected == -1 || R.loading;
    if (submit) {
        html_dom_set_class(submit, "is-disabled", disabled);
        if (disabled) html_dom_set_attr(submit, "disabled", "");
        else html_dom_remove_attr(submit, "disabled");
    }
}

// ---------------------------------------------------------------- ws: INPUT_BLOCKER

void strike_reward_blocker(const char *text) {
    snprintf(R.blocker, sizeof(R.blocker), "%s", text ? text : "");
    if (strike_screen_shown(STRIKE_SCREEN_INPUT_BLOCKER)) strike_screen_rerender(STRIKE_SCREEN_INPUT_BLOCKER);
    else strike_screen_push(STRIKE_SCREEN_INPUT_BLOCKER);
}

void strike_reward_blocker_hide(void) {
    if (strike_screen_top() == STRIKE_SCREEN_INPUT_BLOCKER) strike_screen_pop(); // Tn() == ws && kt()
}

// ---------------------------------------------------------------- U2: the product popup

// Iy: the rarity's colour of an item product (Sv), FRVR's yellow otherwise
static const char *rarity_color(const int rarity) {
    static const char *const IY[4] = {"#32D356", "#00B1FF", "#5736B0", "#FF8E0B"};
    return IY[frvr_rarity(rarity)];
}

char *strike_product_popup_html(const cJSON *product, const char *buy_action) {
    const cJSON *reward = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(product, "products"), 0);
    const cJSON *price = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(product, "price"), 0);
    const char *ptype = jstr(price, "type");
    // sU: a free product is bought at once; the popup is the market's item cards' (FRVR's currency cards
    // pass an undefined itemDef to U2, which cannot render)
    if (!is_type(reward, "item") || (ptype && !strcmp(ptype, "free"))) return NULL;
    const StrikeItem *it = reward_item(reward);
    if (!it) return NULL;
    const int amount = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(price, "amount"));
    char desc[160], full[24], amount_s[24], image[512], bonus[16];
    // Mv: rarity + getItemDesc; a weapon skin + " (<the class whose weapon it is>)" (Fne)
    snprintf(desc, sizeof(desc), "%s %s", it->rarity >= 0 ? rarity_name(it->rarity) : "", item_desc(it));
    if (it->type == STRIKE_ITEM_WEAPON) {
        const char *cls = strike_menu_class_for_weapon(it->weapon);
        if (cls) {
            const size_t n = strlen(desc);
            snprintf(desc + n, sizeof(desc) - n, " (%s)", cls);
        }
    }
    // the default shop design's hasDiscount: nU = ceil(amount * 1 / (1 - 0.25)) struck, "-25%"
    snprintf(full, sizeof(full), "%d", (int) ceil(amount / (1.0 - 0.25)));
    snprintf(amount_s, sizeof(amount_s), "%d", amount);
    item_thumbnail(it, image, sizeof(image));
    const char *img = NULL;
    char *power = NULL;
    if (item_power(it, bonus, sizeof(bonus), &img)) { // Vle(itemDef, showGreenArrow)
        const char *arrow = beats_equipped(it) ? "<img src=\"./img/green-arrow-up.svg\" class=\"arrow\">" : "";
        const StrikeVar pv[] = {{"bonus", bonus}, {"img", img}, {"arrow", arrow}};
        power = part("STORE_CONFIRM_PURCHASE", "power", pv, 3);
    }
    const StrikeVar v[] = {{"color", rarity_color(it->rarity)}, {"name", it->name ? it->name : ""}, {"description", desc},
                           {"full", full}, {"power", power ? power : ""}, {"image", image}, {"buy", buy_action ? buy_action : ""},
                           {"price", amount_s}};
    char *html = strike_template("STORE_CONFIRM_PURCHASE", v, 8);
    free(power);
    KN_LOG_DEBUG1("[strike] product popup %s (%s, %d KR)\n", jstr(product, "_id") ? jstr(product, "_id") : "?", it->id, amount);
    return html;
}

// ---------------------------------------------------------------- oO: the stars

// gI(canvas, Kre, t, n): 50 stars of the B2 sizes, inside the canvas by their outer size
static void stars_generate(const float w, const float h) {
    static const float SIZES[4][2] = {{7, 3.5f}, {11, 5.5f}, {15, 7.5f}, {34, 17}}; // tiny, small, medium, large
    float vw = 0, vh = 0;
    html_dom_viewport(&vw, &vh, NULL);
    const float t = vw > 0 ? w / vw : 1.0f, u = vh > 0 ? h / vh : 1.0f;
    R.stars.count = 0;
    for (int o = 0; o < STARS_COUNT && o < SPIN_STARS_MAX; o++) {
        const float *r = SIZES[rand() % 4];
        const float rx = (float) rand() / ((float) RAND_MAX + 1.0f), ry = (float) rand() / ((float) RAND_MAX + 1.0f);
        R.stars.stars[o].x = fminf(w - r[0], fmaxf(r[0], rx * w)) * t;
        R.stars.stars[o].y = fminf(h - r[0], fmaxf(r[0], ry * h)) * u;
        R.stars.stars[o].timing = (float) o;
        R.stars.stars[o].animation = (float) floor((double) rand() / ((double) RAND_MAX + 1.0) * 5.0 + 1.0);
        R.stars.stars[o].outer = r[0] / 2.0f * t;
        R.stars.stars[o].inner = r[1] / 2.0f * t;
        R.stars.stars[o].opacity = (float) rand() / ((float) RAND_MAX + 1.0f) * 0.3f + 0.1f;
        R.stars.count++;
    }
}

// oO.resizeStars / renderStars: the canvas is its parent's content box; stars only from epic up
static void stars_tick(void) {
    R.stars.visible = 0;
    const HtmlElem root = strike_screen_top() == STRIKE_SCREEN_REWARD ? strike_screen_root(STRIKE_SCREEN_REWARD) : 0;
    const HtmlElem holder = root ? html_dom_query(root, ".reward-screen-bg__star-container") : 0;
    const HtmlElem canvas = holder ? html_dom_query(holder, "canvas") : 0;
    HtmlRect pr;
    if (!canvas || !html_dom_get_rect(holder, &pr) || pr.w <= 0 || pr.h <= 0) return;
    const float w = floorf(pr.w), h = floorf(pr.h);
    if (w != R.stars_w || h != R.stars_h) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%dpx", (int) w);
        html_dom_set_style(canvas, "width", buf);
        snprintf(buf, sizeof(buf), "%dpx", (int) h);
        html_dom_set_style(canvas, "height", buf);
        R.stars_w = w;
        R.stars_h = h;
        R.stars.count = 0;
        if (R.stars_rarity >= EPIC) stars_generate(w, h);
    }
    R.stars.canvas_elem = canvas;
    R.stars.canvas_w = w;
    R.stars.canvas_h = h;
    R.stars.visible = 1; // drawn cleared when there are none (FRVR clears the canvas every frame)
}

const SpinStarsDraw *strike_reward_stars(void) { return &R.stars; }

// ---------------------------------------------------------------- hooks

void strike_reward_render(const StrikeScreen s, const HtmlElem root) {
    if (!root) return;
    switch (s) {
    case STRIKE_SCREEN_REWARD:
        reward_fill(root);
        break;
    case STRIKE_SCREEN_REWARD_CHOICE:
        choice_fill(root);
        break;
    case STRIKE_SCREEN_INPUT_BLOCKER: {
        const HtmlElem t = html_dom_query(root, ".frvr-loader");
        if (t && R.blocker[0]) html_dom_set_text(t, R.blocker);
        break;
    }
    default:
        break;
    }
}

int strike_reward_action(const char *a) {
    if (!strcmp(a, "reward-next")) { // Okay / the close button / the golden banner: nextItemOrPop
        if (R.rewards) next_or_pop();
        return 1;
    }
    if (!strcmp(a, "reward-back")) { // Ps onBack: kt() (its onPop ends the flow)
        if (R.rewards) finish(1);
        return 1;
    }
    if (!strcmp(a, "reward-battlepass")) { // Awesome! = kr(_C): this reward goes (its onPop), the Battle Pass opens
        if (R.rewards) finish(1);
        strike_screen_push(STRIKE_SCREEN_BATTLEPASS);
        return 1;
    }
    if (!strcmp(a, "reward-equip")) { // equipItem(e.item, itemDef) = CO: the item into its loadout slot
        const char *id = reward_item_id(current());
        if (!R.equipped && id && !strike_econ_equip(id, on_equipped, NULL)) printf("[strike] reward equip %s: no Strike account (sign in)\n", id);
        return 1;
    }
    if (!strcmp(a, "reward-select-class")) { // window.selectClass(lC(itemDef.id)), then nextItemOrPop
        const StrikeItem *it = reward_item(current());
        if (it && it->class_id >= 0) menu_set_class_index(it->class_id, 0);
        if (R.rewards) next_or_pop();
        return 1;
    }
    if (!strcmp(a, "reward-use-ticket")) { // Use Now!: kr(SA, {spinnerIndex: luckyDraw}): this screen goes, SPIN comes
        if (R.rewards) finish(1);
        strike_menu_action("spin-ticket");
        return 1;
    }
    if (!strncmp(a, "choice-pick:", 12)) { // this.selected = a
        const int i = atoi(a + 12);
        if (i >= 0 && i < R.option_count && !R.loading) {
            R.selected = i;
            const HtmlElem root = strike_screen_root(STRIKE_SCREEN_REWARD_CHOICE);
            if (root) choice_fill(root);
        }
        return 1;
    }
    if (!strcmp(a, "choice-submit")) { // onChoice(selected).finally(kt)
        if (R.selected < 0 || R.loading || strike_screen_top() != STRIKE_SCREEN_REWARD_CHOICE) return 1;
        R.loading = 1;
        const int choice = R.selected;
        StrikeChoiceFn fn = R.on_choice;
        void *user = R.choice_user;
        R.on_choice = NULL;
        R.choice_user = NULL;
        strike_screen_pop();
        R.loading = 0;
        KN_LOG_VERBOSE2("[strike] REWARD_CHOICE picked %d (%s)\n", choice, R.options[choice]);
        if (fn) fn(user, choice);
        return 1;
    }
    return 0;
}

// the developer console's `strike_reward <kind>`: one sample reward of that kind through the same flow
// (battlepass: the Awesome! opener). 0 = no such kind
int strike_reward_sample(const char *kind) {
    if (!kind || strcmp(kind, "battlepass")) return 0;
    cJSON *rewards = cJSON_CreateArray();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", kind);
    cJSON_AddItemToArray(rewards, r);
    strike_reward_show(rewards, "console", 0, NULL, NULL);
    cJSON_Delete(rewards); // strike_reward_show keeps its own copy
    return 1;
}

void strike_reward_tick(Client *client) {
    R.client = client;
    stars_tick();
    // the flow's screen went (home, the stack replaced): its onPop still runs
    if (R.rewards && !strike_screen_shown(STRIKE_SCREEN_REWARD)) finish(0);
}

// state.strike "rewardScreen": {rewards: [reward objects], origin, heroic, equipped} -> REWARD;
// "rewardChoice": {options: [item ids], selected} -> REWARD_CHOICE (scenarios: sample data)
void strike_reward_scenario(const cJSON *st) {
    const cJSON *rs = cJSON_GetObjectItemCaseSensitive(st, "rewardScreen");
    if (cJSON_IsObject(rs)) {
        strike_reward_show(cJSON_GetObjectItemCaseSensitive(rs, "rewards"), jstr(rs, "origin"),
                           (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(rs, "heroic")), NULL, NULL);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rs, "equipped"))) {
            R.equipped = 1;
            strike_screen_rerender(STRIKE_SCREEN_REWARD);
        }
    }
    const cJSON *rc = cJSON_GetObjectItemCaseSensitive(st, "rewardChoice");
    if (cJSON_IsObject(rc)) {
        const char *ids[CHOICE_MAX];
        int n = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(rc, "options")) if (cJSON_IsString(o) && n < CHOICE_MAX) ids[n++] = o->valuestring;
        strike_reward_choice(ids, n, NULL, NULL);
        const cJSON *sel = cJSON_GetObjectItemCaseSensitive(rc, "selected");
        if (cJSON_IsNumber(sel)) {
            char a[32];
            snprintf(a, sizeof(a), "choice-pick:%d", sel->valueint);
            strike_reward_action(a);
        }
    }
}
