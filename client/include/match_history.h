// v10.0.7's MatchHistory component (svelte-1irqx9a, deob-10.0.7.js:110336-110640), shared by the three places that
// mount it: the Ranked popup's Match History tab (deob-10.0.7.js:114115, no props: it fetches its own pages), the
// in-game profile's Match History tab (deob-10.0.7.js:113205-113221: data = match_history, pageSize 5) and the hub
// profile's (hub-10.0.7.js:2376-2392: data = matchHistory, hasPagination false, showRegion true).
#pragma once

#include <cJSON.h>
#include <ui_logic.h> // StrBuf

#define MH_MATCHES 64 // matches of one list the state follows (v10's list: a page, or the whole data prop)
#define MH_ENTRIES 32 // players of one match

// The component's state: per listed match its expanded flag and its scoreboard's order and sort (v10 keeps the
// sorted historyEntries in the list item itself, and sorts that sorted copy again on the next click).
typedef struct MhState {
    int page;                                    // 1-based (the data prop's pages)
    unsigned char open[MH_MATCHES];              // expanded
    unsigned char ready[MH_MATCHES];             // order / col / asc set for this match
    unsigned char count[MH_MATCHES];             // its historyEntries when ordered
    unsigned char order[MH_MATCHES][MH_ENTRIES]; // historyEntries indices, the shown order
    signed char col[MH_MATCHES];                 // sortColumn (MH_COL_*)
    unsigned char asc[MH_MATCHES];               // sortDirection "asc"
} MhState;

enum { MH_COL_PLAYER, MH_COL_SCORE, MH_COL_KILLS, MH_COL_ASSISTS, MH_COL_DEATHS, MH_COL_KD, MH_COL_OBJECTIVE, MH_COL_DAMAGE, MH_COLS };

typedef struct MhOpts {
    const char *player;    // playerName: whose entry gives the rank, KDA, score, rating and WIN / LOSS
    int show_region;       // showRegion
    int has_pagination;    // hasPagination
    int with_data;         // the data prop is set: `matches` is all of it, paged here by page_size
    int page_size;         // pageSize (5)
    int page, total_pages; // without data: the fetched page and its totalPages
    int loading;           // isLoading
    const char *error;     // the load failed: "Failed to load match history. Please try again."
    const char *fn_match, *fn_sort, *fn_page; // the onclick handlers: fn_match(i), fn_sort(i, col), fn_page(delta)
} MhOpts;

// New data or a fresh mount: page 1, every match collapsed and at the default sort.
void mh_reset(MhState *st);
// The component's markup for `matches` (the fetched page, or the whole data prop when o->with_data).
void mh_html(StrBuf *sb, const cJSON *matches, MhState *st, const MhOpts *o);
// A match's summary clicked: expand / collapse it.
void mh_toggle(MhState *st, int index);
// A scoreboard header clicked: v10's sortBy (deob-10.0.7.js:110410-110428).
void mh_sort(MhState *st, const cJSON *matches, const MhOpts *o, int index, int col);
// Previous / Next of the data prop's pages: 1 when the page changed (the list is built again, collapsed).
int mh_page(MhState *st, const cJSON *matches, const MhOpts *o, int delta);
