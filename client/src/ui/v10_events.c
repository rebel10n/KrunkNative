// Krunker v10.0.3 live events on the v10 page (client/include/live_events_client.h, doc/v10/events.md): the client's
// config.events and window.eventPopup.
//
// config.events (deob-10.0.3.js:22364, :22553) is bundled in v10 (empty in 10.0.3). KrunkNative's clients read the
// operators' events from kn_brain's GET /data/events (services/brain/community.c: the running ones and the ones that
// reveal early), when the page starts and every few minutes after, and at once when a round names an event the list
// does not have. KRUNKNATIVE_GAME_EVENTS=<file> (a community feed with "gameEvents", or the bare list) stands in for
// the brain (scripted runs, a LAN server); a render scenario's state.gameEvents for both. In an official krunker.io
// game the brain is not asked (its gate): 10.0.3's own list is empty there.
//
// window.eventPopup (deob-10.0.3.js:260286-260320) is defined in 10.0.3 but nothing calls it any more (v7.1.4's
// #eventAd button, src/index.js:13755-13765, left the page): KrunkNative keeps it as v10 has it, the `eventPopup`
// handler (inline onclick, input scripts' `call eventPopup`, render scenarios' windowActions).
#include <live_events_client.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <v10_ui.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../viewer/viewer_store.h"

#define EVENTS_REFRESH_MS (5.0 * 60.0 * 1000.0) // the list again every 5 minutes
#define EVENTS_MISS_MS (30.0 * 1000.0)          // a round's unknown event: again after at most 30 s
#define CHAL_WINDOW 33

static struct {
    LiveEvents list;
    int loaded;        // an answer (or the file) is in
    int inflight;
    int fixed;         // the file or a scenario: never fetched
    int file_checked;
    double fetched_at; // client_live_events_now_ms() of the last request
    double scenario_now; // a scenario's clock (0: the wall clock)
    int want;          // an event id a round named that the list lacks
} E;

double client_live_events_now_ms(void) {
    return E.scenario_now > 0.0 ? E.scenario_now : (double) time(NULL) * 1000.0;
}

const LiveEvents *client_live_events(void) {
    return &E.list;
}

static void event_tile_update(int force);

static void events_set(const cJSON *events, const char *source) {
    live_events_parse(&E.list, cJSON_IsArray(events) ? events : NULL);
    E.loaded = 1;
    char ids[96] = "";
    for (int i = 0; i < E.list.count; i++) {
        const size_t used = strlen(ids);
        snprintf(ids + used, sizeof(ids) - used, "%s%d", i ? "," : "", E.list.events[i].id);
    }
    fprintf(stderr, "[events] %d live event(s) from %s%s%s\n", E.list.count, source, E.list.count ? ": " : "", ids);
    menu_rerender_if_open(CHAL_WINDOW); // the Event Challenges section follows the list
    event_tile_update(1);                // so does the menu's event tile
}

static void on_events(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    E.inflight = 0;
    if (E.fixed || status != 200 || !body) return;
    cJSON *json = cJSON_ParseWithLength(body, length);
    const cJSON *data = cJSON_GetObjectItem(json, "data");
    if (cJSON_IsObject(data)) events_set(cJSON_GetObjectItem(data, "events"), "the brain");
    cJSON_Delete(json);
}

// KRUNKNATIVE_GAME_EVENTS: {"gameEvents": [...]} (the brain's feed file) or [...]
static void events_file(void) {
    E.file_checked = 1;
    const char *path = getenv("KRUNKNATIVE_GAME_EVENTS");
    if (!path || !path[0]) return;
    unsigned char *buffer = NULL;
    size_t length = 0;
    if (read_file(path, &buffer, &length)) {
        fprintf(stderr, "[events] cannot read KRUNKNATIVE_GAME_EVENTS %s\n", path);
        return;
    }
    cJSON *json = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    E.fixed = 1;
    events_set(cJSON_IsArray(json) ? json : cJSON_GetObjectItem(json, "gameEvents"), "KRUNKNATIVE_GAME_EVENTS");
    cJSON_Delete(json);
}

