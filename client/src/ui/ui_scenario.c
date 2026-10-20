// `client_bin --render-scenario <scenario.json> <out.png>`: renders one tools/uidiff
// scenario through the real client and its C UI logic (menu_ui.c, hud_ui.c, ui_handlers.c)
// and writes the default framebuffer as a PNG. Contract: tools/uidiff/README.md.
//
// The client runs as usual (offline local server, the scenario's map, html_ui over the
// frame) with four differences, all keyed on ui_scenario_active():
//   - main.c creates a non-resizable hidden window of exactly the scenario `size` (with
//     --headless / KRUNKNATIVE_HEADLESS=1 no window at all: an EGL pbuffer of that size,
//     client/include/headless.h) and calls ui_scenario_run() instead of its main loop;
//   - the world is never drawn: ui_scenario_draw_background() paints the scenario
//     `background` (#rrggbb, or an image path such as the reference capture itself) after
//     the frame's glClear; the first-person weapon and the class preview are drawn only
//     when `state.preview` is true;
//   - the engine's animation/timer clock and the HUD clock follow a virtual 60 Hz clock
//     (engine_set_clock_override / hud_ui_set_clock), so the capture is deterministic;
//   - html_storage (settings, class index, name) lives in a temporary XDG_CONFIG_HOME.
//   - state.anim_seek (ms): the capture frames pin the page's animations the way a Web Animations
//     capture does (tools/uidiff/capture-strike.js FINISH_ANIMS): finite animations and transitions
//     finished, infinite ones at currentTime = anim_seek (engine_anim_seek). The Krunker Strike
//     references were taken like that, at 800 ms, so their animations' phases do not depend on
//     when each one started.
//
// Frame sequences (top-level `anim_times` [ms, ...] + `anim_origin`): instead of one capture,
// <out>.<t>.png is written at origin + t for every t, on the same virtual clock, so animation
// durations, delays, easing and setTimeout chains are tested (tools/uidiff/README.md "Frame
// sequences"). The origin is the first frame showing the applied state (`applied`, default);
// the first k * 1.6 s after it (`cycle`: every infinite menu animation at its start, as
// capture-v10-seq.js' restartAnims puts them); the page's boot (`boot`: state.boot raises it again
// as a fresh page, so like `applied` it is the first frame after the state; clock 0 itself comes
// before the page, its map and the local server are up); the first frame after a state
// change of the running game: the first INIT (`init`), the local player's spawn (`spawn`), its
// death (`death`), its next kill (`kill`), the match end (`end`); or the first frame laying out
// an element matching a CSS selector (`selector:<css>`, a box with a size: the page's own timers
// and packets). state.console runs developer console commands once the state is applied
// (`kill`, `end`), or {"at_ms": n, "cmd": "..."} n ms later (`bot 1`, then `kill @bots`), so the
// death / kill / end sequences come from the sandbox server's own packets.
//
// State is applied through the same calls the game and the page use: client_set_menu_open,
// client_enter_game, menu_show_window / the windows.0.changeTab handler, menu_set_class_index,
// the MenuUi net fields + menu_update_instructions, hud_ui_* (override, chat, kill feed,
// death card, end screen, hit marker). See ui_scenario_apply().
#if defined(KRUNKNATIVE_HTML_ENGINE) && !defined(__EMSCRIPTEN__)
#include <gl.h>
#include <GLFW/glfw3.h>
#include <headless.h> // swap/current window that also work without a window (KRUNKNATIVE_HEADLESS)
#include <krunker_lobbies.h> // state.krunkerLobbies
#include <remote_images.h> // state.remoteImages
#include <ui_logic.h>
#include <v10_wild_offer.h>
#include <v10_wallet.h> // state.wallet (v10_wallet.c)
#include <ui_mobile.h> // state.mobile (ui_mobile_force)
#include <v10_notify.h> // S5: state.notify
#include <hud_modes.h>
#include <net_ext.h> // state.voteKick: NetVoteKickStatePacket
#include <nametags.h> // state.kpdCase: the suspect is a player that sees an enemy (nametags_can_see)
#include <modes.h>
#include <ui_interact.h> // state.interact
#include <backend_ui.h> // WP-E part 2c: state.account
#include <alt_manager_ui.h> // state.altAccounts: the Account Manager's stand-in rows
#include <app_mode.h> // state.page: hub / editor / viewer pages
#include <flavor.h> // top-level "flavor": the Krunker Strike page (strike_ui.c)
#include <strike_ui.h> // state.strike
#include <debug_capture.h> // state.pageActions calls with arguments
#include <html_ui.h>
#include <html_dom.h>
#include <v10_social.h> // state.social
#include <v10_menu.h> // state.officialSession
#include <kio_store.h> // state.kioStore
#include <png_write.h>
#include <pcg_basic.h>
#include <stb_image.h>
#include <cJSON.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <player_names.h> // Hide player names: the scenario's names as the game shows them
#include <game_config.h> // state.officialClasses: game_config_apply (the official game's class list)
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#endif
#include "../html/engine/engine.h"
#include <gltf.h> // gltf_set_clock_override: animated GLTF items follow the pinned clock
#include <v10_ui.h> // state.leaderboards / state.v10responses
#include <spectate.h> // state.spectate
#include <hud_map.h>  // state.customMap (the minimap of custom-map games)
#include <backend_ui.h> // store_skin_preview: killcard player card banners
#include <update_ui.h> // state.update: the Outdated box
#include <lobby_ui.h> // state.matchVotes: the end screen's vote cards
#include <v10_party.h> // state.gameCode: the server password popup
#include <console.h> // state.console: ui_console_exec (frame sequences: `kill`, `end`)
#include <hud_endscreen_v10.h> // the v10 end-of-match components (state.endscreen rewards / rank)

// Portable temp dir / env helpers (MinGW has no mkdtemp, setenv or lstat).
static int scenario_make_storage_dir(char *out, const size_t n) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = ".";
    for (int attempt = 0; attempt < 100; attempt++) {
        snprintf(out, n, "%s\\kn-scenario-%d-%d", tmp, (int) _getpid(), attempt);
        if (_mkdir(out) == 0) return 1;
    }
    return 0;
#else
    snprintf(out, n, "/tmp/kn-scenario-XXXXXX");
    return mkdtemp(out) != NULL;
#endif
}

