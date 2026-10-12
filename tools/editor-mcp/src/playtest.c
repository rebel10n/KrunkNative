// playtest: the map in KrunkNative's own game simulation (shared/src/game.c as the offline
// authority, v7.1.4 bots from shared/src/bots.c, the nav grid of shared/src/bot_nav.c).
//
// The simulation runs in a forked child process: the shared game code has no teardown for a
// whole Game, a crash in it must not take the MCP server down, and a timeout must be able to
// stop it. The child writes one JSON report to a pipe and exits.
#define _GNU_SOURCE
#include "jsonw.h"
#include "mcp.h"
#include "schema.h"
#include "util.h"
#include "validate.h"
#include <bots.h>
#include <chat_commands.h>
#include <map_catalog.h>
#include <pcg_basic.h>
#include <shared.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

char *render_spawn_shot(Ctx *ctx, Project *p, const double pos[3], double yaw, char *err, size_t errsz); // render.c

#define MAX_TRACK 48
#define SAMPLE_HZ 4

typedef struct {
    int uid;
    int deaths, falls, kills;
    double dist;
    double last[3];
    double window_start[3];
    double window_t;
    int below;       // currently under the death height (one fall per life)
    double ground[3]; // last sampled position with a small vertical speed
} Track;

static Game *g_game;
static Track g_tracks[MAX_TRACK];
static int g_track_count;
static int g_kills, g_suicides;
static cJSON *g_falls;
static void add_spot(cJSON *arr, const double p[3], const char *what, const int uid, const double extra, const char *extra_key);

static Track *track_of(const int uid) {
    for (int i = 0; i < g_track_count; i++) if (g_tracks[i].uid == uid) return &g_tracks[i];
    if (g_track_count >= MAX_TRACK) return NULL;
    Track *t = &g_tracks[g_track_count++];
    memset(t, 0, sizeof(*t));
    t->uid = uid;
    t->window_t = -1;
    return t;
}

static void sink(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) length;
    if (type != NET_PACKET_KILL) return;
    const NetKillPacket *k = payload;
    Track *v = track_of(k->victim_uid);
    if (v) v->deaths++;
    // a world death below the death height is a fall (the player is already inactive at the next sample)
    const Player *victim = g_game ? game_find_player(g_game, k->victim_uid) : NULL;
    if (v && victim && g_game->map && victim->position.y <= g_game->map->death_y + 5.0f && !v->below) {
        v->below = 1;
        v->falls++;
        if (g_falls) add_spot(g_falls, v->ground[0] || v->ground[1] || v->ground[2] ? v->ground : (double[3]) {victim->position.x, victim->position.y, victim->position.z}, "fell", k->victim_uid, 0, NULL);
    }
    if (k->killer_uid >= 0 && k->killer_uid != k->victim_uid) {
        Track *kt = track_of(k->killer_uid);
        if (kt) kt->kills++;
        g_kills++;
    } else g_suicides++;
}

static void host_log(void *user, const char *text) {
    (void) user;
    log_msg(LOG_DEBUG, "playtest", "%s", text);
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

static void add_spot(cJSON *arr, const double p[3], const char *what, const int uid, const double extra, const char *extra_key) {
    // merge with an earlier spot within 12 units
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        double q[3];
        if (js_vec3(js_get(it, "p"), q) && hypot(q[0] - p[0], q[2] - p[2]) < 12 && fabs(q[1] - p[1]) < 12) {
            cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "count");
            cJSON_SetNumberValue(c, c->valuedouble + 1);
            return;
        }
    }
    if (cJSON_GetArraySize(arr) >= 40) return;
    cJSON *o = cJSON_CreateObject();
    double r[3] = {js_round(p[0], 1), js_round(p[1], 1), js_round(p[2], 1)};
    cJSON_AddItemToObject(o, "p", js_vec3_new(r));
    cJSON_AddStringToObject(o, "kind", what);
    cJSON_AddNumberToObject(o, "uid", uid);
    if (extra_key) cJSON_AddNumberToObject(o, extra_key, js_round(extra, 2));
    cJSON_AddNumberToObject(o, "count", 1);
    cJSON_AddItemToArray(arr, o);
}

