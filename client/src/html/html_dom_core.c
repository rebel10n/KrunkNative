// Platform-independent half of the DOM-ops API (client/include/html_dom.h):
// the inline-handler table, the non-inline listeners, dispatch by name, the
// setTimeout-style timers and the native storage fallback. The DOM operations
// themselves live in the backend (engine/engine.c natively, web_dom.c under
// Emscripten); both call html_ui_dispatch()/html_ui_dispatch_listener() when
// the page produces an event and html_timers_tick() once per frame.
#include <html_dom.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#include <GLFW/glfw3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#endif

// --------------------------------------------------------------------------- page contexts

// Every page has its own handler table, listeners and timers (html_ui.h "Pages"):
// context 0 belongs to the game page (index.html, alive for the whole session),
// context 1 to the other page shown (social/editor/viewer), cleared each time a fresh
// document is loaded. Only the current page's timers run.
#define HTML_MAX_LISTENERS 16

typedef struct {
    char elem_id[48];  // "" = window/document
    char type[24];
    HtmlHandlerFn fn;
} HtmlListener;

typedef struct {
    int id;
    double due_ms;
    void (*fn)(void *user);
    void *user;
} HtmlTimer;

typedef struct {
    const HtmlHandlerEntry *handlers;
    size_t handler_count;
    void *handler_user;
    HtmlHandlerFn handler_fallback;
    HtmlListener listeners[HTML_MAX_LISTENERS];
    int listener_count;
    HtmlTimer *timers;
    int timer_count;
    int timer_capacity;
} HtmlDomContext;

static HtmlDomContext g_contexts[2];
static HtmlDomContext *g_ctx = &g_contexts[0];

// Called by html_ui.c (html_ui_init / html_ui_load_page).
void html_dom_core_select_page(int base_page, int fresh);
void html_dom_core_select_page(const int base_page, const int fresh) {
    g_ctx = &g_contexts[base_page ? 0 : 1];
    if (fresh) {
        HtmlTimer *timers = g_ctx->timers;
        const int capacity = g_ctx->timer_capacity;
        memset(g_ctx, 0, sizeof(*g_ctx));
        g_ctx->timers = timers; // keep the allocation, drop the pending timers
        g_ctx->timer_capacity = capacity;
    }
}

// --------------------------------------------------------------------------- handlers

static const char *g_current_handler;  // normalised name while a handler/fallback runs

const char *html_ui_current_handler(void) {
    return g_current_handler;
}

void html_ui_set_handlers(const HtmlHandlerEntry *table, const size_t count, void *user, const HtmlHandlerFn fallback) {
    g_ctx->handlers = table;
    g_ctx->handler_count = table ? count : 0;
    g_ctx->handler_user = user;
    g_ctx->handler_fallback = fallback;
}

// "windows[0].changeTab" -> "windows.0.changeTab"; "return showWindow" -> "showWindow";
// surrounding whitespace dropped. Output is truncated to n-1 bytes.
static void normalise_handler_name(const char *name, char *out, const size_t n) {
    size_t len = 0;
    if (!name) name = "";
    while (*name == ' ' || *name == '\t' || *name == '\n' || *name == '\r') name++;
    if (!strncmp(name, "return ", 7)) {
        name += 7;
        while (*name == ' ') name++;
    }

    for (; *name && len + 1 < n; name++) {
        char c = *name;
        if (c == '[') c = '.';
        else if (c == ']') {
            // "windows[0].x": the ']' is followed by '.', keep a single dot.
            if (name[1] == '.') name++;
            c = '.';
            if (len && out[len - 1] == '.') continue;
        }
        if (c == ' ' || c == '\t' || c == '(' || c == ';') break;
        out[len++] = c;
    }
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '.')) len--;
    out[len] = 0;
}

// html_ui_add_handler: run-time handlers of the game page (context 0)
#define HTML_MAX_ADDED_HANDLERS 8
static HtmlHandlerEntry g_added_handlers[HTML_MAX_ADDED_HANDLERS];
static char g_added_names[HTML_MAX_ADDED_HANDLERS][48];