static void scenario_setenv(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

#define SCENARIO_FRAME_MS (1000.0 / 60.0)
// Every infinite animation the menu shows has a period dividing this: #instructions
// `grow 0.8s infinite alternate` (1.6 s cycle) and #merchImg `scaleBob 0.4s alternate`
// (0.8 s). The page's animations start with the first frame at clock 0, so any capture
// clock k * cycle + anim_time shows the phase of `state.anim_time`.
#define SCENARIO_ANIM_CYCLE_MS 1600.0
#define SCENARIO_BOOT_MAX_FRAMES 3000   // page + map + local server ready
#define SCENARIO_SPAWN_MAX_FRAMES 1200  // inGame: ENT -> SPAWN round trip
#define SCENARIO_WALL_LIMIT_S 120.0     // real-time cap for each wait loop
#define SCENARIO_SETTLE_FRAMES 4        // frames between state application and capture
#define SCENARIO_RNG_SEED 0x4b4eu         // deterministic map/spawn picks
#define SCENARIO_SEQ_MAX 64               // anim_times entries
#define SCENARIO_SEQ_ORIGIN_MAX_FRAMES 5400 // 90 s of virtual time for the origin event (a round end, a menu timer)
#define SCENARIO_SEQ_START_MS 1500.0      // a sequence's state is applied no earlier: the boot curtain (0.7 s fade after the first INIT) is gone, as in the long-running v10 page of the references
#define SCENARIO_SEQ_WALL_FPS 20.0          // a sequence's real-time cap: 120 s + its frames at this rate
// the textured background of in-game sequences (backdrop-filter / blur show on it): the same
// pixels as capture-v10-seq.js's checkerPng()
#define SCENARIO_CHECKER_CELL 32
#define SCENARIO_CHECKER_LIGHT 0x8a8a8a
#define SCENARIO_CHECKER_DARK 0x343434

#define SCENARIO_CONSOLE_MAX 16             // state.console entries

typedef enum { ORIGIN_APPLIED, ORIGIN_BOOT, ORIGIN_INIT, ORIGIN_SPAWN, ORIGIN_DEATH, ORIGIN_KILL, ORIGIN_END, ORIGIN_SELECTOR, ORIGIN_CYCLE } ScenarioOrigin;
static const char *const k_origin_names[] = {"applied", "boot", "init", "spawn", "death", "kill", "end", "selector", "cycle"};

void client_tick(Client*, float, float); // main.c

typedef struct {
    int active;
    cJSON *root;
    const cJSON *state;   // state object of root (never NULL while active)
    char out[1024];
    int w, h, frames;
    double anim_time_s;
    double after_ms;      // state.after_ms > 0: capture exactly this long after the state was applied
    double anim_seek_ms;  // state.anim_seek >= 0: the capture frames seek the animations there (engine_anim_seek)
    int seeking;          // the capture frames are running
    int preview;

    int bg_is_color;
    float bg_rgb[3];
    char bg_image[1200];
    unsigned bg_tex, bg_fbo;
    int bg_w, bg_h;

    int bg_checker;       // background "checker[:cell[:#light:#dark]]": generated, no file
    int bg_cell;
    unsigned char bg_light[3], bg_dark[3];

    // frame sequence: anim_times [ms] after the origin, each written to <out>.<t>.png
    double seq_ms[SCENARIO_SEQ_MAX];
    int seq_count, seq_done;
    int seq_origin;       // ScenarioOrigin
    const char *seq_selector; // "selector:<css>": the first frame that lays out a match
    double seq_origin_ms; // virtual clock of the origin frame, < 0 until it happened
    int seq_seen_spawn;   // death origin: the local player was alive first
    unsigned seq_kills0;  // kill origin: the local player's kills when the state was applied
    int seq_failed;

    // state.console: commands at the applied clock + at_ms (strings: at once)
    struct { const char *cmd; double at_ms; int done; } console[SCENARIO_CONSOLE_MAX];
    int console_count;
    double applied_clock_ms; // < 0 until the state is applied
    int kpd_case_done;       // state.kpdCase handed over
    double wall_extra_s;     // real-time allowance a long frame sequence adds to each wait loop

    char root_dir[1024];  // repo root with trailing slash (assets_path() minus "assets/")
    char map_path[1200];
    char storage_dir[256];
    double clock_ms;
    double last_now_s;
} Scenario;

static Scenario g_sc;

/* ------------------------------------------------------------ json helpers */

static const cJSON *st(const char *key) {
    return cJSON_GetObjectItemCaseSensitive(g_sc.state, key);
}

static int st_has(const char *key) {
    const cJSON *item = st(key);
    return item && !cJSON_IsNull(item);
}

static int st_bool(const char *key, const int fallback) {
    const cJSON *item = st(key);
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valuedouble != 0.0;
    return fallback;
}

static int st_int(const char *key, const int fallback) {
    const cJSON *item = st(key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *st_str(const char *key) {
    const cJSON *item = st(key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// state.charge: the charge bar (interactMProg) with this interMProg, or {prog, over} for the
// overCharged cross; drawn by build_ui_render instead of the local player's charge
static void apply_charge(void) {
    const cJSON *v = cJSON_GetObjectItem(g_sc.state, "charge");
    if (cJSON_IsNumber(v)) build_ui_scenario(cJSON_GetNumberValue(v), 0);
    else if (cJSON_IsObject(v)) {
        const cJSON *prog = cJSON_GetObjectItem(v, "prog");
        build_ui_scenario(cJSON_IsNumber(prog) ? cJSON_GetNumberValue(prog) : 0.0, cJSON_IsTrue(cJSON_GetObjectItem(v, "over")));
    }
}

static const char *obj_str(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double obj_num(const cJSON *obj, const char *key, const double fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    return fallback;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t) n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        buf = NULL;
    }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------ setup */

static void resolve_path(const char *path, char *out, const size_t n) {
    if (path[0] == '/') snprintf(out, n, "%s", path);
    else snprintf(out, n, "%s%s", g_sc.root_dir, path);
}

// "name" of a map JSON file, malloc'd ("" when unreadable).
static char *map_file_name(const char *path) {
    char *text = read_text(path);
    cJSON *json = text ? cJSON_Parse(text) : NULL;
    free(text);
    const char *name = obj_str(json, "name");
    char *copy = strdup(name ? name : "");
    cJSON_Delete(json);
    return copy;
}

// assets/maps/<file>.json whose "name" is `name`: the lowercase/underscore file name
// first ("Soul Sanctum" -> soul_sanctum.json), else every map file.
static int find_map(const char *name, char *out, const size_t n) {
    const char *assets = assets_path();
    char guess[128];
    size_t len = 0;
    for (const char *p = name; *p && len + 1 < sizeof(guess); p++) guess[len++] = *p == ' ' ? '_' : (char) tolower((unsigned char) *p);
    guess[len] = 0;
    snprintf(out, n, "%smaps/%s.json", assets, guess);
    char *found = map_file_name(out);
    const int ok = !strcmp(found, name);
    free(found);
    if (ok) return 1;

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%smaps", assets);
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;
    int hit = 0;
    for (struct dirent *e; !hit && (e = readdir(dir));) {
        const size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0) continue;
        snprintf(out, n, "%s/%s", dir_path, e->d_name);
        char *map_name = map_file_name(out);
        hit = !strcmp(map_name, name);
        free(map_name);
    }
    closedir(dir);
    return hit;
}

static void warn_unknown_keys(void) {
    static const char *const known[] = {
        "menu", "window", "settingsTab", "connected", "ready", "inGame", "mapName", "modeName", "region", "name",
        "gameName", "classIndex", "anim_time", "anim_seek", "hud", "chat", "chatOpen", "chatInput", "killfeed", "scoreboard",
        "dead", "killcard", "endscreen", "hit", "preview", "scrollbars", "mobile", "page", "pageQuery", "pageActions",
        "clientChrome", "settingsView", "windowActions", "after_ms", "customMap", "pose", "spectate", "mapFile", "official",
        "officialClasses", // the class list an official krunker.io game sent (game_config_apply, ui_scenario_apply)
        "account", // WP-E part 2c: a stand-in logged-in account (account_ui_scenario)
        "guestEarned", // {kr, score}: a guest's unclaimed rewards (v10_rewards.c: the menu's box, the signup rewards')
        "altAccounts", // the Account Manager's rows [{name, backend: "krunk.io" | "krunker.io", active}] (alt_manager_ui.c)
        "playerList", // the Player List's rows [{uid, name, clan, ping, account, level, hacker, speaking, chatMuted, voiceMuted}] (hud_ui.c)
        "voteKick",   // {target, voter, votes, needed}: a running vote kick's prompt (hud_votekick.c)
        "community", // v10 Community & Events / news without a brain (v10_news_scenario)
        "kioStore", // krunker.io's store answers {limited, ranked} with officialSession (kio_store.c)
        "ranked",  // the Ranked window's state: {tab, info, profile, board, history, shop, status, searching}
        "turf",    // the Turf Wars popup's state: {open, tab, ...} (turf_ui.c)
        "leaderboards", // the v10 Leaderboards popup: {category, tab, region, season} (v10_competitive.c)
        "v10responses", // {path: answer}: the brain's answers for the v10 popups (v10_competitive.c)
        "social",  // the v10 Social hub popup / views: {tab, sub, feed, friends, selected, messages, search, view} (v10_social.c)
        "clan",    // the Clans window (13): {data: clanData, tab, motd, loot, recruit} (clan_ui.c)
        "kpdPhone", // in game: true = the K.P.D phone up with a call in flight, no request sent (hud_votekick.c)
        "kpdPolice", // the K.P.D popup (shoPolicePop): {tab, list: {code, json}}, the list from the fixture (ui_loadout.c)
        "nametags", // in game: the players' nametags (page elements) drawn over the background (the world is not)
        "kpdCase",  // spectating: an officer's case (kpdData) on the first other player, the second its caller (hud_votekick.c)
        "gameCode", // v10 gameCode(): the server password popup (v10_party.c)
        "update",  // the menu's Outdated / Update now box (update_ui_scenario)
        "connectError", // with connected:false, the Lost Connection screen's error text (menu_ui.c)
        "strike",  // Krunker Strike's page: {screen, settingsTab, loading, killfeed, killcard, end, boot} (strike_ui.c)
        "spin",    // the store's prize wheel screen: prizeWheel(spin) without the backend (store_ui.c)
        "spinResult", // {rand, item} (arrays for a multi-spin): the wheel's spin reply (the won item's reveal)
        "spinMulti",  // with spin: the wheel's Nx spin (prizeWheel(i, null, multi))
        "spinHistory", // with spin: a "spinH" reply [{ind, dte, fds}] (the Spin History popup)
        "modeHud", // with modeName: pinned mode HUD state (hud_modes_scenario: scores, flags, gmsg)
        "matchVotes", // [{mode, map, name, votes}]: the end screen's vote cards (a MATCH_VOTE_STATE packet)
        "raidPromo", // 0 / 1: the raid promo roll of a console `end` (hud_endscreen_v10.c)
        "interact",   // #interactMsg's innerHTML in game (ui_interact_scenario): the prompt without an interactable
        "charge",     // in game: the charge bar's interMProg (a number) or {prog, over} (build_ui_scenario)
        "settings",   // {key: value}: setSetting before the state applies (v10's showMinimap off by default)
        "market",     // the v10 Market & Trading popup's state (v10_market_scenario)
        "wallet",     // the Wallet and Inventory modal: {open: true} over the menu (v10_wallet.c)
        "windowScroll", // {id or .selector: scrollTop}: a scroll container of the window after windowActions (clamped: 1e6 = the end)
        "boot",       // S1: true | {loadGamNm: "play" | "host" | "noGames", map}: the boot curtain before the first INIT
        "updateArt",  // the img/menu-bg/<id>.png the Game Updating screen shows (default its first, id 0)
        "autoRespawn", // S1: N: the play prompt's "Auto Respawning in N" subtitle
        "storage",    // {key: "value"}: localStorage entries seeded before the page reads them ("skins": the loadout's weapon skins)
        "notify",     // S5: {notifications, announcement, emoji, tip}: the chat column's v10 widgets, item tooltips (v10_notify.c)
        "servers",    // the brain's /mm/game-list answer {games, totalPlayerCount} (servers_ui_set_games)
        "officialSession", // true: the menu as in a krunker.io server (the header shows the krunker.io account, v10_menu.c)
        "remoteImages", // {url: local file}: the http(s) images the page shows, no network (remote_images.h)
        "krunkerLobbies", // krunker.io's /game-list answer {games, totalPlayerCount} or {error} (krunker_lobbies_set_fixed)
    };
    static const char *const known_seq[] = {
        "console", // developer console commands once the state is applied (`kill`, `end`: frame sequences)
        "endTab",  // the end screen's tab once its results show: "leaderboard" | "performance" | "medals" | "rewards"
    };
    const cJSON *item;
    cJSON_ArrayForEach(item, g_sc.state) {
        int ok = 0;
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]) && !ok; i++) ok = !strcmp(item->string, known[i]);
        for (size_t i = 0; i < sizeof(known_seq) / sizeof(known_seq[0]) && !ok; i++) ok = !strcmp(item->string, known_seq[i]);
        if (!ok) fprintf(stderr, "[scenario] warning: unknown state key '%s' ignored\n", item->string);
    }
}

int ui_scenario_begin(const char *scenario_path, const char *out_png) {
    memset(&g_sc, 0, sizeof(g_sc));
    snprintf(g_sc.out, sizeof(g_sc.out), "%s", out_png);

    // assets_path() is "./assets/" (repo root) or "../assets/" (cmake-build-*): the
    // scenario's repo-relative paths resolve against the same root.
    const char *assets = assets_default_path();
    const size_t alen = strlen(assets);
    snprintf(g_sc.root_dir, sizeof(g_sc.root_dir), "%.*s", (int) (alen >= 7 ? alen - 7 : 0), assets);

    char *text = read_text(scenario_path);
    if (!text) {
        fprintf(stderr, "[scenario] cannot read %s\n", scenario_path);
        return 0;
    }
    g_sc.root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(g_sc.root)) {
        fprintf(stderr, "[scenario] %s is not a JSON object\n", scenario_path);
        return 0;
    }

    const cJSON *size = cJSON_GetObjectItemCaseSensitive(g_sc.root, "size");
    if (!cJSON_IsArray(size) || cJSON_GetArraySize(size) != 2) {
        fprintf(stderr, "[scenario] `size` must be [w, h]\n");
        return 0;
    }
    g_sc.w = cJSON_GetArrayItem(size, 0)->valueint;
    g_sc.h = cJSON_GetArrayItem(size, 1)->valueint;
    if (g_sc.w <= 0 || g_sc.h <= 0) {
        fprintf(stderr, "[scenario] bad size %dx%d\n", g_sc.w, g_sc.h);
        return 0;
    }
    g_sc.frames = (int) obj_num(g_sc.root, "frames", 3);
    if (g_sc.frames < 1) g_sc.frames = 1;

    // optional top-level "flavor" ("base" / "strike"): render that product flavor's page (Krunker
    // Strike: FRVR's ui/strike/index.html, strike_ui.c). Absent = the process flavor. Set before the
    // UI is created, so `make uidiff` renders the Strike page from the scenario file alone.
    const char *flavor = obj_str(g_sc.root, "flavor");
    if (flavor && flavor[0]) flavor_set_key(flavor);

    static cJSON empty_state = {.type = cJSON_Object};
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(g_sc.root, "state");
    g_sc.state = cJSON_IsObject(state) ? state : &empty_state;
    warn_unknown_keys();
    g_sc.anim_time_s = obj_num(g_sc.state, "anim_time", 0.0);
    if (g_sc.anim_time_s < 0.0) g_sc.anim_time_s = 0.0;
    // one-shot animations (the death screen's 1.5 s slide-in): the reference was taken a fixed
    // time after the event, so the capture follows the state by that much instead of the cycle
    g_sc.after_ms = obj_num(g_sc.state, "after_ms", 0.0);
    g_sc.anim_seek_ms = obj_num(g_sc.state, "anim_seek", -1.0);
    g_sc.preview = st_bool("preview", 0);
    // state.mobile: the phone layout (v10's CONTROLS.isMobile: #mobileUI, the joystick, lockless play)
    if (st_has("mobile")) ui_mobile_force(st_bool("mobile", 0));

    // anim_times [ms, ...] + anim_origin: a frame sequence (<out>.<t>.png per time)
    g_sc.seq_origin_ms = -1.0;
    g_sc.applied_clock_ms = -1.0;
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(g_sc.root, "anim_times");
    const cJSON *t;
    cJSON_ArrayForEach(t, times) {
        if (!cJSON_IsNumber(t) || t->valuedouble < 0.0 || g_sc.seq_count >= SCENARIO_SEQ_MAX) {
            fprintf(stderr, "[scenario] `anim_times` takes up to %d times >= 0 (ms)\n", SCENARIO_SEQ_MAX);
            return 0;
        }
        if (g_sc.seq_count && t->valuedouble <= g_sc.seq_ms[g_sc.seq_count - 1]) {
            fprintf(stderr, "[scenario] `anim_times` must increase\n");
            return 0;
        }
        g_sc.seq_ms[g_sc.seq_count++] = t->valuedouble;
    }
    const char *origin = obj_str(g_sc.root, "anim_origin");
    if (origin) {
        g_sc.seq_origin = -1;
        for (int i = 0; i < (int) (sizeof(k_origin_names) / sizeof(k_origin_names[0])); i++) {
            if (!strcmp(origin, k_origin_names[i])) g_sc.seq_origin = i;
        }
        if (!strncmp(origin, "selector:", 9) && origin[9]) {
            g_sc.seq_origin = ORIGIN_SELECTOR;
            g_sc.seq_selector = origin + 9;
        } else if (g_sc.seq_origin == ORIGIN_SELECTOR) {
            g_sc.seq_origin = -1;
        }
        if (g_sc.seq_origin < 0) {
            fprintf(stderr, "[scenario] unknown anim_origin '%s' (applied, cycle, boot, init, spawn, death, kill, end, selector:<css>)\n", origin);
            return 0;
        }
    }

    // background: "#rrggbb", "checker[:cell[:#light:#dark]]" or an image path; default opaque black
    const char *bg = obj_str(g_sc.root, "background");
    g_sc.bg_is_color = 1;
    if (bg && !strncmp(bg, "checker", 7) && (!bg[7] || bg[7] == ':')) {
        g_sc.bg_is_color = 0;
        g_sc.bg_checker = 1;
        g_sc.bg_cell = SCENARIO_CHECKER_CELL;
        unsigned light = SCENARIO_CHECKER_LIGHT, dark = SCENARIO_CHECKER_DARK;
        if (bg[7] == ':') {
            int cell = 0;
            char a[8] = "", b[8] = "";
            const int n = sscanf(bg + 8, "%d:#%6[0-9a-fA-F]:#%6[0-9a-fA-F]", &cell, a, b);
            if (n >= 1 && cell > 0) g_sc.bg_cell = cell;
            if (n == 3) { light = (unsigned) strtoul(a, NULL, 16); dark = (unsigned) strtoul(b, NULL, 16); }
        }
        for (int i = 0; i < 3; i++) {
            g_sc.bg_light[i] = (unsigned char) (light >> (16 - 8 * i));
            g_sc.bg_dark[i] = (unsigned char) (dark >> (16 - 8 * i));
        }
    } else if (bg && bg[0] == '#' && strlen(bg) == 7) {
        const unsigned v = (unsigned) strtoul(bg + 1, NULL, 16);
        g_sc.bg_rgb[0] = (float) ((v >> 16) & 255) / 255.0f;
        g_sc.bg_rgb[1] = (float) ((v >> 8) & 255) / 255.0f;
        g_sc.bg_rgb[2] = (float) (v & 255) / 255.0f;
    } else if (bg && bg[0]) {
        g_sc.bg_is_color = 0;
        resolve_path(bg, g_sc.bg_image, sizeof(g_sc.bg_image));
        struct stat sb;
        if (stat(g_sc.bg_image, &sb) != 0) {
            fprintf(stderr, "[scenario] background '%s' is neither #rrggbb nor a readable image (%s)\n", bg, g_sc.bg_image);
            return 0;
        }
    }

    // state.mapName: start the local server on that map file, exactly like
    // `client_bin <map.json>` (the map list becomes [that map] + the rotation). Without
    // it the client keeps its default rotation list (what the host/games windows show)
    // and the seeded RNG below makes its random map pick repeatable.
    // state.mapFile (part 2a): an explicit map JSON under assets/ ("maps/official/10_kanji.json": the official
    // Kanji, v10.0.2's file; "maps/311_kanji.json": the v3.1.1 Kanji) instead of
    // the mapName lookup
    const char *map_name = st_str("mapName");
    const char *map_file = st_str("mapFile");
    if (map_file && map_file[0]) {
        snprintf(g_sc.map_path, sizeof(g_sc.map_path), "%s%s", assets, map_file);
    } else if (map_name && map_name[0] && !find_map(map_name, g_sc.map_path, sizeof(g_sc.map_path))) {
        fprintf(stderr, "[scenario] warning: no map named '%s' in %smaps, using the rotation\n", map_name, assets);
        g_sc.map_path[0] = 0;
    }
    pcg32_srandom(SCENARIO_RNG_SEED, 1u); // map/mode/spawn picks
    srand(SCENARIO_RNG_SEED);             // menu speech bubbles
    // modeName: the offline game's mode (offline.c offline_mode_id reads KRUNKNATIVE_MODE)
    const char *mode = st_str("modeName");
    if (mode && mode[0]) {
        if (mode_find(mode) < 0) fprintf(stderr, "[scenario] warning: modeName '%s': no such mode, Free for All\n", mode);
        else scenario_setenv("KRUNKNATIVE_MODE", mode);
    }

    // html_storage (settings, class index, name) must not read or write the user's files.
    if (!scenario_make_storage_dir(g_sc.storage_dir, sizeof(g_sc.storage_dir))) {
        fprintf(stderr, "[scenario] could not create a temporary storage directory\n");
        g_sc.storage_dir[0] = 0;
        return 0;
    }
    scenario_setenv("XDG_CONFIG_HOME", g_sc.storage_dir);
    // state.storage {key: "value"}: localStorage entries seeded before the page first reads them
    // (weapon_skin caches "skins" at the menu's first class box update, before the state applies)
    const cJSON *storage = st("storage"), *entry;
    cJSON_ArrayForEach(entry, storage) {
        if (entry->string && cJSON_IsString(entry)) html_storage_set(entry->string, entry->valuestring);
    }

    // The page's animations start at the first style flush: pin the clock before html_ui_init.
    engine_set_clock_override(0.0);
    // Most of doc/ref was captured with headless Chromium's --hide-scrollbars; state.scrollbars
    // = true keeps them (references captured over CDP without it, e.g. win-reticle).
    engine_set_hide_scrollbars(!st_bool("scrollbars", 0));
    hud_ui_set_clock(0.0);
    g_sc.active = 1;
    return 1;
}