void v10_events_tick(void) {
    client_event_objects_frame(menu_ui()->client); // event_objects.c: GAME_CONFIG can make the game custom after its INIT
    if (!E.file_checked) events_file();
    event_tile_update(0); // the menu's event tile: its countdown every 30 s, the round's event when it changes
    if (E.fixed || E.inflight || backend_available() != 1) return;
    const double now = client_live_events_now_ms();
    const int stale = !E.loaded || now - E.fetched_at >= EVENTS_REFRESH_MS;
    const int missing = E.want && !live_events_find(&E.list, E.want) && now - E.fetched_at >= EVENTS_MISS_MS;
    if (!stale && !missing) return;
    E.want = missing ? 0 : E.want; // one retry for a missing id
    E.fetched_at = now;
    E.inflight = backend_http_get("/data/events", on_events, NULL) != 0;
}

void v10_events_want(const int event_index) {
    if (event_index > 0 && !live_events_find(&E.list, event_index)) E.want = event_index;
}

void v10_events_scenario(const cJSON *events, const double now_ms) {
    if (!events) return;
    E.fixed = 1;
    E.file_checked = 1;
    E.scenario_now = now_ms > 0.0 ? now_ms : 0.0;
    events_set(events, "the scenario");
}

/* ------------------------------------------------------------------ the menu's event tile */

// v10.0.7's event tile (svelte-18gradk, deob-10.0.7.js:146873-146877, its component :148238-148290): a button in the
// menu's right panel (#tlInfHold's second slot: after the guest's earned box, before Live Streams, :146880) while the
// round's active event (UTILS.getActiveEvent(eventIndex), :226479) has not ended. It shows the event art, the "EVENT" tag,
// the name and "Starts in" / "Ends in" with the bundle's d h / h m / m format, refreshed every 30 s (:148244), in the
// event's accent color (event.color || #fbc02d, :148250); a click is playSelect() + eventPopup() (:148230). The art is
// left out when it fails to load (:148270); KrunkNative's engine draws a missing image as nothing, so it always goes in.
#define EVENT_TILE_ID "knEventTile"
#define EVENT_TILE_REFRESH_MS (30.0 * 1000.0)

static struct {
    char key[192];   // what the tile shows now ("" = no tile)
    double drawn_at; // client_live_events_now_ms() of the last check
    int event_index; // the round's eventIndex at that check: a new round's event redraws at once
} T;

// the bundle's duration text: days and hours, else hours and minutes, else minutes (at least 1)
static void tile_duration(char *out, const size_t n, const double ms) {
    const long minutes = ms > 0.0 ? (long) (ms / 60000.0) : 0;
    const long days = minutes / 1440, hours = minutes % 1440 / 60;
    if (days) snprintf(out, n, "%ldd %ldh", days, hours);
    else if (hours) snprintf(out, n, "%ldh %ldm", hours, minutes % 60);
    else snprintf(out, n, "%ldm", minutes > 1 ? minutes : 1);
}

static void event_tile_update(const int force) {
    const double now = client_live_events_now_ms();
    const Client *client = menu_ui()->client;
    const int event_index = client ? client->game.event_index : 0;
    if (!force && event_index == T.event_index && T.drawn_at > 0.0 && now - T.drawn_at < EVENT_TILE_REFRESH_MS) return;
    T.drawn_at = now;
    T.event_index = event_index;
    const HtmlElem panel = html_dom_by_id("tlInfHold");
    if (!panel) return;
    const LiveEvent *e = live_events_active(&E.list, event_index, now);
    // open-ended events (KrunkNative's feed, end 0) count as running; v10's always end
    const int on = e && (e->end_ms <= 0.0 || e->end_ms > now);
    char when[48] = "", key[sizeof T.key] = "";
    if (on) {
        char span[24];
        const int upcoming = e->start_ms > now;
        tile_duration(span, sizeof(span), upcoming ? e->start_ms - now : e->end_ms - now);
        if (upcoming) I18N_FMT(when, "menu.event.starts", span);
        else if (e->end_ms > 0.0) I18N_FMT(when, "menu.event.ends", span);
        snprintf(key, sizeof(key), "%d|%s|%s|%s", e->id, e->color, e->name, when);
    }
    HtmlElem tile = html_dom_by_id(EVENT_TILE_ID);
    if (!strcmp(key, T.key) && (tile != 0) == on) return;
    snprintf(T.key, sizeof(T.key), "%s", key);
    if (tile) html_dom_remove(tile);
    if (!on) return;
    char path[96], url[256];
    live_event_image(e, e->id, path, sizeof(path));
    ui_img_url(path, url, sizeof(url));
    StrBuf sb = {0};
    sb_appendf(&sb, "<button type=\"button\" class=\"event-tile svelte-18gradk\" id=\"" EVENT_TILE_ID "\" style=\"--event-accent: %s\" "
                    "onclick=\"playSelect();eventPopup()\"><div class=\"event-tile__art svelte-18gradk\">"
                    "<img alt=\"\" draggable=\"false\" class=\"svelte-18gradk\" src=\"",
               e->color[0] ? e->color : "#fbc02d");
    sb_append_esc(&sb, url);
    sb_append(&sb, "\"></div> <div class=\"event-tile__info svelte-18gradk\"><span class=\"event-tile__tag svelte-18gradk\">");
    sb_append_esc(&sb, I18N("menu.event.tag", "EVENT"));
    sb_append(&sb, "</span> <span class=\"event-tile__name svelte-18gradk\">");
    sb_append_esc(&sb, e->name);
    sb_append(&sb, "</span> <span class=\"event-tile__time svelte-18gradk\">");
    sb_append_esc(&sb, when);
    sb_append(&sb, "</span></div></button>");
    char *html = sb_finish(&sb);
    // the slot: after the guest's earned box when it is there, else after the ad row; Live Streams stays below
    const HtmlElem earned = html_dom_by_id("knGuestEarned"), ads = html_dom_by_id("topLeftAdHolder");
    const HtmlElem before = earned ? earned : ads;
    html_dom_insert_html(before ? before : panel, before ? HTML_INSERT_AFTER_END : HTML_INSERT_AFTER_BEGIN, html);
    free(html);
}

