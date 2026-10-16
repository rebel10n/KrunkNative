#pragma once
// Shared building blocks of Krunker v10.0.2's Svelte popups on the v10 page, for every C UI module
// that ports one (implemented in client/src/ui/v10_competitive.c; first users: ranked_ui.c
// rankedMenuModal, turf_ui.c Turf Wars, v10_competitive.c Leaderboards). Keep this API stable:
//   * genericPopup({component}, cls) (v10:215938): the component's markup in #popupHolder >
//     #genericPop with the popup's class (defaultModal, rankedMenuModal, ...); the render callback
//     regenerates it in place (scroll offsets kept) when its data arrives;
//   * the view stack (_5216 viewStack / _5235(component).push(), mounted in #app, v10:223075):
//     io-popup dialogs above everything (confirm, error, success, nested views);
//   * a request cache for the brain's JSON routes (v10's request cache: one answer per path, kept
//     ttl_ms) whose answers re-render the open popup and the top view;
//   * markup of shared Svelte components: tabs (svelte-1o3qul6), main-button (svelte-whnh40), the
//     generic table (svelte-69sryq), the currency display (svelte-1r6bdx8), the market card
//     (svelte-r5taai), the error / success io-popups.
// Inline handlers it writes: knViewPop(accepted), knViewBackdrop(). The markup classes match v10's
// templates, so ui/css/svelte.css styles them.
#include <cJSON.h>
#include <html_dom.h>
#include <ui_logic.h>

// ---- genericPopup
typedef void (*V10RenderFn)(void);
// resetPops(); #genericPop gets class `cls` and `html`; render() regenerates it (data arrived)
void v10_popup_show(const char *cls, const char *html, V10RenderFn render);
int  v10_popup_open(const char *cls);     // #genericPop is shown with this class
// replace the open popup's markup, keeping the scroll offsets of its scroll containers
void v10_popup_set(const char *cls, const char *html);
void v10_popup_close(void);               // clearPops()
void v10_refresh(void);                   // re-render the open popup and the view stack's top

// ---- view stack (#app > .view-stack)
// html: the pushed component's markup (usually v10_io_popup_*); render (may be NULL) regenerates
// it on v10_refresh(); on_close runs when it is popped (accepted: 1 confirm, 0 dismissed / cancel)
typedef void (*V10ViewClose)(int accepted, void *user);
void v10_view_push(char *html, V10RenderFn render, V10ViewClose on_close, void *user); // takes html
void v10_view_set(char *html);            // replace the top view's markup (takes html)
void v10_view_pop(int accepted);          // _5221(accepted)
void v10_view_clear(void);                // _5227(): every view, no callbacks fire as accepted
int  v10_view_depth(void);

// ---- request cache
typedef struct V10Req {
    int status;          // 0 = pending, HTTP status otherwise (transport error: -1)
    cJSON *json;         // parsed body (NULL until loaded / not JSON)
} V10Req;
// GET backend path (cached ttl_ms; 0 = this call only re-fetches when no answer is cached)
const V10Req *v10_get(const char *path, int ttl_ms);
// The Leaderboards window's Rating tab for a krunker.io account (ranked_ui.c): NULL when the account is not one
const V10Req *ranked_ui_rating_board(int season, int region);
void v10_forget(const char *prefix);      // drop cached answers whose path starts with prefix ("" = all)
void v10_scenario_answer(const char *path, const cJSON *json); // --render-scenario: a fixed answer (path without query = any query)
// POST / PUT / DELETE: fn gets the answer once (no cache); body may be NULL
typedef void (*V10DoneFn)(void *user, int status, const cJSON *json);
int  v10_send(const char *method, const char *path, const char *json_body, V10DoneFn fn, void *user);
void v10_url_token(char *out, size_t n);  // url-encoded session token ("" for guests)

// ---- markup helpers
void v10_fmt_int(char *out, size_t n, double v);      // Number.toLocaleString('en-US'): 1,234
void v10_tabs(StrBuf *sb, const char *const *labels, int count, int active, const char *handler); // svelte-1o3qul6, margin-bottom 0
void v10_back_button(StrBuf *sb, const char *onclick); // main-button "← Back"
void v10_currency(StrBuf *sb, double value, const char *currency, const char *size, int centered); // svelte-1r6bdx8
void v10_emoji(StrBuf *sb, const char *utf8, const char *cls); // an emoji as its image (ui/img/kn/emoji/), else the text

// generic table (svelte-69sryq)
typedef struct {
    const char *label;
    const char *width;   // NULL = auto
    int right;           // align right
    int sortable;
} V10Col;
// sort_col -1 = none; sort_fmt (one %d: the column, e.g. "knTwSort('mc-maps',%d)") is the sortable
// headers' onclick (NULL: none)
void v10_table_begin(StrBuf *sb, const V10Col *cols, int n, int sort_col, int sort_desc, const char *sort_fmt);
void v10_table_row(StrBuf *sb, const char *onclick, int highlight); // onclick NULL = not clickable
void v10_table_cell(StrBuf *sb, const V10Col *col, const char *text); // escaped
void v10_table_cell_html(StrBuf *sb, const V10Col *col, const char *html);
void v10_table_row_end(StrBuf *sb);
void v10_table_end(StrBuf *sb);

// io-popup dialogs (malloc'd markup for v10_view_push)
char *v10_io_error(const char *title, const char *message);           // svelte-1t9wm2m, "Dismiss"
char *v10_io_success(const char *title, const char *message_html, const char *button, const char *button_mods);

// the store item card (market-card, svelte-r5taai) of skin `index` with extra markup inside
void v10_market_card(StrBuf *sb, int skin_index, int w, int h, const char *position, const char *onclick, const char *children);

// JSON helpers
double v10_num(const cJSON *o, const char *key, double def);
const char *v10_str(const cJSON *o, const char *key);     // "" when absent

// handlers (account_ui / ui_handlers module chain)
int  v10_competitive_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
void v10_competitive_setup(void);         // the page's menu entries (#menuBtnRanked, ...)
void v10_competitive_scenario(const cJSON *state); // --render-scenario state.leaderboards