// The child: returns the report.
static cJSON *simulate(Doc *doc, const cJSON *args) {
    cJSON *rep = cJSON_CreateObject();
    cJSON *m = doc_map_form(doc);
    map_sanitize_json(m);
    static const cJSON *maps[1];
    maps[0] = m;
    static int modes[1];
    const char *mode_name = js_str(args, "mode", "ffa");
    modes[0] = mode_find(mode_name);
    if (modes[0] < 0) modes[0] = MODE_ID_FFA;
    const double seconds = fmin(fmax(js_num(args, "seconds", 20), 1), 300);
    const int bot_count = (int) fmin(fmax(js_num(args, "bots", 4), 0), GAME_BOTS_MAX);
    pcg32_srandom((uint64_t) js_int(args, "seed", 1234), 54u);

    static Game game;
    memset(&game, 0, sizeof(game));
    g_game = &game;
    game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game.hooks.send = sink;
    game.round_length = (float) (seconds * 1000.0 + 60000.0);
    game_init(&game, 0, 0, 1);
    if (!game.ready || !game.map) {
        cJSON_AddStringToObject(rep, "error", "the game could not start on this map (map_validate shows why)");
        return rep;
    }
    cJSON_AddStringToObject(rep, "mode", mode_name);
    const Map *map = game.map;
    cJSON_AddNumberToObject(rep, "death_y", map->death_y);

    // spawn reachability (nav grid)
    const double t_nav = now_seconds();
    BotNav *nav = bot_nav_for(&game);
    cJSON *reach = cJSON_AddObjectToObject(rep, "spawn_reachability");
    cJSON_AddNumberToObject(reach, "nav_nodes", bot_nav_node_count(nav));
    const int ns = (int) map->spawn_count;
    if (nav && ns > 1) {
        cJSON *rows = cJSON_AddArrayToObject(reach, "reaches");
        int isolated = 0;
        for (int i = 0; i < ns; i++) {
            int n = 0;
            for (int j = 0; j < ns; j++) {
                if (i == j) continue;
                BotPath path;
                if (bot_nav_find_path(&game, map->spawns[i]->position, map->spawns[j]->position, &path) && path.reached) n++;
            }
            isolated += n == 0;
            cJSON_AddItemToArray(rows, cJSON_CreateNumber(n));
        }
        cJSON_AddNumberToObject(reach, "isolated_spawns", isolated);
    }
    cJSON_AddNumberToObject(reach, "ms", js_round((now_seconds() - t_nav) * 1000, 1));

    // bots
    ChatCommandHost host;
    memset(&host, 0, sizeof(host));
    host.game = &game;
    host.log = host_log;
    host.cheats_open = 1;
    if (bot_count > 0) {
        char line[96];
        snprintf(line, sizeof(line), "/bot_add %d difficulty=%s", bot_count, js_str(args, "difficulty", "normal"));
        chat_command_handle(&host, CHAT_CMD_CONSOLE, line);
    }
    cJSON_AddNumberToObject(rep, "bots", game_bots_count(&game));

    // scripted walker
    const cJSON *path = js_get(args, "path");
    Player *walker = NULL;
    int wp_count = cJSON_GetArraySize(path), wp = 1;
    double wp_deadline = 0, wp_t0 = 0;
    cJSON *wp_report = NULL;
    if (wp_count >= 1) {
        walker = player_init(&game);
        if (walker) {
            walker->uid = 900;
            walker->class_index = 0;
            snprintf(walker->name, sizeof(walker->name), "walker");
            game_players_add(&game, walker);
            player_spawn(walker);
            double p0[3];
            if (js_vec3(cJSON_GetArrayItem(path, 0), p0)) {
                walker->position = (vec3) {(float) p0[0], (float) p0[1], (float) p0[2]};
                walker->velocity = (vec3) {0, 0, 0};
            }
            walker->god_mode = 1;
            wp_report = cJSON_CreateArray();
        }
    }

    const float dt = 1.0f / 60.0f;
    const int ticks = (int) (seconds * 60);
    double *tick_ms = malloc(sizeof(double) * (size_t) ticks);
    cJSON *falls = cJSON_CreateArray(), *stuck = cJSON_CreateArray();
    g_falls = falls;
    const double cell = 16;
    int cap = 4096, visited = 0;
    long long *cells = malloc(sizeof(long long) * (size_t) cap);
    double now = 1.0, walker_progress_t = 0;
    double walker_best = 1e30;
    for (int t = 0; t < ticks; t++) {
        now += dt;
        if (walker && wp < wp_count + 1 && wp <= wp_count) {
            if (wp == wp_count) {
                // done
            } else {
                double target[3];
                js_vec3(cJSON_GetArrayItem(path, wp), target);
                const double dx = target[0] - walker->position.x, dz = target[2] - walker->position.z, dy = target[1] - walker->position.y;
                const double d = hypot(dx, dz);
                if (wp_deadline == 0) {
                    wp_t0 = now;
                    wp_deadline = now + d / 12.0 + 6.0;
                    walker_best = d;
                    walker_progress_t = now;
                }
                if (d < walker_best - 0.5) {
                    walker_best = d;
                    walker_progress_t = now;
                }
                const int reached = d < 4.0 && fabs(dy) < 9.0;
                if (reached || now > wp_deadline) {
                    cJSON *e = cJSON_CreateObject();
                    cJSON_AddNumberToObject(e, "waypoint", wp);
                    cJSON_AddBoolToObject(e, "reached", reached);
                    cJSON_AddNumberToObject(e, "seconds", js_round(now - wp_t0, 2));
                    if (!reached) {
                        double at[3] = {js_round(walker->position.x, 1), js_round(walker->position.y, 1), js_round(walker->position.z, 1)};
                        cJSON_AddItemToObject(e, "stopped_at", js_vec3_new(at));
                        cJSON_AddNumberToObject(e, "distance_left", js_round(d, 1));
                    }
                    cJSON_AddItemToArray(wp_report, e);
                    wp++;
                    wp_deadline = 0;
                } else {
                    Input in;
                    memset(&in, 0, sizeof(in));
                    in.seq = t;
                    in.move_dir = 0; // forward
                    in.delta = dt;
                    in.y_dir = (float) atan2(-dx, -dz);
                    in.x_dir = 0;
                    in.jump = (dy > 2.5 && d < 20) || (now - walker_progress_t > 0.6);
                    player_queue_input(walker, &in);
                }
            }
        }
        const double t0 = now_seconds();
        game_tick(&game, (float) (now * 1000.0), dt * 1000.0f);
        tick_ms[t] = (now_seconds() - t0) * 1000.0;
        if (t % (60 / SAMPLE_HZ)) continue;
        for (size_t i = 0; i < game.player_count; i++) {
            Player *pl = game.players[i];
            Track *tr = track_of(pl->uid);
            if (!tr || !pl->active) continue;
            const double pos[3] = {pl->position.x, pl->position.y, pl->position.z};
            if (tr->window_t >= 0) tr->dist += hypot(pos[0] - tr->last[0], pos[2] - tr->last[2]);
            memcpy(tr->last, pos, sizeof(pos));
            if (fabs(pl->velocity.y) < 0.01) memcpy(tr->ground, pos, sizeof(pos));
            if (pos[1] < map->death_y + 2) {
                if (!tr->below) {
                    tr->below = 1;
                    tr->falls++;
                    add_spot(falls, tr->ground, "fell", pl->uid, 0, NULL);
                }
            } else tr->below = 0;
            // stuck: < 2 units in 3 s while not fighting
            GameBot *bot = game_bot_of(&game, pl);
            if (tr->window_t < 0 || now - tr->window_t >= 3.0) {
                if (tr->window_t >= 0 && hypot(pos[0] - tr->window_start[0], pos[2] - tr->window_start[2]) < 2.0 &&
                    (!bot || bot->state != BOT_STATE_ATTACK) && (bot || pl == walker))
                    add_spot(stuck, pos, bot ? bot_state_name(bot->state) : "walker", pl->uid, 3.0, "seconds");
                memcpy(tr->window_start, pos, sizeof(pos));
                tr->window_t = now;
            }
            const long long key = (long long) floor(pos[0] / cell) * 1000003LL + (long long) floor(pos[2] / cell);
            int seen = 0;
            for (int c = 0; c < visited && !seen; c++) seen = cells[c] == key;
            if (!seen && visited < 200000) {
                if (visited == cap) {
                    cap *= 2;
                    long long *nc = realloc(cells, sizeof(long long) * (size_t) cap);
                    if (!nc) break;
                    cells = nc;
                }
                cells[visited++] = key;
            }
        }
    }
    // timings
    qsort(tick_ms, (size_t) ticks, sizeof(double), cmp_double);
    double sum = 0;
    for (int i = 0; i < ticks; i++) sum += tick_ms[i];
    cJSON *tm = cJSON_AddObjectToObject(rep, "tick_ms");
    cJSON_AddNumberToObject(tm, "avg", js_round(sum / ticks, 3));
    cJSON_AddNumberToObject(tm, "p95", js_round(tick_ms[(int) (ticks * 0.95)], 3));
    cJSON_AddNumberToObject(tm, "max", js_round(tick_ms[ticks - 1], 3));
    cJSON_AddNumberToObject(tm, "budget_60hz", 16.667);
    free(tick_ms);
    cJSON_AddNumberToObject(rep, "simulated_seconds", seconds);
    cJSON_AddNumberToObject(rep, "kills", g_kills);
    cJSON_AddNumberToObject(rep, "suicides_or_world_deaths", g_suicides);
    cJSON *players = cJSON_AddArrayToObject(rep, "players");
    int total_falls = 0;
    for (int i = 0; i < g_track_count; i++) {
        const Track *tr = &g_tracks[i];
        Player *pl = game_find_player(&game, tr->uid);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "uid", tr->uid);
        cJSON_AddStringToObject(o, "name", pl ? pl->name : "?");
        cJSON_AddBoolToObject(o, "bot", pl && pl->is_bot);
        cJSON_AddNumberToObject(o, "distance", js_round(tr->dist, 1));
        cJSON_AddNumberToObject(o, "kills", tr->kills);
        cJSON_AddNumberToObject(o, "deaths", tr->deaths);
        cJSON_AddNumberToObject(o, "falls", tr->falls);
        total_falls += tr->falls;
        cJSON_AddItemToArray(players, o);
    }
    cJSON_AddNumberToObject(rep, "falls", total_falls);
    cJSON_AddItemToObject(rep, "fall_spots", falls);
    cJSON_AddItemToObject(rep, "stuck_spots", stuck);
    cJSON *cov = cJSON_AddObjectToObject(rep, "coverage");
    cJSON_AddNumberToObject(cov, "cells_visited", visited);
    cJSON_AddNumberToObject(cov, "cell_size", cell);
    const double walkable = bot_nav_node_count(nav) * 64.0;
    if (walkable > 0) cJSON_AddNumberToObject(cov, "walkable_fraction_visited", js_round(fmin(1.0, visited * cell * cell / walkable), 3));
    if (wp_report) {
        cJSON *w = cJSON_AddObjectToObject(rep, "walker");
        int ok = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, wp_report) ok += js_bool(e, "reached", 0);
        cJSON_AddNumberToObject(w, "waypoints", wp_count - 1);
        cJSON_AddNumberToObject(w, "reached", ok);
        cJSON_AddItemToObject(w, "legs", wp_report);
    }
    free(cells);
    return rep;
}