/* ------------------------------------------------------------------ window.eventPopup */

// acc.skins has the index with cnt > 0 (v10.0.7's owned check on the reward tiles, deob-10.0.7.js:228462-228466)
static int reward_owned(const int skin) {
    const AccountInfo *acc = active_account();
    const cJSON *skins = acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL;
    const cJSON *row;
    cJSON_ArrayForEach(row, skins) {
        const cJSON *ind = cJSON_GetObjectItem(row, "ind"), *cnt = cJSON_GetObjectItem(row, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == skin) return !cJSON_IsNumber(cnt) || cnt->valueint > 0;
    }
    return 0;
}

// the reward tiles' sprite-sheet animation (UTILS.sprayAnimation, deob-10.0.3.js:27896-27902)
static void spray_animation(StrBuf *sb, const ViewerSkin *skin, const char *name) {
    if (skin->frames) sb_appendf(sb, "animation: %s%d %.15gs steps(%d) infinite;", name, skin->frames, 1.0 + skin->frame_t / 1000.0, skin->frames);
}

// longDesc as text (KrunkNative's feed carries no markup): a list becomes the <li> items main.css styles
static void desc_html(StrBuf *sb, const LiveEvent *e) {
    const char *s = e->long_desc;
    while (*s) {
        const char *nl = strchr(s, '\n');
        const size_t n = nl ? (size_t) (nl - s) : strlen(s);
        char line[LIVE_EVENT_DESC_SIZE];
        snprintf(line, sizeof(line), "%.*s", (int) n, s);
        if (e->long_desc_list) sb_append(sb, "<li>");
        sb_append_esc(sb, line);
        if (e->long_desc_list) sb_append(sb, "</li>");
        else if (nl) sb_append(sb, "<br>");
        s += n + (nl ? 1 : 0);
    }
}

// eventPopup's tiles (deob-10.0.7.js:228456-228472): `count` of them, a grey tile for an index the store does not have
// (or past the list); a spray's frame count joins `frames` for the popup's keyframes
static void reward_tiles(StrBuf *sb, const int *list, const int list_n, const int count, int *frames, int *frame_count) {
    for (int i = 0; i < count; i++) {
        const int index = i < list_n ? list[i] : -1;
        const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
        if (!skin) {
            sb_append(sb, "<div class='bundlItmC' style='border: 3px solid #707070;'></div>");
            continue;
        }
        const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
        const int spray = skin->type == 4 && skin->frames;
        if (spray) {
            int dup = 0;
            for (int k = 0; k < *frame_count; k++) dup |= frames[k] == skin->frames;
            if (!dup && *frame_count < 2 * LIVE_EVENT_SKINS_MAX) frames[(*frame_count)++] = skin->frames;
        }
        char thumb[256];
        store_skin_preview(index, thumb, sizeof(thumb));
        sb_appendf(sb, "<div class='bundlItmC' style='background:url(%s);", thumb);
        if (spray) {
            sb_append(sb, "background-size: cover !important;");
            spray_animation(sb, skin, "sprayAnmB");
        } else {
            sb_append(sb, "background-size: contain;background-position: center;");
        }
        // v10.0.7: the tile is the owned mark's containing block, and an owned reward gets the check
        sb_appendf(sb, "border: 3px solid %s;position:relative;' onclick='viewItem(%d)'>%s</div>", g_viewer_rarities[r].color, index,
                   reward_owned(index) ? "<i class='material-icons eventPopOwned'>check_circle</i>" : "");
    }
}