void html_ui_add_handler(const char *name, const HtmlHandlerFn fn) {
    if (!name || !*name) return;
    int free_slot = -1;
    for (int i = 0; i < HTML_MAX_ADDED_HANDLERS; i++) {
        if (g_added_handlers[i].name && !strcmp(g_added_handlers[i].name, name)) {
            g_added_handlers[i].fn = fn;
            if (!fn) g_added_handlers[i].name = NULL;
            return;
        }
        if (!g_added_handlers[i].name && free_slot < 0) free_slot = i;
    }
    if (!fn || free_slot < 0) return;
    snprintf(g_added_names[free_slot], sizeof(g_added_names[free_slot]), "%s", name);
    g_added_handlers[free_slot] = (HtmlHandlerEntry) {g_added_names[free_slot], fn};
}

static const HtmlHandlerEntry *find_handler(const char *name) {
    if (g_ctx == &g_contexts[0]) {
        for (int i = 0; i < HTML_MAX_ADDED_HANDLERS; i++) {
            if (g_added_handlers[i].name && g_added_handlers[i].fn && !strcmp(g_added_handlers[i].name, name)) return &g_added_handlers[i];
        }
    }
    for (size_t i = 0; i < g_ctx->handler_count; i++) {
        if (g_ctx->handlers[i].name && !strcmp(g_ctx->handlers[i].name, name)) return &g_ctx->handlers[i];
    }
    return NULL;
}

static int quiet_event_type(const char *type) {
    // Hover handlers (playTick on 98 elements) fire on every mouse move: keep them out of the log.
    return type && (!strcmp(type, "mouseenter") || !strcmp(type, "mouseover") || !strcmp(type, "mousemove") || !strcmp(type, "wheel"));
}

static void log_dispatch(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc, const int handled) {
    if (ev && quiet_event_type(ev->type)) return;
    fprintf(stderr, "[ui] handler %s(", name);
    for (int i = 0; i < argc; i++) {
        const HtmlArg *a = &args[i];
        if (i) fputs(", ", stderr);
        switch (a->kind) {
            case HTML_ARG_NUM: fprintf(stderr, "%g", a->num); break;
            case HTML_ARG_STR: fprintf(stderr, "'%s'", a->str ? a->str : ""); break;
            case HTML_ARG_BOOL: fputs(a->num != 0 ? "true" : "false", stderr); break;
            case HTML_ARG_THIS: fputs("this", stderr); break;
            case HTML_ARG_THIS_VALUE: fputs("this.value", stderr); break;
            case HTML_ARG_THIS_CHECKED: fputs("this.checked", stderr); break;
            case HTML_ARG_EVENT: fputs("event", stderr); break;
        }
    }
    fprintf(stderr, ") %s target=%u%s\n", ev && ev->type ? ev->type : "?", ev ? ev->target : 0u,
            handled ? "" : " [unhandled]");
}

int html_ui_dispatch(const char *handler_name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    char name[128];
    normalise_handler_name(handler_name, name, sizeof(name));
    if (!name[0]) return 1;

    HtmlEvent local = {0};
    if (ev) local = *ev;
    if (!local.type) local.type = "";
    if (!local.value) local.value = "";
    if (!args) {
        static const HtmlArg none = {HTML_ARG_NUM, 0, NULL};
        args = &none;
    }
    const int n = argc > 0 ? argc : 0;

    const HtmlHandlerEntry *entry = find_handler(name);
    if (!entry && !strncmp(name, "window.", 7)) {
        // onkeyup="window.searchProfile()" calls the global searchProfile; "window.open" stays.
        memmove(name, name + 7, strlen(name + 7) + 1);
        entry = find_handler(name);
    }
    if (entry && entry->fn) {
        log_dispatch(name, &local, args, n, 1);
        const char *outer = g_current_handler;
        g_current_handler = name;
        const int r = entry->fn(g_ctx->handler_user, &local, args, n);
        g_current_handler = outer;
        return r ? 1 : 0;
    }
    if (g_ctx->handler_fallback) {
        log_dispatch(name, &local, args, n, 1);
        const char *outer = g_current_handler;
        g_current_handler = name;
        const int r = g_ctx->handler_fallback(g_ctx->handler_user, &local, args, n);
        g_current_handler = outer;
        return r ? 1 : 0;
    }
    log_dispatch(name, &local, args, n, 0);
    return 1;
}

// --------------------------------------------------------------------------- listeners