int t_playtest(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    const double seconds = fmin(fmax(js_num(args, "seconds", 20), 1), 300);
    int pipefd[2];
    if (pipe(pipefd) != 0) return tr_error(r, "pipe failed");
    fflush(NULL);
    const double t0 = now_seconds();
    const pid_t pid = fork();
    if (pid < 0) return tr_error(r, "fork failed");
    if (pid == 0) {
        close(pipefd[0]);
        log_set_stderr(0);
        log_set_sink(NULL);
        cJSON *rep = simulate(p->doc, args);
        char *text = jsonw_print(rep, JSONW_COMPACT);
        size_t n = text ? strlen(text) : 0, off = 0;
        while (off < n) {
            const ssize_t w = write(pipefd[1], text + off, n - off);
            if (w <= 0) break;
            off += (size_t) w;
        }
        _exit(0);
    }
    close(pipefd[1]);
    Sb sb = {0};
    const double limit = seconds * 3 + 60;
    int status = 0, done = 0, killed = 0;
    while (!done) {
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
        poll(&pfd, 1, 200);
        char buf[65536];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) sb_putn(&sb, buf, (size_t) n);
        if (n == 0) {
            waitpid(pid, &status, 0);
            done = 1;
        } else if (now_seconds() - t0 > limit) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            killed = 1;
            done = 1;
        }
    }
    close(pipefd[0]);
    char *text = sb_take(&sb);
    cJSON *rep = text && text[0] ? cJSON_Parse(text) : NULL;
    free(text);
    if (!rep) {
        if (killed) return tr_error(r, "the simulation did not finish within %.0f s", limit);
        if (WIFSIGNALED(status)) return tr_error(r, "the simulation crashed (signal %d): the game code could not run this map", WTERMSIG(status));
        return tr_error(r, "the simulation produced no report (exit %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    cJSON_AddStringToObject(rep, "project", p->id);
    cJSON_AddNumberToObject(rep, "wall_seconds", js_round(now_seconds() - t0, 2));
    if (js_get(rep, "error")) {
        tr_json(r, rep);
        r->is_error = 1;
        return 0;
    }
    log_msg(LOG_INFO, "playtest", "%s: %d bots, %d kills, %d falls, %d stuck spots", p->name, js_int(rep, "bots", 0), js_int(rep, "kills", 0),
            js_int(rep, "falls", 0), cJSON_GetArraySize(js_get(rep, "stuck_spots")));
    if (js_bool(args, "screenshot", 1)) {
        cJSON *m = doc_map_form(p->doc);
        const cJSON *s0 = cJSON_GetArrayItem(js_get(m, "spawns"), 0);
        double pos[3];
        if (js_vec3(s0, pos)) {
            // player.c player_spawn: yaw = -pi/2 * (1 + spawn direction)
            const double dir = cJSON_IsNumber(cJSON_GetArrayItem(s0, 4)) ? cJSON_GetArrayItem(s0, 4)->valuedouble : 0;
            const double yaw = -M_PI / 2 * (1 + dir);
            char err[1024];
            char *b64 = render_spawn_shot(ctx, p, pos, yaw, err, sizeof(err));
            if (b64) {
                tr_text(r, "view from spawn 0");
                tr_image(r, b64, "image/png");
                free(b64);
            } else cJSON_AddStringToObject(rep, "screenshot_error", err);
        }
        cJSON_Delete(m);
    }
    tr_json(r, rep);
    return 0;
}
