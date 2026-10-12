// WP-E client: lobby packets (block 180-189) and the end screen's match vote cards
// (client/include/lobby_ui.h). Krunker v7.1.4 index.js: f1915 builds #matchVoteHolder from
// endData.mts, updMatchVote ("mv") writes each card's total and colours our pick, voteOnMatch
// sends "maVote".
#include <lobby_ui.h>
#include <game_config.h>
#include <map_catalog.h>
#include <modes.h>
#include <html_ui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOTE_SELECTED "rgba(251, 192, 45, 1)"      // COLORS.matchVote.selected
#define VOTE_NOT_SELECTED "rgba(255, 255, 255, 0.7)" // COLORS.matchVote.notSelected

static NetMatchVoteStatePacket g_votes; // count 0: no vote
static int g_mine = -1;
static int g_shown;                     // the cards are in #matchVoteHolder

void lobby_vote_reset(void) {
    memset(&g_votes, 0, sizeof(g_votes));
    g_mine = -1;
    g_shown = 0;
}

const NetMatchVoteStatePacket *lobby_vote_state(void) {
    return &g_votes;
}

int lobby_vote_mine(void) {
    return g_mine;
}

static int same_picks(const NetMatchVoteStatePacket *a, const NetMatchVoteStatePacket *b) {
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count && i < NET_MATCH_PICKS; i++) {
        if (a->entries[i].mode_id != b->entries[i].mode_id || a->entries[i].map_index != b->entries[i].map_index) return 0;
    }
    return 1;
}

// HTML-escapes a map name into `out`.
static void escape(char *out, const size_t size, const char *in) {
    size_t n = 0;
    for (const char *c = in; c && *c && n + 6 < size; c++) {
        const char *rep = *c == '<' ? "&lt;" : *c == '>' ? "&gt;" : *c == '&' ? "&amp;" : *c == '"' ? "&quot;" : *c == '\'' ? "&#39;" : NULL;
        if (rep) {
            const size_t len = strlen(rep);
            memcpy(out + n, rep, len);
            n += len;
        } else {
            out[n++] = *c;
        }
    }
    out[n] = 0;
}

char *lobby_vote_cards_html(void) {
    if (g_votes.count < 2) return NULL; // v7 shows the cards only for more than one pick
    const size_t size = 2048 * NET_MATCH_PICKS;
    char *html = malloc(size);
    if (!html) return NULL;
    size_t n = 0;
    html[0] = 0;
    for (int i = 0; i < g_votes.count && i < NET_MATCH_PICKS; i++) {
        const NetMatchVoteEntry *e = &g_votes.entries[i];
        const GameModeConfig *mode = mode_config(e->mode_id);
        if (!mode || !mode->name) continue;
        char name[sizeof(e->map_name)], esc[sizeof(e->map_name) * 6], thumb[96] = "img/noimg.png", upper[64];
        memcpy(name, e->map_name, sizeof(name));
        name[sizeof(name) - 1] = 0;
        escape(esc, sizeof(esc), name);
        const int found = name[0] ? map_catalog_find(name) : -1;
        const MapCatalogEntry *entry = found >= 0 ? map_catalog_at((size_t) found) : NULL;
        char path[96];
        if (entry && map_catalog_thumb(entry, path, sizeof(path))) snprintf(thumb, sizeof(thumb), "%s", path);
        snprintf(upper, sizeof(upper), "%s", mode->name);
        for (char *c = upper; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char) (*c - 'a' + 'A');
        const int w = snprintf(html + n, size - n,
            "<label class='matchVote' onclick='voteOnMatch(\"%d,%d\")'><img src='%s' class='matchVoteThumb' draggable='false'>"
            "<div class='matchVoteMap'>%s</div><div class='matchVoteMode'>%s</div>"
            "<div class='matchVoteTotal' id='voteT%d,%d'>%u</div></label>",
            e->mode_id, e->map_index, thumb, esc, upper, e->mode_id, e->map_index, (unsigned int) e->votes);
        if (w < 0 || (size_t) w >= size - n) break;
        n += (size_t) w;
    }
    return html;
}

// updMatchVote: every card's total, our pick in the selected colour.
static void update_totals(void) {
    if (!html_ui_available()) return;
    for (int i = 0; i < g_votes.count && i < NET_MATCH_PICKS; i++) {
        const NetMatchVoteEntry *e = &g_votes.entries[i];
        char id[48], text[16];
        snprintf(id, sizeof(id), "voteT%d,%d", e->mode_id, e->map_index);
        const HtmlElem el = html_dom_by_id(id);
        if (!el) continue;
        snprintf(text, sizeof(text), "%u", (unsigned int) e->votes);
        html_dom_set_text(el, text);
        html_dom_set_style(el, "color", i == g_mine ? VOTE_SELECTED : VOTE_NOT_SELECTED);
    }
}

// f1915: the cards into #matchVoteHolder (the end screen's results call it, and every new list).
int lobby_vote_show(void) {
    if (!html_ui_available()) return 0;
    const HtmlElem holder = html_dom_by_id("matchVoteHolder");
    if (!holder) return 0;
    char *html = lobby_vote_cards_html();
    if (!html) {
        html_dom_set_display(holder, "none");
        g_shown = 0;
        return 0;
    }
    html_dom_set_html(holder, html);
    free(html);
    html_dom_set_display(holder, "inline-block");
    const HtmlElem mid = html_dom_by_id("endMidHolder");
    if (mid) html_dom_set_style(mid, "max-height", "calc(100% - 490px)");
    g_shown = 1;
    update_totals();
    return 1;
}

static void on_vote_state(const NetMatchVoteStatePacket *packet) {
    const int new_list = !same_picks(&g_votes, packet);
    g_votes = *packet;
    if (g_votes.count > NET_MATCH_PICKS) g_votes.count = NET_MATCH_PICKS;
    if (packet->voted >= -1) g_mine = packet->voted; // -2: someone else voted, ours is unchanged
    else if (new_list) g_mine = -1;
    if (new_list && g_shown) lobby_vote_show(); // a new list while the results are up
    else if (g_shown) update_totals();
}

int lobby_vote_send(Client *client, const int mode_id, const int map_index) {
    if (!client || (!client->offline && client->net_server_minor < NET_PROTOCOL_MINOR_V7)) return 0;
    const NetMatchVotePacket vote = {.mode_id = mode_id, .map_index = map_index};
    client_net_send(client, NET_PACKET_MATCH_VOTE, &vote, sizeof(vote));
    return 1;
}

int lobby_vote_handler(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    Client *client = user;
    if (argc < 1 || !args || args[0].kind != HTML_ARG_STR || !args[0].str) return 1;
    int mode_id = -1, map_index = -1;
    if (sscanf(args[0].str, "%d,%d", &mode_id, &map_index) != 2) return 1;
    lobby_vote_send(client, mode_id, map_index);
    return 1;
}

int lobby_events_handle(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_GAME_CONFIG:
            game_config_receive(&client->game, payload, length);
            return 1;
        case NET_PACKET_NETWORK_DATA: {
            NetNetworkDataPacket packet;
            if (length != sizeof(packet)) return 1;
            memcpy(&packet, payload, sizeof(packet));
            game_network_receive(&client->game, &packet);
            return 1;
        }
        case NET_PACKET_MATCH_VOTE_STATE: {
            NetMatchVoteStatePacket packet;
            if (length != sizeof(packet)) return 1;
            memcpy(&packet, payload, sizeof(packet));
            on_vote_state(&packet);
            return 1;
        }
        default:
            return 0;
    }
}