int ui_scenario_active(void) { return g_sc.active; }

// state.clientChrome: the desktop client's additions (Exit menu item, Settings > Client
// tab, custom CSS) are drawn; otherwise a scenario renders krunker.io's browser page,
// which is what the v3.1.1 reference captures show.
int ui_scenario_client_chrome(void) { return g_sc.active && st_bool("clientChrome", 0); }

void ui_scenario_window_size(int *w, int *h) {
    *w = g_sc.w;
    *h = g_sc.h;
}

const char *ui_scenario_map_path(void) { return g_sc.map_path[0] ? g_sc.map_path : NULL; }

int ui_scenario_draws_preview(void) { return g_sc.preview; }
int ui_scenario_draws_nametags(void) { return g_sc.active && st_bool("nametags", 0); }

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        for (struct dirent *e; (e = readdir(dir));) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat sb;
#ifdef _WIN32
            if (stat(child, &sb) == 0 && S_ISDIR(sb.st_mode)) remove_tree(child);
#else
            if (lstat(child, &sb) == 0 && S_ISDIR(sb.st_mode)) remove_tree(child);
#endif
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

void ui_scenario_end(void) {
    if (g_sc.storage_dir[0]) remove_tree(g_sc.storage_dir);
    g_sc.storage_dir[0] = 0;
    if (g_sc.bg_fbo) glDeleteFramebuffers(1, &g_sc.bg_fbo);
    if (g_sc.bg_tex) glDeleteTextures(1, &g_sc.bg_tex);
    g_sc.bg_fbo = g_sc.bg_tex = 0;
    hud_ui_set_override(NULL);
    hud_ui_set_override_scoreboard(NULL);
    build_ui_scenario(-1.0, 0);
    cJSON_Delete(g_sc.root);
    g_sc.root = NULL;
    g_sc.active = 0;
}

