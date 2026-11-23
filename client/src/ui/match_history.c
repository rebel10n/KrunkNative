// v10.0.7's MatchHistory component (svelte-1irqx9a): match_history.h.
#include <match_history.h>
#include <ranked.h> // ranked_rank_icon / ranked_rank_name / ranked_region_code
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *const k_keys[MH_COLS] = {"player_name", "score", "kills", "assists", "deaths", "kd", "objective_score", "damage_done"};
static const char *const k_labels[MH_COLS] = {"Player", "Score", "Kills", "Assists", "Deaths", "K/D", "Objective", "Damage"};

// Number#toString of the values the component prints as they come
static void js_num(char *out, const size_t n, const double v) {
    if (v == floor(v) && fabs(v) < 1e15) snprintf(out, n, "%.0f", v);
    else snprintf(out, n, "%.15g", v);
}

static double num(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : "";
}

void mh_reset(MhState *st) {
    memset(st, 0, sizeof(*st));
    st->page = 1;
}

static int page_size(const MhOpts *o) { return o->page_size > 0 ? o->page_size : 5; }
static int cur_page(const MhState *st) { return st->page > 0 ? st->page : 1; } // a zeroed state is page 1

// the list v10 builds (deob-10.0.7.js:110366-110385): the data prop's page (hasPagination) or all of it, else the
// fetched page
static int list_range(const cJSON *matches, const MhState *st, const MhOpts *o, int *first, int *pages) {
    const int n = cJSON_GetArraySize(matches);
    if (o->with_data && o->has_pagination) {
        const int ps = page_size(o);
        *first = (cur_page(st) - 1) * ps;
        *pages = (n + ps - 1) / ps; // Math.ceil(data.length / pageSize)
        const int end = *first + ps < n ? *first + ps : n;
        return end > *first ? end - *first : 0;
    }
    *first = 0;
    *pages = o->with_data ? 1 : o->total_pages;
    return n;
}

static const cJSON *list_item(const cJSON *matches, const MhState *st, const MhOpts *o, const int index) {
    int first, pages;
    const int n = list_range(matches, st, o, &first, &pages);
    return index >= 0 && index < n ? cJSON_GetArrayItem(matches, first + index) : NULL;
}

// sortBy's comparator (deob-10.0.7.js:110418-110422): numbers by value, strings by localeCompare, anything else
// equal; 'kd' is parseFloat(entry.kd), NaN for v10's entries (they have no kd), so a K/D click keeps the order
static int cmp_values(const cJSON *a, const cJSON *b, const int col, const int asc) {
    if (col == MH_COL_KD) return 0;
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(a, k_keys[col]), *y = cJSON_GetObjectItemCaseSensitive(b, k_keys[col]);
    int c = 0;
    if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) c = x->valuedouble < y->valuedouble ? -1 : x->valuedouble > y->valuedouble ? 1 : 0;
    else if (cJSON_IsString(x) && cJSON_IsString(y)) {
        c = strcasecmp(x->valuestring, y->valuestring); // localeCompare: letters first by their base letter,
        if (!c) c = -strcmp(x->valuestring, y->valuestring); // then lower case before upper case
    }
    return asc ? c : -c;
}

// one sortBy on the match's current order: a stable sort (Array#sort is), so equal entries keep their order
static void sort_order(MhState *st, const int i, const cJSON *entries, const int col, const int asc) {
    const int n = st->count[i];
    for (int a = 1; a < n; a++) { // insertion sort: stable, and a match has a handful of players
        const unsigned char v = st->order[i][a];
        const cJSON *ev = cJSON_GetArrayItem(entries, v);
        int b = a - 1;
        while (b >= 0 && cmp_values(cJSON_GetArrayItem(entries, st->order[i][b]), ev, col, asc) > 0) {
            st->order[i][b + 1] = st->order[i][b];
            b--;
        }
        st->order[i][b + 1] = v;
    }
}

