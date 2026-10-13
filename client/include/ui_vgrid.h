// Virtual card grids for the long item lists (client/src/ui/ui_vgrid.c): the Select Skin /
// item / spray pickers (ui_loadout.c) and the social hub's market, inventory and item lists
// (social_ui.c). Krunker writes every card into one innerHTML (v7.1.4: thousands for a staff
// account that owns all store items); a VGrid writes only the rows around the scroll position
// (plus VGRID_BUFFER_ROWS above and below) between two sized spacers, so the scroll height,
// the scrollbar and every card's position stay what the full list gives. The cards are the
// caller's own markup (fixed-size inline-blocks, Krunker's skinCard / marketCard / noBtnCard);
// the grid measures their columns and row pitch from the page and re-renders when the
// scroll position leaves the rendered rows. Same C on both backends (html_dom.h only).
#ifndef KN_UI_VGRID_H
#define KN_UI_VGRID_H

#include <html_dom.h>
#include <ui_logic.h>

#define VGRID_BUFFER_ROWS 3   // rows rendered beyond each edge of the viewport
#define VGRID_FIRST_CHUNK 48  // cards of the first render (before any measurement)

typedef void (*VGridCardFn)(StrBuf *out, int pos, void *user); // the card at list position pos

typedef struct VGrid {
    char list_id[48];        // the scroll container (its children are the grid's)
    unsigned serial;         // data-kn-vg of the top spacer: the list on the page is this grid's
    int count;               // cards (list positions 0..count-1)
    VGridCardFn card;
    void *user;
    int cols;                // measured; 0 = not yet
    float pitch;             // row pitch, list-local CSS px
    float origin;            // content y of row 0 (list-local, unscrolled)
    float width;             // the list's offsetWidth when measured
    int first, last;         // rendered positions [first, last)
    int dirty;               // render on the next tick whatever the scroll position
    int pending_top;         // keep this position at the top after the next measurement (-1 none)
    int renders;             // re-renders so far (perf logs / tests)
    char *prefix;            // markup kept before the cards (a <style>), vgrid_set_prefix()
} VGrid;

// Start a grid for list `list_id` with `count` cards; vgrid_html() then writes the list's
// initial children (the caller wraps them in the list element it generates).
void vgrid_begin(VGrid *g, const char *list_id, int count, VGridCardFn card, void *user);
void vgrid_html(VGrid *g, StrBuf *out);
// Markup the list keeps in front of the cards on every render (no box: e.g. its @keyframes).
void vgrid_set_prefix(VGrid *g, const char *html);
// The list changed (search / filter / sort): `count` cards from the top, rendered now.
void vgrid_reset(VGrid *g, int count);
// Once per frame: measure, follow the scroll position. Returns 0 when the list is not on
// the page (another window, or never shown).
int vgrid_tick(VGrid *g);
// Re-render the rendered rows in place (a card's state changed, e.g. equipped / favourite).
void vgrid_refresh(VGrid *g);

#endif