static HtmlListener *find_listener(const char *elem_id, const char *type) {
    if (!elem_id) elem_id = "";
    if (!type) type = "";
    for (int i = 0; i < g_ctx->listener_count; i++) {
        if (!strcmp(g_ctx->listeners[i].elem_id, elem_id) && !strcmp(g_ctx->listeners[i].type, type)) return &g_ctx->listeners[i];
    }
    return NULL;
}

void html_ui_set_listener(const char *elem_id, const char *type, const HtmlHandlerFn fn) {
    if (!type || !*type) return;
    if (!elem_id) elem_id = "";

    HtmlListener *l = find_listener(elem_id, type);
    if (l) {
        l->fn = fn;
        return;
    }
    if (!fn) return;
    if (g_ctx->listener_count == HTML_MAX_LISTENERS) {
        fprintf(stderr, "[ui] too many listeners, '%s' %s not set\n", elem_id, type);
        return;
    }
    l = &g_ctx->listeners[g_ctx->listener_count++];
    snprintf(l->elem_id, sizeof(l->elem_id), "%s", elem_id);
    snprintf(l->type, sizeof(l->type), "%s", type);
    l->fn = fn;
}

int html_ui_dispatch_listener(const char *elem_id, const HtmlEvent *ev) {
    if (!ev || !ev->type) return 1;
    HtmlEvent local = *ev;
    if (!local.value) local.value = "";

    const HtmlListener *l = find_listener(elem_id, local.type);
    const int handled = l && l->fn;
    if (!quiet_event_type(local.type) && strcmp(local.type, "resize") != 0) {
        fprintf(stderr, "[ui] listener %s %s target=%u key=%d button=%d%s\n",
                elem_id && *elem_id ? elem_id : "window", local.type, local.target, local.key, local.button,
                handled ? "" : " [unhandled]");
    }
    if (!handled) return 1;
    return l->fn(g_ctx->handler_user, &local, NULL, 0) ? 1 : 0;
}

// --------------------------------------------------------------------------- keys

int html_key_is_enter(const int key) {
    return key == 257 /* GLFW_KEY_ENTER */ || key == 335 /* GLFW_KEY_KP_ENTER */ || key == 13 /* DOM */;
}

int html_key_is_escape(const int key) {
    return key == 256 /* GLFW_KEY_ESCAPE */ || key == 27 /* DOM */;
}

// --------------------------------------------------------------------------- timers

static int g_timer_next_id = 1;
static double g_timer_now_ms;
static int g_timer_now_valid;

int html_timer_add(const double ms, void (*fn)(void *user), void *user) {
    if (!fn) return 0;
    if (g_ctx->timer_count == g_ctx->timer_capacity) {
        const int capacity = g_ctx->timer_capacity ? g_ctx->timer_capacity * 2 : 16;
        HtmlTimer *grown = realloc(g_ctx->timers, sizeof(HtmlTimer) * (size_t) capacity);
        if (!grown) return 0;
        g_ctx->timers = grown;
        g_ctx->timer_capacity = capacity;
    }
    HtmlTimer *t = &g_ctx->timers[g_ctx->timer_count++];
    t->id = g_timer_next_id++;
    if (g_timer_next_id <= 0) g_timer_next_id = 1;
    // Before the first tick the clock origin is unknown: timers added then are relative
    // to the first tick, like setTimeout before the page's first frame.
    t->due_ms = (g_timer_now_valid ? g_timer_now_ms : 0.0) + (ms > 0 ? ms : 0);
    t->fn = fn;
    t->user = user;
    return t->id;
}

void html_timer_cancel(const int id) {
    if (id <= 0) return;
    for (int i = 0; i < g_ctx->timer_count; i++) {
        if (g_ctx->timers[i].id == id) {
            g_ctx->timers[i] = g_ctx->timers[g_ctx->timer_count - 1];
            g_ctx->timer_count--;
            return;
        }
    }
}