/* ------------------------------------------------------------ background */

// the "checker" background: cells of bg_cell px from the top-left corner, light first
static unsigned char *checker_pixels(void) {
    g_sc.bg_w = g_sc.w;
    g_sc.bg_h = g_sc.h;
    unsigned char *px = malloc((size_t) g_sc.w * (size_t) g_sc.h * 4);
    if (!px) return NULL;
    for (int y = 0; y < g_sc.h; y++) {
        for (int x = 0; x < g_sc.w; x++) {
            const unsigned char *c = ((x / g_sc.bg_cell) + (y / g_sc.bg_cell)) % 2 ? g_sc.bg_dark : g_sc.bg_light;
            unsigned char *o = px + ((size_t) y * (size_t) g_sc.w + (size_t) x) * 4;
            o[0] = c[0];
            o[1] = c[1];
            o[2] = c[2];
            o[3] = 255;
        }
    }
    return px;
}

static int background_init(void) {
    if (g_sc.bg_is_color) return 1;
    int n = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    unsigned char *px = g_sc.bg_checker ? checker_pixels() : stbi_load(g_sc.bg_image, &g_sc.bg_w, &g_sc.bg_h, &n, 4);
    if (!px) {
        fprintf(stderr, "[scenario] cannot decode background %s\n", g_sc.bg_image);
        return 0;
    }
    glGenTextures(1, &g_sc.bg_tex);
    glBindTexture(GL_TEXTURE_2D, g_sc.bg_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_sc.bg_w, g_sc.bg_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    g_active_texture = 0; // the client's texture-binding cache (main.c)
    if (g_sc.bg_checker) free(px);
    else stbi_image_free(px);
    glGenFramebuffers(1, &g_sc.bg_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sc.bg_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sc.bg_tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[scenario] background framebuffer incomplete (0x%x)\n", status);
        return 0;
    }
    return 1;
}

// Called by client_tick() right after its glClear: the world is replaced by the
// scenario background (flat colour, or the image stretched over the framebuffer).
void ui_scenario_draw_background(void) {
    if (!g_sc.active) return;
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(g_sc.bg_rgb[0], g_sc.bg_rgb[1], g_sc.bg_rgb[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    // The window's stencil is never cleared by the client and starts undefined (non-zero
    // on NVIDIA, 0 on llvmpipe). Dirty it like a real driver may so a renderer that relies
    // on its contents (the HTML engine's nested clips once did) fails uidiff on llvmpipe too.
    GLint clear_stencil = 0;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &clear_stencil);
    glStencilMask(0xFF);
    glClearStencil(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    glClearStencil(clear_stencil);
    if (g_sc.bg_tex) {
        // image rows are top-down: flip while blitting
        GLint draw = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sc.bg_fbo);
        glBlitFramebuffer(0, 0, g_sc.bg_w, g_sc.bg_h, 0, g_sc.h, g_sc.w, 0, GL_COLOR_BUFFER_BIT,
                          g_sc.bg_w == g_sc.w && g_sc.bg_h == g_sc.h ? GL_NEAREST : GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) draw);
    }
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

/* ------------------------------------------------------------ frames */

// One client frame at virtual time `clock_ms` (the real main loop's client_frame with
// a pinned clock). `present` = 0 keeps the back buffer for the read-back.
static void scenario_frame(Client *client, const double clock_ms, const int present) {
    g_sc.clock_ms = clock_ms;
    engine_set_clock_override(clock_ms);
    // state.anim_seek: before this frame's engine update ticks the animations (FINISH_ANIMS)
    if (g_sc.seeking && g_sc.anim_seek_ms >= 0.0) engine_anim_seek(engine_get(), clock_ms, g_sc.anim_seek_ms);
    hud_ui_set_clock(clock_ms / 1000.0);
    gltf_set_clock_override(clock_ms / 1000.0);
    const double now_s = clock_ms / 1000.0;
    double delta = now_s - g_sc.last_now_s;
    if (delta <= 0.0) delta = SCENARIO_FRAME_MS / 1000.0;
    g_sc.last_now_s = now_s;
    client->last_frame_time = now_s;
    hud_modes_scenario(client, st("modeHud")); // the pinned mode state, before the HUD tick reads it
    // state.matchVotes []: no vote cards even when a console `end` makes the sandbox send some. The v10
    // sandbox's map list is the capture's map alone, so its endData.mts has one entry: no #matchVoteHolder,
    // #endMidHolder at top 205px (v10:219684); KrunkNative's offline server rotates every map.
    const cJSON *raid = st("raidPromo");
    if (cJSON_IsNumber(raid)) hud_endscreen_v10_scenario_raid(raid->valueint ? 1 : 0);
    const cJSON *votes = st("matchVotes");
    if (cJSON_IsArray(votes) && !cJSON_GetArraySize(votes) && lobby_vote_state()->count) lobby_vote_reset();
    client_tick(client, (float) now_s, (float) delta);
    if (present) headless_swap_buffers(client->window);
    glfwPollEvents();
}

static int game_connected(const Client *c) {
    return c->game.is_local || c->offline || c->net_connected;
}

static int boot_done(const Client *c) {
    const MenuUi *ui = menu_ui();
    return html_ui_is_ready() && ui->dom_ready && c->game.ready && c->map_loaded && game_connected(c);
}

static int spawned(const Client *c) {
    return c->in_game && c->me && c->me->active;
}

static void run_frames(Client *client, int n);
static int read_back(const char *path);
static void run_console(double clock_ms);
static void apply_kpd_case(Client *client);
static void apply_kpd_police(Client *client);

/* ------------------------------------------------------------ frame sequences */

// <out>.<t>.png: "uidiff/death.png" at 500 ms -> "uidiff/death.500.png"
static void seq_path(const double t_ms, char *out, const size_t n) {
    const size_t len = strlen(g_sc.out);
    const int ext = len > 4 && !strcasecmp(g_sc.out + len - 4, ".png");
    snprintf(out, n, "%.*s.%.0f.png", (int) (ext ? len - 4 : len), g_sc.out, t_ms);
}

// the virtual clock of the next pending capture (INFINITY: none, or the origin has not happened)
static double seq_next_ms(void) {
    if (g_sc.seq_done >= g_sc.seq_count || g_sc.seq_origin_ms < 0.0) return INFINITY;
    return g_sc.seq_origin_ms + g_sc.seq_ms[g_sc.seq_done];
}

// the state change of anim_origin, checked after every frame: the origin is the first frame that
// processed it (the page's animations it starts begin on that frame's clock)
static int seq_origin_reached(const Client *c) {
    switch ((ScenarioOrigin) g_sc.seq_origin) {
    case ORIGIN_INIT: return c->game.ready && c->map_loaded;   // the first INIT
    case ORIGIN_SPAWN: return spawned(c);
    case ORIGIN_DEATH: return g_sc.seq_seen_spawn && c->in_game && (c->net_death_menu_pending || !(c->me && c->me->active));
    case ORIGIN_KILL: return g_sc.applied_clock_ms >= 0.0 && c->me && c->me->kills > g_sc.seq_kills0;
    case ORIGIN_SELECTOR: {
        const HtmlElem e = html_dom_query(0, g_sc.seq_selector);
        HtmlRect r;
        return e && html_dom_get_rect(e, &r) && r.w > 0.0f && r.h > 0.0f;
    }
    case ORIGIN_END: return g_sc.seq_seen_spawn && c->net_end_seconds >= 0 && c->net_end_result != INT32_MIN; // menu_view.c `ended`
    default: return 0; // boot: clock 0 (begin); applied: ui_scenario_run
    }
}

// state.endTab "leaderboard" | "performance" | "medals" | "rewards": the end screen's tab once its results
// show (GUI.endScreen.changeTab, the tab headers' onclick). The results open on the Performance tab
// (hud_end_results), so the tab is set on the first frame the results are interactive.
static void apply_end_tab(void) {
    static int done;
    const char *tab = st_str("endTab");
    if (done || !tab || !tab[0] || !hud_ui_end_screen_interactive()) return;
    done = 1;
    hud_ui_end_screen_tab(tab);
}

// One 60 Hz frame. With a sequence, a frame lands exactly on the next capture time when that
// comes first, and every capture due at this clock is read back before the buffers swap.
static void scenario_step(Client *client) {
    double next = g_sc.clock_ms + SCENARIO_FRAME_MS;
    run_console(g_sc.clock_ms);
    apply_end_tab();
    apply_kpd_case(client);
    if (!g_sc.seq_count) {
        scenario_frame(client, next, 1);
        return;
    }
    const double due = seq_next_ms();
    if (due > g_sc.clock_ms + 1e-6 && due < next) next = due;
    scenario_frame(client, next, 0);
    if (spawned(client)) g_sc.seq_seen_spawn = 1;
    // init: in the page an INIT always follows a drawn first frame (the boot curtain up at opacity
    // 1, menu_ui.c g_boot_settled), so an offline client ready on the boot frame itself has its
    // origin on the next one, where the fade starts (it was a frame early: timing:curtain 21 ms)
    const int boot_frame = g_sc.seq_origin == ORIGIN_INIT && g_sc.clock_ms < SCENARIO_FRAME_MS - 1e-3;
    if (g_sc.seq_origin_ms < 0.0 && !boot_frame && seq_origin_reached(client)) {
        g_sc.seq_origin_ms = g_sc.clock_ms;
        printf("[scenario] sequence origin '%s' at %.3f s\n", k_origin_names[g_sc.seq_origin], g_sc.clock_ms / 1000.0);
    }
    while (!g_sc.seq_failed && seq_next_ms() <= g_sc.clock_ms + 1e-6) {
        char path[1100];
        seq_path(g_sc.seq_ms[g_sc.seq_done], path, sizeof(path));
        if (!read_back(path)) {
            fprintf(stderr, "[scenario] could not write %s\n", path);
            g_sc.seq_failed = 1;
            break;
        }
        printf("[scenario] %dx%d at %.3f s (origin + %.0f ms) -> %s\n", g_sc.w, g_sc.h, g_sc.clock_ms / 1000.0,
               g_sc.seq_ms[g_sc.seq_done], path);
        // the last frame is also <out> itself (smoke renders, a missing-render check)
        if (++g_sc.seq_done == g_sc.seq_count && !read_back(g_sc.out)) g_sc.seq_failed = 1;
    }
    headless_swap_buffers(client->window);
}

static int spectating(const Client *c) {
    return c->in_game && spectate_active();
}

// state.pose [x, y, z, yaw, pitch]: the input script's `pose` (the player at rest there, the
// offline sandbox server's player too), so position-dependent HUD parts (minimap) repeat.
static void apply_pose(Client *client) {
    const cJSON *pose = st("pose");
    if (!cJSON_IsArray(pose) || cJSON_GetArraySize(pose) < 5 || !client->me) return;
    double v[5];
    for (int i = 0; i < 5; i++) v[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(pose, i));
    if (client->offline) {
        const NetTeleportPacket tp = {(float) v[0], (float) v[1] + client->me->height, (float) v[2]};
        client_net_send(client, NET_PACKET_TELEPORT, &tp, sizeof(tp));
    }
    // held while the server's teleport comes back (its corrections would move the view)
    for (int i = 0; i < 30 && client->me; i++) {
        Player *me = client->me;
        me->position = (vec3) {(float) v[0], (float) v[1], (float) v[2]};
        me->last_position = me->position;
        me->velocity = (vec3) {0};
        me->direction = (vec2) {(float) v[4], (float) v[3]};
        run_frames(client, 1);
    }
}

// Runs frames until `done(client)` (or the frame / wall-clock cap). Returns 1 when done.
static int run_until(Client *client, int (*done)(const Client*), const int max_frames, const char *what) {
    const double wall0 = glfwGetTime();
    for (int i = 0; i < max_frames; i++) {
        if (done(client)) return 1;
        scenario_step(client);
        if (glfwGetTime() - wall0 > SCENARIO_WALL_LIMIT_S + g_sc.wall_extra_s) break;
    }
    if (done(client)) return 1;
    fprintf(stderr, "[scenario] timed out waiting for %s\n", what);
    return 0;
}

static void run_frames(Client *client, const int n) {
    for (int i = 0; i < n; i++) scenario_step(client);
}

/* ------------------------------------------------------------ state */

// state.window -> MenuUi window id. Scenario ids are v7.1.4 showWindow(N) ids except 0, which
// is windows[0] (settings; showWindow(1) in Krunker and menu_ui.c); "host" = 8 (Host Custom
// Game), "join" = 24.
static int scenario_window_id(const cJSON *window, int *id) {
    if (cJSON_IsNumber(window)) {
        *id = window->valueint == 0 ? 1 : window->valueint;
        return 1;
    }
    if (cJSON_IsString(window)) {
        if (!strcmp(window->valuestring, "host")) *id = MENU_WINDOW_HOST;
        else if (!strcmp(window->valuestring, "join")) *id = MENU_WINDOW_JOIN;
        else {
            fprintf(stderr, "[scenario] warning: unknown window '%s'\n", window->valuestring);
            return 0;
        }
        return 1;
    }
    return 0;
}

// state.windowActions: inline-handler calls dispatched once the window is open, as the
// window's own markup would (["windows.2.switchTab", 0], ["windows.6.switchTab", 2], ...):
// each entry is [name, args...] with number / string / bool arguments.
static void apply_window_actions(Client *client) {
    const cJSON *actions = st("windowActions");
    const cJSON *action;
    cJSON_ArrayForEach(action, actions) {
        const cJSON *name = cJSON_IsArray(action) ? cJSON_GetArrayItem(action, 0) : action;
        if (!cJSON_IsString(name)) continue;
        HtmlArg args[8];
        int argc = 0;
        for (const cJSON *a = cJSON_IsArray(action) ? name->next : NULL; a && argc < 8; a = a->next) {
            HtmlArg *arg = &args[argc++];
            memset(arg, 0, sizeof(*arg));
            if (cJSON_IsNumber(a)) { arg->kind = HTML_ARG_NUM; arg->num = a->valuedouble; }
            else if (cJSON_IsBool(a)) { arg->kind = HTML_ARG_BOOL; arg->num = cJSON_IsTrue(a); }
            else { arg->kind = HTML_ARG_STR; arg->str = cJSON_IsString(a) ? a->valuestring : ""; }
        }
        HtmlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = "click";
        ev.value = "";
        html_ui_dispatch(name->valuestring, &ev, argc ? args : NULL, argc);
        run_frames(client, 2);
    }
}

// state.windowScroll {id: top}: scrollTop of the window's scroll containers once its actions ran
// (html_dom_set_scroll clamps it: a big value is the end, the Object Manager's #knOmToolsList).
// A key starting with '.' is a selector instead (the first match: v10's Svelte popups have no ids).
static void apply_window_scroll(void) {
    const cJSON *item;
    cJSON_ArrayForEach(item, st("windowScroll")) {
        const HtmlElem e = !item->string ? 0 : item->string[0] == '.' ? html_dom_query(0, item->string) : html_dom_by_id(item->string);
        if (e && cJSON_IsNumber(item)) html_dom_set_scroll(e, 0.0f, (float) item->valuedouble);
        else fprintf(stderr, "[scenario] windowScroll: no #%s\n", item->string ? item->string : "?");
    }
}

static void apply_window(Client *client) {
    int id;
    if (!scenario_window_id(st("window"), &id)) {
        apply_window_actions(client); // no window: the actions open a popup (openGuide, openCommunityMenu)
        apply_window_scroll();
        return;
    }
    if (id == 1) {
        // windows[0].settingType: "basic" (v7's default) or "advanced" (state.settingsView)
        const char *view = st_str("settingsView");
        if (view) menu_ui()->settings_view = !strcmp(view, "advanced");
        else if (st_has("settingsView")) menu_ui()->settings_view = st_int("settingsView", 0) != 0;
    }
    if (id == 1 && st_has("settingsTab")) {
        // windows[0].changeTab(N): the page's own handler (stores the tab, opens settings)
        const HtmlArg arg = {HTML_ARG_NUM, (double) st_int("settingsTab", 0), NULL};
        HtmlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = "click";
        ev.value = "";
        html_ui_dispatch("windows.0.changeTab", &ev, &arg, 1);
        if (menu_ui()->active_window != 1) menu_show_window(1, 1);
    } else {
        menu_show_window(id, 1);
    }
    apply_window_actions(client);
    apply_window_scroll();
}

// connected/ready: the `connect` + `state` net fields that drive #instructions and the
// region label. Applied last (the client's own state mirror rewrites them on changes).
static void apply_net_state(void) {
    MenuUi *ui = menu_ui();
    if (st_has("connected")) ui->connected = st_bool("connected", ui->connected);
    if (st_has("ready")) ui->ready = st_bool("ready", ui->ready);
    const char *error = st_str("connectError");
    if (error) snprintf(ui->connect_error, sizeof(ui->connect_error), "%s", error);
    menu_update_top_bar();
    menu_update_instructions();
}

static void apply_chat(void) {
    const cJSON *chat = st("chat");
    if (!cJSON_IsArray(chat)) return;
    hud_ui_clear_chat();
    const cJSON *row;
    cJSON_ArrayForEach(row, chat) {
        const char *name = obj_str(row, "name"), *msg = obj_str(row, "msg");
        const char *text = obj_str(row, "text"), *color = obj_str(row, "color");
        // an official server's named line ("chi" key [name, ...]): the game's own path (hud_ui_add_official_name_line)
        const char *official = obj_str(row, "official");
        if (official) {
            const char *args[16];
            int argc = 0;
            const cJSON *arg;
            cJSON_ArrayForEach(arg, cJSON_GetObjectItemCaseSensitive(row, "args")) {
                if (argc < 16 && cJSON_IsString(arg)) args[argc++] = arg->valuestring;
            }
            hud_ui_add_official_name_line(official, args, argc, NULL, -1);
            continue;
        }
        // the dev console mirrors every chat line the game receives (events_chat): the sender as the chat shows it
        if (name && msg) {
            char pn[PLAYER_NAME_SIZE]; // the scenario's sender as the game's chat shows it (Hide player names)
            const char *shown = player_names_shown(-1, 0, name, pn, sizeof(pn));
            hud_ui_add_chat(shown, msg, NULL);
            ui_console_on_chat(shown, msg, NET_CHAT_PLAYER);
            continue;
        }
        // White rows are Krunker's plain game lines (type 1: .chatMsg default colour).
        const int plain = !color || !strcasecmp(color, "#ffffff") || !strcasecmp(color, "#fff");
        hud_ui_add_chat(NULL, text ? text : "", plain ? NULL : color);
        ui_console_on_chat(NULL, text ? text : "", NET_CHAT_SERVER);
    }
}

static void apply_chat_input(void) {
    const HtmlElem input = html_dom_by_id("chatInput");
    if (!input) return;
    const char *value = st_str("chatInput");
    if (value) html_dom_set_value(input, value);
    if (st_bool("chatOpen", 0)) html_dom_focus(input);
}

static void apply_killcard(Client *client) {
    if (!st_bool("dead", 0)) return;
    const cJSON *kc = st("killcard");
    HudKillCard card = {0};
    if (cJSON_IsObject(kc)) {
        static char kill_shown[PLAYER_NAME_SIZE];
        card.killer = player_names_shown(-1, 0, obj_str(kc, "killer"), kill_shown, sizeof(kill_shown)); // Hide player names
        card.hide_extras = player_names_extras_hidden(0);
        card.weapon = obj_str(kc, "weapon");
        card.level = (int) obj_num(kc, "level", 1);
        card.score = (int) obj_num(kc, "score", 0);
        card.class_index = (int) obj_num(kc, "classIndex", 0);
        card.flag_code = obj_str(kc, "flag"); // the killer's Country Flag ISO code ("us"): Strike's kill card flag
        // v7 death screen: weaponIcon / myLevel and the life's stats [timeAlive ms, shots, "acc%", kills, score]
        card.weapon_icon = obj_str(kc, "weaponIcon");
        card.my_level = (int) obj_num(kc, "myLevel", 0);
        // part 2a: the killer's player card stat ["KDR", "2.50"], banners (store.skins indices of
        // player cards: Game.store.skins[playerCardIndex].thumbnail) and the report prompt
        const cJSON *pc = cJSON_GetObjectItemCaseSensitive(kc, "pcStat");
        if (cJSON_IsArray(pc) && cJSON_GetArraySize(pc) >= 2) {
            card.pc_stat_name = cJSON_GetStringValue(cJSON_GetArrayItem(pc, 0));
            card.pc_stat_value = cJSON_GetStringValue(cJSON_GetArrayItem(pc, 1));
        }
        static char killer_card[256], my_card[256];
        const cJSON *kcard = cJSON_GetObjectItemCaseSensitive(kc, "killerCard"), *mcard = cJSON_GetObjectItemCaseSensitive(kc, "myCard");
        if (cJSON_IsNumber(kcard)) { store_skin_preview(kcard->valueint, killer_card, sizeof(killer_card)); card.killer_card_url = killer_card; }
        if (cJSON_IsNumber(mcard)) { store_skin_preview(mcard->valueint, my_card, sizeof(my_card)); card.my_card_url = my_card; }
        card.report = (int) obj_num(kc, "report", 0);
        card.weapon_color = obj_str(kc, "weaponColor");
        const cJSON *stats = cJSON_GetObjectItemCaseSensitive(kc, "stats");
        if (cJSON_IsArray(stats) && cJSON_GetArraySize(stats) >= 5) {
            card.stats = 1;
            card.time_alive_ms = cJSON_GetArrayItem(stats, 0)->valueint;
            card.shots = cJSON_GetArrayItem(stats, 1)->valueint;
            card.accuracy = cJSON_GetStringValue(cJSON_GetArrayItem(stats, 2));
            card.kills = cJSON_GetArrayItem(stats, 3)->valueint;
            card.life_score = cJSON_GetArrayItem(stats, 4)->valueint;
        }
    }
    hud_ui_show_death(client, cJSON_IsObject(kc) ? &card : NULL);
}

static void apply_endscreen(Client *client) {
    const cJSON *end = st("endscreen");
    if (!cJSON_IsObject(end)) return;
    const cJSON *votes = st("matchVotes");
    if (cJSON_IsArray(votes)) { // what the server sends with endData.mts (lobby_events.c)
        NetMatchVoteStatePacket packet;
        memset(&packet, 0, sizeof(packet));
        packet.voted = -1;
        const cJSON *v;
        cJSON_ArrayForEach(v, votes) {
            if (packet.count >= NET_MATCH_PICKS) break;
            NetMatchVoteEntry *e = &packet.entries[packet.count++];
            e->mode_id = (int32_t) obj_num(v, "mode", 0);
            e->map_index = (int32_t) obj_num(v, "map", 0);
            e->votes = (uint16_t) obj_num(v, "votes", 0);
            snprintf(e->map_name, sizeof(e->map_name), "%s", obj_str(v, "name") ? obj_str(v, "name") : "");
        }
        lobby_events_handle(client, NET_PACKET_MATCH_VOTE_STATE, &packet, (uint16_t) sizeof(packet));
    }
    // FFA: first place wins. `text` ("#1 VICTORY") is the overlay-canvas title of the end
    // animation (aV.vicTitle), not page markup: the reference background carries it.
    const int place = (int) obj_num(end, "place", 1);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(end, "result");
    if (obj_num(end, "comp", 0)) client->game.config.comp = 1; // a comp match (Game.isComp): v10's "Return to Krunker"
    hud_ui_show_end_screen(client, cJSON_IsNumber(result) ? result->valueint : place == 1, obj_str(end, "nextRound"));
    hud_endscreen_v10_scenario(end); // rank / rp / items / raid / collect (hud_endscreen_v10.c)
    if (obj_num(end, "results", 0)) hud_ui_end_results_now(client); // the GUI.endScreen tabs (performance)
}

// Time-dependent pieces, applied at `clock_ms` for a capture at `capture_ms`.
static void apply_timed(Client *client, const double capture_ms) {
    const double ahead = capture_ms - g_sc.clock_ms; // time until the captured frame
    const cJSON *feed = st("killfeed");
    if (cJSON_IsArray(feed)) {
        const cJSON *row;
        cJSON_ArrayForEach(row, feed) {
            const char *icon = obj_str(row, "weaponIcon");
            char kpn[PLAYER_NAME_SIZE], vpn[PLAYER_NAME_SIZE]; // the names as the kill feed shows them (Hide player names)
            const int killer_you = (int) obj_num(row, "killerIsYou", 0), victim_you = (int) obj_num(row, "victimIsYou", 0);
            hud_ui_add_killfeed_ex(client, player_names_shown(-1, killer_you, obj_str(row, "killer"), kpn, sizeof(kpn)), killer_you,
                                   player_names_shown(-1, victim_you, obj_str(row, "victim"), vpn, sizeof(vpn)), victim_you,
                                   icon, (int) obj_num(row, "headshot", 0), (int) obj_num(row, "streak", -1), // stk: a Nuke kill's icon
                                   obj_num(row, "age_ms", 0.0) - ahead);
        }
    }
    const cJSON *hit = st("hit");
    if (cJSON_IsObject(hit)) {
        hud_ui_hit_elapsed(client, (int) obj_num(hit, "kill", 0), obj_num(hit, "elapsed_ms", 0.0) - ahead);
    }
}

// state.page ("hub" | "editor" | "viewer") + state.pageQuery: show that page (app_mode.h)
// like `client_bin --hub "<query>"`; state.pageActions: handler names dispatched once the
// page is up (e.g. "openSettings", "loginPopup"), or calls with literal arguments
// ("showWindow(3)"), as the page's inline handlers would.
static int page_entered(const Client *c) {
    (void) c;
    return app_mode() != APP_MODE_GAME && html_ui_is_ready();
}

static int apply_page(Client *client) {
    const char *page = st_str("page");
    if (!page || !page[0]) return 1;
    AppMode mode = APP_MODE_COUNT;
    for (int m = 1; m < APP_MODE_COUNT; m++) {
        if (!strcmp(page, app_mode_name((AppMode) m))) mode = (AppMode) m;
    }
    if (mode == APP_MODE_COUNT) {
        fprintf(stderr, "[scenario] unknown page '%s'\n", page);
        return 0;
    }
    app_set_mode(client, mode, st_str("pageQuery"));
    if (!run_until(client, page_entered, SCENARIO_BOOT_MAX_FRAMES, "the page")) return 0;
    run_frames(client, 6); // enter() + the server's replies (in-process: a frame or two)
    const cJSON *actions = st("pageActions");
    const cJSON *action;
    cJSON_ArrayForEach(action, actions) {
        if (!cJSON_IsString(action)) continue;
        if (strchr(action->valuestring, '(')) {
            debug_capture_dispatch(action->valuestring); // "showWindow(3)": a call with arguments
        } else {
            HtmlEvent ev = {.type = "click", .value = ""};
            html_ui_dispatch(action->valuestring, &ev, NULL, 0);
        }
        run_frames(client, 2);
    }
    return 1;
}

static int ui_scenario_apply(Client *client) {
    if (st_str("page")) return apply_page(client);
    const int in_game = st_bool("inGame", 0);

    // identity: name tag, class box, region label
    const char *name = st_str("name");
    if (name && name[0]) {
        snprintf(client->menu_name, sizeof(client->menu_name), "%s", name);
        menu_update_class_box();
    }
    if (st_has("classIndex")) menu_set_class_index(st_int("classIndex", 0), 1);
    // official: a krunker.io lobby (krunker_net.c sets Game.is_official on join): the KrunkNative-only classes are locked
    client->game.is_official = (unsigned char) st_bool("official", 0);
    // an official game is never hosted (its class and weapon rules are krunker.io's, game_weapon_req): the
    // scenario's offline sandbox hosting is dropped, as a krunker.io join starts with no host state
    if (client->game.is_official) game_config_free(&client->game);
    // officialClasses: the class list an official krunker.io game sends once it has loaded (its GAME_CONFIG "classes",
    // which krunker.io never fills with KrunkNative's own classes). Applied the way gc_apply_received applies it on join.
    if (cJSON_IsArray(st("officialClasses"))) {
        cJSON *cfg = cJSON_CreateObject();
        if (cfg) {
            cJSON_AddItemToObject(cfg, "classes", cJSON_Duplicate(st("officialClasses"), 1));
            game_config_apply(&client->game, cfg, GAME_CONFIG_CLAMP_DEDICATED, GAME_HOST_CUSTOM);
            cJSON_Delete(cfg);
        }
    }
    menu_set_region_label(st_str("region"));

    // state.settings {key: value}: setSetting(key, value) first (a capture that changed a setting,
    // e.g. v10's showMinimap, off by default there)
    const cJSON *sets = st("settings");
    const cJSON *set;
    cJSON_ArrayForEach(set, sets) {
        if (!set->string) continue;
        HtmlArg v = {HTML_ARG_NUM, 0, NULL};
        if (cJSON_IsBool(set)) v = (HtmlArg) {HTML_ARG_BOOL, cJSON_IsTrue(set) ? 1 : 0, NULL};
        else if (cJSON_IsNumber(set)) v.num = set->valuedouble;
        else if (cJSON_IsString(set)) v = (HtmlArg) {HTML_ARG_STR, 0, set->valuestring};
        ui_settings_set(set->string, &v);
    }
    // HUD values before the first HUD write
    const cJSON *hud = st("hud");
    if (cJSON_IsObject(hud)) hud_ui_set_override(hud);
    const cJSON *board = st("scoreboard");
    if (cJSON_IsArray(board)) hud_ui_set_override_scoreboard(board);
    const cJSON *plist = st("playerList");
    if (cJSON_IsArray(plist)) hud_votekick_set_override(plist);

    // v7's reference captures load Kanji as a custom map (customsData): no minimap unless the
    // scenario says otherwise (WP-E part 2a, client/src/ui/hud_map.c)
    hud_minimap_set_custom_override(st_bool("customMap", 1));
    if (in_game) {
        // what `play` does: close the menu, ENT -> SPAWN through the local server (or, with
        // state.spectate, the spectate toggle on: ENT spectate -> SPECTATE)
        const int spectate = st_bool("spectate", 0);
        if (spectate) spectate_set_requested(1);
        client_set_menu_open(client, 0);
        client_enter_game(client);
        client->menu_lock_grace_until = 1e18; // a hidden window never holds the pointer lock
        if (spectate) {
            if (!run_until(client, spectating, SCENARIO_SPAWN_MAX_FRAMES, "spectating")) return 0;
        } else if (!run_until(client, spawned, SCENARIO_SPAWN_MAX_FRAMES, "the local player to spawn")) {
            return 0;
        }
        apply_pose(client);
        ui_interact_scenario(st_str("interact")); // the prompt from the next HUD tick on
        apply_charge();
        run_frames(client, 2); // HUD visible + first value write (session edge seeds chat)
        apply_chat();
    }

    const int menu_open = st_bool("menu", !in_game);
    if (menu_open != html_ui_is_menu_open()) client_set_menu_open(client, menu_open);
    if (in_game) client->menu_lock_grace_until = 1e18;
    if (st_has("gameName")) menu_set_game_name(st_str("gameName"));
    // the Servers window's lists: ours and the official krunker.io lobbies, without a brain or krunker.io
    remote_images_scenario(st("remoteImages")); // http(s) images from local files only (thumbnails)
    if (st_has("servers")) servers_ui_set_games(st("servers"));
    if (st_has("krunkerLobbies")) krunker_lobbies_set_fixed(st("krunkerLobbies"));
    if (st_bool("officialSession", 0)) v10_menu_scenario_official(1);
    account_ui_scenario(st("account")); // WP-E part 2c: a stand-in logged-in account (badges, challenges)
    v10_rewards_guest_earned_scenario(st("guestEarned")); // a guest's unclaimed KR / score (v10_rewards.c)
    alt_manager_ui_scenario(st("altAccounts")); // the Account Manager's rows (alt_manager_ui.c): stand-in accounts
    kio_store_scenario(st("kioStore"));  // krunker.io's shop answers (kio_store.c), without krunker.io
    v10_competitive_scenario(g_sc.state); // the v10 popups' brain answers + Leaderboards (v10_competitive.c)
    ranked_ui_scenario(st("ranked"));   // the Ranked popup (ranked_ui.c)
    turf_ui_scenario(st("turf"));       // the Turf Wars popup (turf_ui.c)
    v10_news_scenario(st("community")); // v10 events / polls / news (v10_news.c)
    clan_ui_scenario(st("clan"));       // the Clans window (clan_ui.c)
    if (menu_open) v10_market_scenario(st("market")); // the v10 Market & Trading popup (v10_market.c)
    if (menu_open) v10_wallet_scenario(st("wallet")); // the Wallet and Inventory modal (v10_wallet.c)
    update_ui_scenario(st("update"));   // the Outdated box without a network check
    if (st_has("kpdPhone")) hud_votekick_scenario_phone(st_bool("kpdPhone", 0)); // after the game's INIT, which ends a call
    if (menu_open) apply_window(client);
    if (menu_open) apply_kpd_police(client); // after the window: the popup on its tab, the list from the fixture
    if (menu_open) v10_social_scenario(st("social")); // the v10 Social hub (v10_social.c)
    account_ui_scenario_post(); // account.c2fa: v7.1.4's 2FA popup over the window
    if (st_bool("gameCode", 0)) v10_party_password_prompt(); // v10 gameCode(): the server password popup
    if (menu_open) v10_wild_offer_scenario(st("wildOffer")); // the brain's "wo" reply (v10_wild_offer.c)
    if (menu_open && st_has("spin")) {
        store_ui_scenario_wheel(st_int("spin", 0), st_bool("spinMulti", 0));
        const cJSON *result = st("spinResult");
        if (cJSON_IsObject(result)) {
            run_frames(client, 2); // the wheel's geometry first (store_ui_tick)
            store_ui_scenario_result(result);
        }
        const cJSON *history = st("spinHistory");
        if (cJSON_IsArray(history)) { // the brain's "spinH" reply: the Spin History popup opens
            cJSON *frame = cJSON_CreateArray();
            cJSON_AddItemToArray(frame, cJSON_CreateString("spinH"));
            cJSON_AddItemToArray(frame, cJSON_Duplicate(history, 1));
            store_ui_frame(frame);
            cJSON_Delete(frame);
        }
    }

    const cJSON *vk = st("voteKick"); // io "vk" (target, votes, needed, true, voter): the prompt
    if (cJSON_IsObject(vk)) {
        NetVoteKickStatePacket state;
        memset(&state, 0, sizeof(state));
        state.active = state.opened = 1;
        state.votes = (int32_t) cJSON_GetNumberValue(cJSON_GetObjectItem(vk, "votes"));
        state.needed = (int32_t) cJSON_GetNumberValue(cJSON_GetObjectItem(vk, "needed"));
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(vk, "target")), *v = cJSON_GetStringValue(cJSON_GetObjectItem(vk, "voter"));
        snprintf(state.target, sizeof(state.target), "%s", t ? t : "");
        snprintf(state.voter, sizeof(state.voter), "%s", v ? v : "");
        hud_votekick_on_state(client, &state);
    }

    strike_ui_scenario(client, st("strike")); // Krunker Strike's screens / in-game UI (no-op on the base page)
    run_frames(client, 2);
    apply_killcard(client);
    apply_endscreen(client);
    apply_chat_input();
    menu_boot_scenario(st("boot"));                 // S1: the page before its first INIT (menu_ui.c)
    menu_update_art_scenario(st("updateArt"));      // before apply_net_state shows Game Updating
    menu_auto_respawn_scenario(st("autoRespawn"));  // S1: "Auto Respawning in N" (menu_ui.c)
    v10_notify_scenario(st("notify"));              // S5: notifications, announcement, emoji panel, tooltips (v10_notify.c)
    apply_net_state();
    return 1;
}

/* ------------------------------------------------------------ capture */

static int read_back(const char *path) {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(headless_current_window(), &fb_w, &fb_h);
    if (fb_w != g_sc.w || fb_h != g_sc.h) {
        fprintf(stderr, "[scenario] framebuffer is %dx%d, scenario wants %dx%d\n", fb_w, fb_h, g_sc.w, g_sc.h);
        return 0;
    }
    const size_t stride = (size_t) g_sc.w * 4;
    unsigned char *px = malloc(stride * (size_t) g_sc.h);
    unsigned char *flip = malloc(stride * (size_t) g_sc.h);
    if (!px || !flip) {
        free(px);
        free(flip);
        return 0;
    }
    glFinish();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g_sc.w, g_sc.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    for (int y = 0; y < g_sc.h; y++) {  // GL rows are bottom-up
        memcpy(flip + (size_t) y * stride, px + (size_t) (g_sc.h - 1 - y) * stride, stride);
        for (int x = 0; x < g_sc.w; x++) flip[(size_t) y * stride + (size_t) x * 4 + 3] = 255;
    }
    const int ok = png_write_rgba(path, flip, g_sc.w, g_sc.h);
    free(px);
    free(flip);
    return ok;
}

static void dump_line(const char *line, void *user) {
    (void) user;
    fprintf(stderr, "%s\n", line);
}

// state.console: developer console commands (devConsole.exec) once the state is applied: `kill`
// (the sandbox server kills the local player: the death sequence), `end` (the round ends in 3 s);
// {"at_ms": n, "cmd": "..."} runs n ms of virtual time later (`bot 1`, then `kill @bots` once the
// bot spawned: the kill sequences)
static void run_console(const double clock_ms) {
    for (int i = 0; g_sc.applied_clock_ms >= 0.0 && i < g_sc.console_count; i++) {
        if (g_sc.console[i].done || clock_ms + 1e-6 < g_sc.applied_clock_ms + g_sc.console[i].at_ms) continue;
        g_sc.console[i].done = 1;
        printf("[scenario] console at %.3f s: %s\n", clock_ms / 1000.0, g_sc.console[i].cmd);
        ui_console_exec(g_sc.console[i].cmd);
    }
}

// state.kpdPolice: the K.P.D popup (showPolicePop) on {tab}: an officer's tab asks the server, which the
// scenario does not wait for; its list comes from {list: {code, json}} instead (the server's answer shape).
static void apply_kpd_police(Client *client) {
    const cJSON *kp = st("kpdPolice");
    if (!cJSON_IsObject(kp)) return;
    hud_votekick_scenario_lists(1);
    const cJSON *tab = cJSON_GetObjectItem(kp, "tab");
    ui_loadout_switch_kpd_tab(cJSON_IsNumber(tab) ? tab->valueint : 0);
    const cJSON *list = cJSON_GetObjectItem(kp, "list");
    if (!cJSON_IsObject(list)) return;
    const cJSON *code = cJSON_GetObjectItem(list, "code"), *rows = cJSON_GetObjectItem(list, "json");
    char *text = cJSON_IsArray(rows) ? cJSON_PrintUnformatted(rows) : NULL;
    hud_votekick_scenario_list(client, cJSON_IsNumber(code) ? code->valueint : 1, text ? text : "[]");
    free(text);
}

// state.kpdCase: the officer's case (NET_EXT_KPD_DATA {sus, caller}) once every console command ran
// (`bot N`, then `bot_stop` so the view holds still) and two other players are in the game. The suspect
// is the first player that sees an enemy (KPD Vision then has a bar to colour; else the first player),
// its caller the next other player that is not the one it sees. The case focuses the suspect
// (spectating), and the spectator HUD / nametags show it (KPD Vision on by default).
static void apply_kpd_case(Client *client) {
    if (g_sc.kpd_case_done || g_sc.applied_clock_ms < 0.0 || !st_bool("kpdCase", 0) || !spectate_active()) return;
    for (int i = 0; i < g_sc.console_count; i++)
        if (!g_sc.console[i].done) return;
    const Game *game = &client->game;
    const Player *list[64];
    int n = 0;
    for (size_t i = 0; i < game->player_count && n < 64; i++) {
        const Player *p = game->players[i];
        if (p && p != client->me && p->active && p->mesh) list[n++] = p;
    }
    if (n < 2) return;
    int sus = 0, seen = -1;
    for (int s = 0; s < n && seen < 0; s++) {
        for (int e = 0; e < n; e++) {
            if (e == s || (list[s]->team && list[s]->team == list[e]->team) || !nametags_can_see(game, list[s], list[e]->position)) continue;
            sus = s;
            seen = e;
            break;
        }
    }
    int caller = -1;
    for (int c = 0; c < n && caller < 0; c++)
        if (c != sus && c != seen) caller = c;
    if (caller < 0) caller = sus ? 0 : 1;
    g_sc.kpd_case_done = 1;
    const NetKpdDataPacket d = {.sus = list[sus]->uid, .caller = list[caller]->uid};
    printf("[scenario] K.P.D case: suspect %d (sees %d), caller %d\n", d.sus, seen >= 0 ? list[seen]->uid : -1, d.caller);
    hud_votekick_on_kpd_data(client, &d);
}

static int apply_console(const Client *client) {
    const cJSON *cmd;
    cJSON_ArrayForEach(cmd, st("console")) {
        const char *text = cJSON_IsString(cmd) ? cmd->valuestring : obj_str(cmd, "cmd");
        if (!text || !text[0] || g_sc.console_count >= SCENARIO_CONSOLE_MAX) {
            fprintf(stderr, "[scenario] state.console takes up to %d commands: \"cmd\" or {\"at_ms\": n, \"cmd\": \"cmd\"}\n",
                    SCENARIO_CONSOLE_MAX);
            return 0;
        }
        g_sc.console[g_sc.console_count].cmd = text;
        g_sc.console[g_sc.console_count].at_ms = cJSON_IsObject(cmd) ? obj_num(cmd, "at_ms", 0.0) : 0.0;
        g_sc.console_count++;
    }
    g_sc.seq_kills0 = client->me ? client->me->kills : 0;
    g_sc.applied_clock_ms = g_sc.clock_ms;
    run_console(g_sc.clock_ms); // the at_ms 0 ones
    return 1;
}

static int seq_finished(const Client *c) {
    (void) c;
    return g_sc.seq_done >= g_sc.seq_count || g_sc.seq_failed;
}

// anim_times: frames until every capture is written (the origin may still be ahead: a round end)
static int run_sequence(Client *client) {
    if (g_sc.seq_origin == ORIGIN_APPLIED || g_sc.seq_origin == ORIGIN_CYCLE || g_sc.seq_origin == ORIGIN_BOOT) {
        // the first frame after the state is the origin; timed state (kill feed age, hit marker)
        // is what it is at that frame. cycle: the first animation cycle start from there on.
        // boot: state.boot (menu_boot_scenario) re-arms the page's boot curtain and ring there, as
        // a fresh page; at clock 0 the client was still booting (seq-loading-map-name showed the
        // CLICK TO PLAY its own boot ended in, not v10's "Loading Kanji ...")
        g_sc.seq_origin_ms = g_sc.clock_ms + SCENARIO_FRAME_MS;
        if (g_sc.seq_origin == ORIGIN_CYCLE) g_sc.seq_origin_ms = ceil(g_sc.seq_origin_ms / SCENARIO_ANIM_CYCLE_MS) * SCENARIO_ANIM_CYCLE_MS;
        apply_timed(client, g_sc.seq_origin_ms);
        printf("[scenario] sequence origin '%s' at %.3f s\n", k_origin_names[g_sc.seq_origin], g_sc.seq_origin_ms / 1000.0);
    }
    const int frames = SCENARIO_SEQ_ORIGIN_MAX_FRAMES + g_sc.seq_count +
                       (int) ceil(g_sc.seq_ms[g_sc.seq_count - 1] / SCENARIO_FRAME_MS);
    g_sc.wall_extra_s = frames / SCENARIO_SEQ_WALL_FPS;
    if (!run_until(client, seq_finished, frames, g_sc.seq_origin_ms < 0.0 ? "the sequence origin" : "the frame sequence")) return 1;
    return g_sc.seq_failed ? 1 : 0;
}

int ui_scenario_run(Client *client) {
    if (!g_sc.active) return 2;
    if (!html_ui_available()) {
        fprintf(stderr, "[scenario] the HTML UI is not available\n");
        return 1;
    }
    if (!background_init()) return 1;

    // boot: first frame at clock 0 (the page's animation origin), then 60 Hz
    g_sc.last_now_s = -1.0;
    g_sc.clock_ms = -SCENARIO_FRAME_MS; // the step lands on 0, the page animations' origin
    scenario_step(client);
    if (!run_until(client, boot_done, SCENARIO_BOOT_MAX_FRAMES, "the page, map and local server")) return 1;
    if (g_sc.seq_count && g_sc.seq_origin != ORIGIN_BOOT && g_sc.seq_origin != ORIGIN_INIT) {
        while (g_sc.clock_ms < SCENARIO_SEQ_START_MS) scenario_step(client);
    }
    if (!ui_scenario_apply(client)) return 1;
    if (!apply_console(client)) return 1;
    if (g_sc.seq_count) return run_sequence(client);
    const double applied_ms = g_sc.clock_ms;
    run_frames(client, SCENARIO_SETTLE_FRAMES);

    // capture clock: the first k * cycle + anim_time after the settle frames, reached by
    // one jump and `frames` 60 Hz frames ending exactly on it
    const double anim_ms = g_sc.anim_time_s * 1000.0;
    const double earliest = g_sc.clock_ms + (double) g_sc.frames * SCENARIO_FRAME_MS;
    const double k = ceil((earliest - anim_ms) / SCENARIO_ANIM_CYCLE_MS);
    double capture_ms = (k > 0.0 ? k : 0.0) * SCENARIO_ANIM_CYCLE_MS + anim_ms;
    if (g_sc.after_ms > 0.0 && applied_ms + g_sc.after_ms >= earliest) capture_ms = applied_ms + g_sc.after_ms;
    const double first_ms = capture_ms - (double) (g_sc.frames - 1) * SCENARIO_FRAME_MS;

    g_sc.clock_ms = first_ms - SCENARIO_FRAME_MS; // timed state is applied one frame before the first
    engine_set_clock_override(g_sc.clock_ms);
    hud_ui_set_clock(g_sc.clock_ms / 1000.0);
    apply_timed(client, capture_ms);
    g_sc.seeking = 1;
    for (int i = 0; i < g_sc.frames; i++) {
        const int last = i == g_sc.frames - 1;
        // windowScroll clamps against the scroll range, which is only final once the window has
        // settled (a short window overflows only at its final height): re-apply it against the
        // settled layout, right before the capturing frame (the Object Manager's #knOmToolsList).
        if (last) apply_window_scroll();
        run_console(g_sc.clock_ms); // timed commands (`bot_stop` at_ms) come due here too
        apply_kpd_case(client); // state.kpdCase: the bots of the console's `bot N` may spawn only now
        scenario_frame(client, first_ms + (double) i * SCENARIO_FRAME_MS, !last);
    }
    // Debug: KRUNKNATIVE_SCENARIO_DUMP=<selector> prints the captured frame's box tree of
    // the first match (engine_dump_boxes), for routing layout findings.
    const char *dump = getenv("KRUNKNATIVE_SCENARIO_DUMP");
    if (dump && dump[0]) {
        const HtmlElem e = html_dom_query(0, dump);
        if (e && engine_get()) engine_dump_boxes(engine_get(), e, dump_line, NULL);
        else fprintf(stderr, "[scenario] dump: no match for '%s'\n", dump);
    }
    // Debug: KRUNKNATIVE_SCENARIO_HTML=<path> writes the captured frame's document (its serialized DOM),
    // so a browser can render the same markup (tools/strike/static_ref.py)
    const char *html_out = getenv("KRUNKNATIVE_SCENARIO_HTML");
    if (html_out && html_out[0] && engine_get()) {
        char *doc = dom_outer_html(engine_dom(engine_get()), 0);
        FILE *f = doc ? fopen(html_out, "wb") : NULL;
        if (f) {
            fputs(doc, f);
            fclose(f);
            printf("[scenario] document -> %s\n", html_out);
        }
        free(doc);
    }
    const int ok = read_back(g_sc.out);
    if (ok) printf("[scenario] %dx%d at %.3f s -> %s\n", g_sc.w, g_sc.h, capture_ms / 1000.0, g_sc.out);
    return ok ? 0 : 1;
}

#elif !defined(__EMSCRIPTEN__)
// KRUNKNATIVE_HTML_ENGINE=OFF: no page to render, so no scenario is ever active (ui_logic.h
// still declares the API for the native callers).
#include <ui_logic.h>
#include <stdio.h>

int ui_scenario_begin(const char *scenario_path, const char *out_png) {
    (void) out_png;
    fprintf(stderr, "[scenario] %s: this client was built without the HTML engine\n", scenario_path ? scenario_path : "");
    return 0;
}
int ui_scenario_active(void) { return 0; }
int ui_scenario_client_chrome(void) { return 0; }
void ui_scenario_window_size(int *w, int *h) { (void) w; (void) h; }
const char *ui_scenario_map_path(void) { return NULL; }
int ui_scenario_run(Client *client) { (void) client; return 1; }
void ui_scenario_end(void) {}
void ui_scenario_draw_background(void) {}
int ui_scenario_draws_preview(void) { return 0; }
int ui_scenario_draws_nametags(void) { return 0; }
#endif // KRUNKNATIVE_HTML_ENGINE && !__EMSCRIPTEN__