static void apply_sort(MhState *st, const int i, const cJSON *entries, const int col) {
    // the next direction: desc when the same column is ascending, else asc (deob-10.0.7.js:110416)
    const int asc = !(st->col[i] == col && st->asc[i]);
    sort_order(st, i, entries, col, asc);
    st->col[i] = (signed char) col;
    st->asc[i] = (unsigned char) asc;
}

// a listed match's first sight: historyEntries as sent, sortColumn score / asc, then sortBy(i, 'score') once at
// load: score descending (deob-10.0.7.js:110386-110396)
static void ensure_ready(MhState *st, const int i, const cJSON *entries) {
    int n = cJSON_GetArraySize(entries);
    if (n > MH_ENTRIES) n = MH_ENTRIES;
    if (st->ready[i] && st->count[i] == n) return;
    st->count[i] = (unsigned char) n;
    for (int k = 0; k < n; k++) st->order[i][k] = (unsigned char) k;
    st->col[i] = MH_COL_SCORE;
    st->asc[i] = 1;
    apply_sort(st, i, entries, MH_COL_SCORE);
    st->ready[i] = 1;
}

void mh_toggle(MhState *st, const int index) {
    if (index >= 0 && index < MH_MATCHES) st->open[index] = !st->open[index];
}

void mh_sort(MhState *st, const cJSON *matches, const MhOpts *o, const int index, const int col) {
    if (index < 0 || index >= MH_MATCHES || col < 0 || col >= MH_COLS) return;
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(list_item(matches, st, o, index), "historyEntries");
    ensure_ready(st, index, entries);
    apply_sort(st, index, entries, col);
}

int mh_page(MhState *st, const cJSON *matches, const MhOpts *o, const int delta) {
    int first, pages;
    list_range(matches, st, o, &first, &pages);
    // nextPage: page < totalPages; prevPage: page > 1 (deob-10.0.7.js:110400-110405)
    if ((delta > 0 && cur_page(st) >= pages) || (delta < 0 && cur_page(st) <= 1) || !delta) return 0;
    const int page = cur_page(st) + (delta > 0 ? 1 : -1);
    mh_reset(st); // the page's list is built again: collapsed, at the default sort
    st->page = page;
    return 1;
}