void html_timers_tick(const double now_ms) {
    if (!g_timer_now_valid) {
        // Rebase timers created before the first tick onto the real clock.
        for (int i = 0; i < g_ctx->timer_count; i++) g_ctx->timers[i].due_ms += now_ms;
        g_timer_now_valid = 1;
    }
    g_timer_now_ms = now_ms;

    // Fire in due order; a callback may add or cancel timers, so rescan after each one
    // and never fire timers created during this tick (their due time is >= now).
    for (;;) {
        int best = -1;
        for (int i = 0; i < g_ctx->timer_count; i++) {
            if (g_ctx->timers[i].due_ms <= now_ms && (best < 0 || g_ctx->timers[i].due_ms < g_ctx->timers[best].due_ms)) best = i;
        }
        if (best < 0) break;
        const HtmlTimer fired = g_ctx->timers[best];
        g_ctx->timers[best] = g_ctx->timers[g_ctx->timer_count - 1];
        g_ctx->timer_count--;
        fired.fn(fired.user);
    }
}

// --------------------------------------------------------------------------- storage (native)

#ifndef __EMSCRIPTEN__

static int storage_dir(char *out, const size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int written;
    if (xdg && *xdg) {
        written = snprintf(out, n, "%s/krunknative", xdg);
    } else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home) home = ".";
        written = snprintf(out, n, "%s/.config/krunknative", home);
    }
    return written > 0 && (size_t) written < n;
}

static int make_dir(const char *path) {
#ifdef _WIN32
    return mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

// Creates "<dir>/<key>.json"; `key` is reduced to [A-Za-z0-9_.-] so it can never escape the directory.
static int storage_path(const char *key, char *out, const size_t n, const int create) {
    if (!key || !*key) return 0;
    char dir[512];
    if (!storage_dir(dir, sizeof(dir))) return 0;
    if (create) {
        // mkdir -p for the two levels we own (~/.config may already exist; create it if not).
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0;
            make_dir(parent);
        }
        if (!make_dir(dir)) return 0;
    }
    char safe[96];
    size_t len = 0;
    for (const char *p = key; *p && len + 1 < sizeof(safe); p++) {
        const char c = *p;
        const int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        safe[len++] = ok ? c : '_';
    }
    safe[len] = 0;
    if (!len || !strcmp(safe, ".") || !strcmp(safe, "..")) return 0;
    const int written = snprintf(out, n, "%s/%s.json", dir, safe);
    return written > 0 && (size_t) written < n;
}

// native builds have no BroadcastChannel (html_dom.h): the editor bus connects windows
int html_channel_open(const char *name) { (void) name; return 0; }
void html_channel_post(const char *name, const char *text) { (void) name; (void) text; }
char *html_channel_take(const char *name) { (void) name; return NULL; }

int html_storage_get(const char *key, char *buf, const size_t n) {
    char path[640];
    if (!storage_path(key, path, sizeof(path), 0)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    if (buf && n) {
        const size_t want = (size_t) size < n - 1 ? (size_t) size : n - 1;
        const size_t got = fread(buf, 1, want, f);
        buf[got] = 0;
    }
    fclose(f);
    return (int) size;
}

void html_storage_set(const char *key, const char *value) {
    char path[640];
    if (!storage_path(key, path, sizeof(path), 1)) return;
    if (!value) {
        remove(path);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[ui] storage: cannot write %s\n", path);
        return;
    }
    fwrite(value, 1, strlen(value), f);
    fclose(f);
}

// --------------------------------------------------------------------------- page address + clipboard (native)
// No address bar natively: the URL calls are no-ops (web_dom.c has the browser's).

int html_page_query(char *buf, const size_t n) {
    if (buf && n) buf[0] = 0;
    return -1;
}

void html_page_set_url(const char *href, const int push) {
    (void) href;
    (void) push;
}

// Links copied from the native client point at the public web client.
void html_page_origin(char *buf, const size_t n) {
    if (buf && n) snprintf(buf, n, "%s", KN_PUBLIC_ORIGIN);
}

void html_clipboard_write(const char *text) {
    GLFWwindow *window = glfwGetCurrentContext();
    if (window && text) glfwSetClipboardString(window, text);
}

#endif // !__EMSCRIPTEN__

// --------------------------------------------------------------------------- game content URLs

void html_dom_asset_url(const char *path, char *out, const size_t n) {
    if (!out || !n) return;
    char rel[512];
    snprintf(rel, sizeof(rel), "/assets/%s", path ? path : "");
    char *url = asset_url(rel);
    snprintf(out, n, "%s", url ? url : rel);
    free(url);
}