static void event_popup(void) {
    const Client *client = menu_ui()->client;
    const int event_index = client ? client->game.event_index : 0;
    const double now = client_live_events_now_ms();
    const LiveEvent *e = live_events_active(&E.list, event_index, now); // UTILS.getActiveEvent(Game.eventIndex)
    if (!e) {
        fprintf(stderr, "[events] eventPopup: no event (round event %d, %d listed)\n", event_index, E.list.count);
        return;
    }
    const double starts_in = e->start_ms - now, ends_in = e->end_ms - now;
    char path[96], url[256], when[64];
    live_event_image(e, e->id, path, sizeof(path));
    ui_img_url(path, url, sizeof(url));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='eventPopHeader' style='background-image:url(%s);'><div class='eventPopName'>", url);
    sb_append_esc(&sb, e->name);
    sb_append(&sb, "</div></div>");
    // rewards.skins: the first 12 ("playing the event" for noChallenges || rewardChance); rewards.challengeSkins: every one
    // (deob-10.0.7.js:228474-228475)
    StrBuf skins = {0};
    int frames[2 * LIVE_EVENT_SKINS_MAX], frame_count = 0;
    if (e->has_reward_skins) {
        sb_appendf(&skins, "<div class='eventPopSkinsH'>Earn these skins from %s</div>",
                   e->no_challenges || e->has_reward_chance ? "playing the event" : "completing challenges");
        reward_tiles(&skins, e->reward_skins, e->reward_skin_count, LIVE_EVENT_POPUP_SKINS, frames, &frame_count);
    }
    if (e->has_challenge_skins) {
        sb_append(&skins, "<div class='eventPopSkinsH'>Complete every challenge for</div>");
        reward_tiles(&skins, e->challenge_skins, e->challenge_skin_count, e->challenge_skin_count, frames, &frame_count);
    }
    if (frame_count) {
        sb_append(&sb, "<style>");
        for (int k = 0; k < frame_count; k++) sb_appendf(&sb, "@keyframes sprayAnmB%d {100%%{background-position:-%dpx}}", frames[k], frames[k] * 81);
        sb_append(&sb, "</style>");
    }
    sb_append(&sb, "<div class='eventPopInfoH'><div class='eventPopInfo'>");
    desc_html(&sb, e);
    sb_append(&sb, "</div>");
    char *skins_html = sb_finish(&skins);
    sb_append(&sb, skins_html);
    free(skins_html);
    // KrunkNative: an open-ended event (no endsAt) has no "Ends in" line (v10's events always end)
    if (starts_in > 0.0 || e->end_ms > 0.0) {
        account_readable_time(when, sizeof(when), starts_in > 0.0 ? starts_in : ends_in);
        sb_appendf(&sb, "<div class='eventPopEnd'>%s in %s</div>", starts_in > 0.0 ? "Starts" : "Ends", when);
    }
    sb_append(&sb, "</div>");
    if (!e->no_challenges)
        sb_append(&sb, "<div class='eventPopChallBtn' onclick='playSelect();logBtnFRVR(`challenges`);clearPops();windows[32].openHome()'>View Challenges</div>");
    sb_append(&sb, "</div>"); // v10's markup closes one div more than it opens (the parser drops it)
    char *html = sb_finish(&sb);
    v10_popup_show("eventPop", html, NULL); // genericPopup(html, "eventPop")
    free(html);
    fprintf(stderr, "[events] eventPopup: event %d \"%s\" (%s, %d reward skin(s))\n", e->id, e->name, starts_in > 0.0 ? "upcoming" : "running",
            e->reward_skin_count);
}

int v10_events_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    (void) args;
    (void) argc;
    if (!strcmp(name, "eventPopup")) {
        event_popup();
        return 1;
    }
    return -1;
}