void mh_html(StrBuf *sb, const cJSON *matches, MhState *st, const MhOpts *o) {
    int first, pages;
    const int n = list_range(matches, st, o, &first, &pages);
    sb_append(sb, "<div class=\"match-history svelte-1irqx9a\">");
    // isLoading with nothing listed / error / nothing listed / the list (deob-10.0.7.js:110441-110453)
    if (o->loading && !n) {
        sb_append(sb, "<div class=\"loading svelte-1irqx9a\">Loading match history...</div></div>");
        return;
    }
    if (o->error && o->error[0]) {
        sb_append(sb, "<div class=\"error svelte-1irqx9a\">");
        sb_append_esc(sb, o->error);
        sb_append(sb, "</div></div>");
        return;
    }
    if (!n) {
        sb_append(sb, "<div class=\"no-matches svelte-1irqx9a\">No match history available.</div></div>");
        return;
    }
    const char *me = o->player ? o->player : "";
    char a[32], b[32], c[32], d[32];
    sb_append(sb, "<div class=\"match-list svelte-1irqx9a\">");
    for (int index = 0; index < n; index++) {
        const cJSON *m = cJSON_GetArrayItem(matches, first + index);
        const cJSON *entries = cJSON_GetObjectItemCaseSensitive(m, "historyEntries");
        // the player's entry: historyEntries.find(player_name === playerName) (deob-10.0.7.js:110459)
        const cJSON *mine = NULL, *e;
        cJSON_ArrayForEach(e, entries) {
            if (!mine && !strcmp(str(e, "player_name"), me)) mine = e;
        }
        int team = mine ? (int) num(mine, "team") : 0; // Alpha 1, Bravo 2 (deob-10.0.7.js:98164)
        int win = mine && ((team == 1 && num(m, "score_alpha") > num(m, "score_bravo")) || (team == 2 && num(m, "score_bravo") > num(m, "score_alpha")));
        // KrunkNative: the brain's rows carry the player's own line at the top level (team 0 Alpha / 1 Bravo,
        // victory), read when historyEntries lack the name (an account renamed since)
        if (!mine && cJSON_HasObjectItem(m, "kills")) {
            mine = m;
            team = (int) num(m, "team") + 1;
            win = num(m, "victory") > 0;
        }
        const int rank = (int) num(mine, "rank"); // mine?.rank || Unranked (deob-10.0.7.js:110460)
        char score[80];
        js_num(a, sizeof(a), num(m, "score_alpha"));
        js_num(b, sizeof(b), num(m, "score_bravo"));
        // the player's team first (deob-10.0.7.js:110465-110471)
        if (me[0] && team == 2) snprintf(score, sizeof(score), "%s - %s", b, a);
        else snprintf(score, sizeof(score), "%s - %s", a, b);
        // new Date(date).toLocaleDateString("en-GB", 2-digit day / month, numeric year) (deob-10.0.7.js:110487)
        char date[16] = "";
        const time_t t = (time_t) (num(m, "date") / 1000.0);
        struct tm tm;
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        strftime(date, sizeof(date), "%d/%m/%Y", &tm);
        sb_appendf(sb, "<div class=\"match-entry %s svelte-1irqx9a\" onclick=\"playTick(0.5);%s(%d)\"><div class=\"match-summary svelte-1irqx9a\">"
                       "<img alt=\"map thumbnail\" class=\"map-thumbnail svelte-1irqx9a\" src=\"./img/maps/map_%d.png\"> <div class=\"match-info svelte-1irqx9a\">"
                       "<div class=\"rank-display svelte-1irqx9a\"><img class=\"rank-icon svelte-1irqx9a\" src=\"./%s\" alt=\"%d\"> <span class=\"rank svelte-1irqx9a\">%s</span> "
                       "<span class=\"match-date svelte-1irqx9a\">%s</span> ",
                   win ? "win" : "loss", o->fn_match, index, (int) num(m, "map"), ranked_rank_icon(rank), rank, ranked_rank_name(rank), date);
        if (o->show_region) { // regions[region].toUpperCase() (deob-10.0.7.js:110493-110497, 16465-16471)
            const cJSON *rg = cJSON_GetObjectItemCaseSensitive(m, "region");
            const char *code = cJSON_IsNumber(rg) ? ranked_region_code(rg->valueint) : cJSON_IsString(rg) ? rg->valuestring : NULL;
            sb_append(sb, "<span class=\"match-region svelte-1irqx9a\">");
            for (const char *p = code ? code : ""; *p; p++) {
                const char up[2] = {(char) toupper((unsigned char) *p), 0};
                sb_append_esc(sb, up);
            }
            sb_append(sb, "</span>");
        } else sb_append(sb, "<!---->");
        js_num(a, sizeof(a), num(mine, "kills"));
        js_num(b, sizeof(b), num(mine, "deaths"));
        js_num(c, sizeof(c), num(mine, "assists"));
        js_num(d, sizeof(d), num(mine, "score"));
        sb_appendf(sb, "</div> <div class=\"stats svelte-1irqx9a\"><span class=\"stat svelte-1irqx9a\">KDA: %s/%s/%s</span> <span class=\"stat svelte-1irqx9a\">Score: %s</span> ",
                   a, b, c, d);
        // 0 !== mine?.mmr_change: a missing entry or field shows "0 Rating" (deob-10.0.7.js:110503-110509)
        const cJSON *mmr = mine ? cJSON_GetObjectItemCaseSensitive(mine, "mmr_change") : NULL;
        if (!cJSON_IsNumber(mmr) || mmr->valuedouble != 0) {
            const double change = cJSON_IsNumber(mmr) ? mmr->valuedouble : 0;
            js_num(a, sizeof(a), change);
            sb_appendf(sb, "<span class=\"stat %s svelte-1irqx9a\">%s%s Rating</span>", change > 0 ? "positive" : "negative", change > 0 ? "+" : "", a);
        } else sb_append(sb, "<!---->");
        sb_appendf(sb, "</div></div> <div class=\"match-result svelte-1irqx9a\"><div class=\"result svelte-1irqx9a\">%s</div> <div class=\"score svelte-1irqx9a\">%s</div></div></div> ",
                   win ? "WIN" : "LOSS", score);
        if (index < MH_MATCHES && st->open[index]) {
            ensure_ready(st, index, entries);
            // the scoreboard; a click inside stops at it (deob-10.0.7.js:110596-110604)
            sb_append(sb, "<div class=\"match-details svelte-1irqx9a\" onclick=\"event.stopPropagation()\"><table class=\"table-container svelte-1irqx9a\"><thead><tr class=\"table-row svelte-1irqx9a\">");
            for (int col = 0; col < MH_COLS; col++) // "Player " + the arrow of the sorted column (deob-10.0.7.js:110432-110434)
                sb_appendf(sb, "<td class=\"table-header sortable svelte-1irqx9a\" onclick=\"event.stopPropagation();playTick(0.5);%s(%d,%d)\">%s %s</td>", o->fn_sort, index,
                           col, k_labels[col], st->col[index] == col ? (st->asc[index] ? "\xe2\x86\x91" : "\xe2\x86\x93") : "");
            sb_append(sb, "</tr></thead><tbody>");
            for (int k = 0; k < st->count[index]; k++) {
                const cJSON *x = cJSON_GetArrayItem(entries, st->order[index][k]);
                const int xr = (int) num(x, "rank");
                const double kills = num(x, "kills"), deaths = num(x, "deaths");
                sb_appendf(sb, "<tr class=\"table-row %ssvelte-1irqx9a\"><td class=\"table-cell svelte-1irqx9a\"><div class=\"player-info svelte-1irqx9a\">"
                               "<img class=\"player-rank-icon svelte-1irqx9a\" src=\"./%s\" alt=\"%d\"> <span class=\"%s svelte-1irqx9a\">",
                           !strcmp(str(x, "player_name"), me) ? "current-player " : "", ranked_rank_icon(xr), xr, (int) num(x, "team") == 2 ? "blue-team" : "red-team");
                sb_append_esc(sb, str(x, "player_name"));
                sb_append(sb, "</span></div></td>");
                static const char *const cells[] = {"score", "kills", "assists", "deaths"};
                for (int q = 0; q < 4; q++) {
                    js_num(a, sizeof(a), num(x, cells[q]));
                    sb_appendf(sb, "<td class=\"table-cell svelte-1irqx9a\">%s</td>", a);
                }
                // deaths > 0 ? (kills / deaths).toFixed(2) : kills.toFixed(2) (deob-10.0.7.js:110565)
                sb_appendf(sb, "<td class=\"table-cell svelte-1irqx9a\">%.2f</td>", deaths > 0 ? kills / deaths : kills);
                js_num(a, sizeof(a), num(x, "objective_score"));
                js_num(b, sizeof(b), num(x, "damage_done"));
                sb_appendf(sb, "<td class=\"table-cell svelte-1irqx9a\">%s</td><td class=\"table-cell svelte-1irqx9a\">%s</td></tr>", a, b);
            }
            sb_append(sb, "</tbody></table></div>");
        } else sb_append(sb, "<!---->");
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> ");
    // hasPagination && (data || totalPages > 1); Previous / Next play playTick(0.5) as they turn the page
    // (deob-10.0.7.js:110400-110405, 110618-110630)
    const int page = o->with_data ? cur_page(st) : o->page;
    if (o->has_pagination && (o->with_data || pages > 1))
        sb_appendf(sb, "<div class=\"pagination svelte-1irqx9a\"><button class=\"page-btn svelte-1irqx9a\"%s onclick=\"playTick(0.5);%s(-1)\">&lt; Previous</button> "
                       "<span class=\"page-info svelte-1irqx9a\">Page %d of %d</span> <button class=\"page-btn svelte-1irqx9a\"%s onclick=\"playTick(0.5);%s(1)\">Next &gt;</button></div>",
                   page == 1 || o->loading ? " disabled" : "", o->fn_page, page, pages, page == pages || o->loading ? " disabled" : "", o->fn_page);
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div>");
}
