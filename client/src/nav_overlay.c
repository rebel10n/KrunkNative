// Krunker v7.1.4 `show_navmeshes` (client/include/nav_overlay.h): a port of mod/devConsole.ts
// buildGeneratedNavOverlay / clearNavOverlay / ensureNavOverlayOwnerCurrent.
//
// v7 adds a THREE.Group to the world scene: one Mesh (the area fills, black at 0.16 opacity)
// and seven LineSegments (borders red, crouch blue, two-way connections cyan, one-way arrows
// blue, jump arrows green, drop arrows red, obstacle areas red), all depthTest false, drawn last
// (renderOrder 996 / 999). Here they are LineMaterial meshes (GL_LINES / triangles) rendered
// through scene_render() after the world scene, one layer at a time in that order; the
// first-person weapon pass still draws over them, as in Krunker. The geometry below follows v7's line for line (insets, lifts,
// arrow heads, connection order).
//
// KrunkNative differences: the build runs on a worker thread natively (v7 builds inside the
// command and stalls the page for up to 22 s on Shipwrecked); its result is cached for the map,
// so `show_navmeshes` after `off` is instant. v7 tracks a map switch through its own console
// commands (rebuilt on the new map) and clears the overlay on any other switch; so does this.
#include <gl.h>
#include "klog.h"
#include <nav_overlay.h>
#include <nav_overlay_geom.h> // KrunkNative: the spectated bot's route / markers (GL-free geometry)
#include <bot_path_net.h>     // NET_EXT_KN_BOT_PATH decode
#include <spectate.h>         // spectate_active / spectate_target_uid, client_find_player
#include <navgen.h>
#include <console.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __EMSCRIPTEN__
#include <pthread.h>
#define NAV_THREADED 1
#endif

#define COL_INFO "#9ac8ff"
#define COL_WARN "#ffb84d"
#define COL_ERROR "#ff6a6a"

// v7's draw order (group children): fill, borders, crouch, twoWay, oneWay, jump, drop, obstacles
enum { NAV_FILL, NAV_BORDER, NAV_CROUCH, NAV_TWO_WAY, NAV_ONE_WAY, NAV_JUMP, NAV_DROP, NAV_OBSTACLE, NAV_LAYERS };

// sourceColors (hex) and opacity per layer
static const int k_layer_hex[NAV_LAYERS] = {0x000000, 0xff0000, 0x0000ff, 0x00ffff, 0x0000ff, 0x00ff00, 0xff0000, 0xff0000};
#define NAV_FILL_OPACITY 0.16f

typedef struct {
    float *v; // x, y, z per vertex
    size_t n, cap;
} NavVerts;

typedef struct {
    const void *map;          // Map*
    const void *raw;          // Map.raw_data
    size_t objects;
    int index;                // Game.current_map_index
} NavKey;

typedef struct {
    NavKey key;
    cJSON *map;               // the build's own copy of the map's objects / xyz / spawns
    NavVerts layers[NAV_LAYERS];
    int areas, nodes, obstacle_links;
    int ok;                   // 0: navgen_build failed (OOM)
    double navgen_ms, total_ms;
} NavBuild;

static struct {
    int shown;                // the overlay meshes are in place
    int show_pending;         // show_navmeshes ran: built from the frame loop (nav_overlay_frame)
    NavKey key;               // the map they belong to
    int restore;              // a console map switch: rebuild on the next map
    NavKey restore_from;      // the map before the switch
    double restore_at;        // when the switch was asked for
    NavBuild *cache;          // the last finished build (kept after `off`, freed on a map change)
    NavBuild *job;            // the running build
    double job_started;
    int job_cancelled;
#ifdef NAV_THREADED
    pthread_t thread;
    pthread_mutex_t lock;
    int lock_ready;
    int job_done;
#endif
    Mesh *meshes[NAV_LAYERS];
    Geometry geometry[NAV_LAYERS];
    unsigned int vbo[NAV_LAYERS];
} g_nav;

// KrunkNative spectate overlay: the followed bot's route, goal and target markers from the
// server's NET_EXT_KN_BOT_PATH (nav_overlay_bot_path), drawn with the nav mesh layers while
// spectating that bot. The geometry is rebuilt every frame (the bot and its target move
// between packets) into dynamic buffers; a packet older than NAV_BOT_STALE_MS is dropped.
#define NAV_BOT_STALE_MS 2000.0
static struct {
    NetBotPathPacket packet;
    int have;
    double at;                // nav_now_ms() of the packet
    NavBotGeom geom;
    Mesh *meshes[NAV_BOT_LAYERS];
    Geometry geometry[NAV_BOT_LAYERS];
    unsigned int vbo[NAV_BOT_LAYERS];
    size_t vbo_floats[NAV_BOT_LAYERS]; // the VBO's capacity
    unsigned int ibo;         // one sequential index buffer for every layer
    size_t ibo_count;
} g_bot;

// ------------------------------------------------------------------------- geometry (CPU)

static int verts_push(NavVerts *b, const double x, const double y, const double z) {
    if (b->n + 3 > b->cap) {
        const size_t cap = b->cap ? b->cap * 2 : 3 * 1024;
        float *grown = realloc(b->v, cap * sizeof(float));
        if (!grown) return 0;
        b->v = grown;
        b->cap = cap;
    }
    b->v[b->n++] = (float) x;
    b->v[b->n++] = (float) y;
    b->v[b->n++] = (float) z;
    return 1;
}

// pushPoint / pushLine / pushTriangle (lift: y offset)
static void push_point(NavVerts *b, const NavGenPoint p, const double lift) { verts_push(b, p.x, p.y + lift, p.z); }

static void push_line(NavVerts *b, const NavGenPoint a, const NavGenPoint c, const double lift) {
    push_point(b, a, lift);
    push_point(b, c, lift);
}

static void push_triangle(NavVerts *b, const NavGenPoint a, const NavGenPoint c, const NavGenPoint d, const double lift) {
    push_point(b, a, lift);
    push_point(b, c, lift);
    push_point(b, d, lift);
}

// centerOf(area): the corner average (y too, unlike navgen's areaCenter)
static NavGenPoint center_of(const NavGenArea *a) {
    return (NavGenPoint) {
        (a->nw.x + a->ne.x + a->se.x + a->sw.x) / 4.0,
        (a->nw.y + a->ne.y + a->se.y + a->sw.y) / 4.0,
        (a->nw.z + a->ne.z + a->sw.z + a->se.z) / 4.0,
    };
}

// insetPoint(point, center, margin = 0.2)
static NavGenPoint inset_point(const NavGenPoint p, const NavGenPoint center) {
    const double dx = center.x - p.x, dz = center.z - p.z;
    const double len = hypot(dx, dz);
    if (len <= 0.0001) return p;
    return (NavGenPoint) {p.x + dx / len * 0.2, p.y, p.z + dz / len * 0.2};
}

// pushArrow(dst, from, to, lift, width)
static void push_arrow(NavVerts *b, const NavGenPoint from, const NavGenPoint to, const double lift, const double width) {
    const double dx = to.x - from.x, dz = to.z - from.z;
    const double len = hypot(dx, dz);
    if (len <= 0.0001) return;
    const double ux = dx / len, uz = dz / len, px = -uz, pz = ux;
    const double head = fmin(12.0, fmax(5.0, len * 0.28));
    const NavGenPoint base = {to.x - ux * head, to.y, to.z - uz * head};
    push_line(b, from, to, lift);
    push_line(b, to, (NavGenPoint) {base.x + px * width, base.y, base.z + pz * width}, lift);
    push_line(b, to, (NavGenPoint) {base.x - px * width, base.y, base.z - pz * width}, lift);
}

static void build_layers(const NavGenMesh *m, NavVerts *layers) {
    for (int a = 0; a < m->area_count; a++) {
        const NavGenArea *area = &m->areas[a];
        const NavGenPoint center = center_of(area);
        const NavGenPoint nw = inset_point(area->nw, center), ne = inset_point(area->ne, center);
        const NavGenPoint se = inset_point(area->se, center), sw = inset_point(area->sw, center);
        push_triangle(&layers[NAV_FILL], nw, se, ne, 0.8);
        push_triangle(&layers[NAV_FILL], se, nw, sw, 0.8);
        push_line(&layers[NAV_BORDER], nw, ne, 1.0);
        push_line(&layers[NAV_BORDER], ne, se, 1.0);
        push_line(&layers[NAV_BORDER], se, sw, 1.0);
        push_line(&layers[NAV_BORDER], sw, nw, 1.0);
        if (area->attrs & NAVGEN_ATTR_CROUCH) push_line(&layers[NAV_CROUCH], nw, se, 1.4);
        if (area->attrs & NAVGEN_ATTR_OBSTACLE) {
            push_line(&layers[NAV_OBSTACLE], nw, ne, 1.8);
            push_line(&layers[NAV_OBSTACLE], ne, se, 1.8);
            push_line(&layers[NAV_OBSTACLE], se, sw, 1.8);
            push_line(&layers[NAV_OBSTACLE], sw, nw, 1.8);
        }
        // Object.values(area.connections): the direction lists in key order
        for (int o = 0; o < area->order_count; o++) {
            const NavGenIdList *list = &area->connections[area->order[o]];
            for (int i = 0; i < list->count; i++) {
                const int target_id = list->ids[i];
                if (target_id < 0 || target_id >= m->area_count) continue;
                const NavGenPoint to = center_of(&m->areas[target_id]);
                const double dx = to.x - center.x, dz = to.z - center.z;
                const double len = hypot(dx, dz);
                if (len <= 0.0001) continue;
                const double reach = fmin(22.0, len * 0.45);
                const NavGenPoint start = {center.x + dx / len * 5.0, center.y, center.z + dz / len * 5.0};
                const NavGenPoint end = {center.x + dx / len * reach, center.y + (to.y - center.y) * 0.45, center.z + dz / len * reach};
                const NavGenAreaLink *link = navgen_area_link(m, area->id, target_id);
                if (link && link->kind == NAVGEN_LINK_JUMP) push_arrow(&layers[NAV_JUMP], start, end, 3.2, 4.0);
                else if (link && link->kind == NAVGEN_LINK_DROP) push_arrow(&layers[NAV_DROP], start, end, 3.2, 4.0);
                else if (link && link->kind == NAVGEN_LINK_CROUCH) push_arrow(&layers[NAV_CROUCH], start, end, 3.2, 4.0);
                else if (navgen_has_connection(m, target_id, area->id)) { // hasReverseConnection
                    if (area->id < target_id) push_line(&layers[NAV_TWO_WAY], start, end, 3.0);
                } else {
                    push_arrow(&layers[NAV_ONE_WAY], start, end, 3.0, 4.0);
                }
            }
        }
    }
}

static double nav_now_ms(void) { return glfwGetTime() * 1000.0; } // glfwGetTime is thread-safe

// The worker: navgen + the vertex arrays (no GL)
static void nav_build_run(NavBuild *job) {
    const double t0 = nav_now_ms();
    NavGenMesh *m = navgen_build(job->map);
    job->navgen_ms = nav_now_ms() - t0;
    if (m) {
        build_layers(m, job->layers);
        job->areas = m->area_count;
        job->nodes = m->node_count;
        job->obstacle_links = m->stats.obstacle_links;
        job->ok = 1;
        navgen_free(m);
    }
    cJSON_Delete(job->map);
    job->map = NULL;
}

#ifdef NAV_THREADED
static void *nav_build_thread(void *arg) {
    nav_build_run(arg);
    pthread_mutex_lock(&g_nav.lock);
    g_nav.job_done = 1;
    pthread_mutex_unlock(&g_nav.lock);
    return NULL;
}
#endif

static void nav_build_free(NavBuild *b) {
    if (!b) return;
    for (int i = 0; i < NAV_LAYERS; i++) free(b->layers[i].v);
    cJSON_Delete(b->map);
    free(b);
}

// ------------------------------------------------------------------------- GL

static void nav_meshes_free(void) {
    for (int i = 0; i < NAV_LAYERS; i++) {
        if (g_nav.meshes[i]) {
            mesh_fini(g_nav.meshes[i]); // frees the LineMaterial
            g_nav.meshes[i] = NULL;
        }
        if (g_nav.geometry[i].vao) glDeleteVertexArrays(1, &g_nav.geometry[i].vao);
        if (g_nav.geometry[i].ebo) glDeleteBuffers(1, &g_nav.geometry[i].ebo);
        if (g_nav.vbo[i]) glDeleteBuffers(1, &g_nav.vbo[i]);
        memset(&g_nav.geometry[i], 0, sizeof(g_nav.geometry[i]));
        g_nav.vbo[i] = 0;
    }
    g_nav.shown = 0;
}

// ------------------------------------------------------------------------- the spectated bot (KrunkNative)

static void nav_bot_meshes_free(void) {
    for (int i = 0; i < NAV_BOT_LAYERS; i++) {
        if (g_bot.meshes[i]) {
            mesh_fini(g_bot.meshes[i]);
            g_bot.meshes[i] = NULL;
        }
        if (g_bot.geometry[i].vao) glDeleteVertexArrays(1, &g_bot.geometry[i].vao);
        if (g_bot.vbo[i]) glDeleteBuffers(1, &g_bot.vbo[i]);
        memset(&g_bot.geometry[i], 0, sizeof(g_bot.geometry[i]));
        g_bot.vbo[i] = 0;
        g_bot.vbo_floats[i] = 0;
    }
    if (g_bot.ibo) glDeleteBuffers(1, &g_bot.ibo);
    g_bot.ibo = 0;
    g_bot.ibo_count = 0;
}

// 0 .. count-1 (GL_LINES over sequential vertices), grown as needed
static int nav_bot_indices(const size_t count) {
    if (g_bot.ibo && g_bot.ibo_count >= count) return 1;
    size_t n = g_bot.ibo_count ? g_bot.ibo_count : 256;
    while (n < count) n *= 2;
    unsigned int *indices = malloc(n * sizeof(unsigned int));
    if (!indices) return 0;
    for (size_t k = 0; k < n; k++) indices[k] = (unsigned int) k;
    if (!g_bot.ibo) glGenBuffers(1, &g_bot.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_bot.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (n * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(indices);
    g_bot.ibo_count = n;
    return 1;
}

static int nav_bot_layer_upload(const int i, const NavGeomVerts *v) {
    const size_t count = v->n / 3;
    Geometry *g = &g_bot.geometry[i];
    if (!g_bot.meshes[i]) {
        LineMaterial *material = line_material_init();
        if (!material) return 0;
        const int hex = nav_bot_layer_hex[i];
        material->color = (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f, 1.0f};
        material->depth_test = 0;
        g->draw_mode = GL_LINES;
        glGenVertexArrays(1, &g->vao);
        glGenBuffers(1, &g_bot.vbo[i]);
        glBindVertexArray(g->vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_bot.vbo[i]);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_bot.meshes[i] = mesh_init(g, &material->base);
        if (!g_bot.meshes[i]) {
            free(material);
            return 0;
        }
    }
    if (!count) {
        g->index_count = 0;
        return 1;
    }
    if (!nav_bot_indices(count)) return 0;
    glBindBuffer(GL_ARRAY_BUFFER, g_bot.vbo[i]);
    if (g_bot.vbo_floats[i] < v->n) { // grown: a new store (GLES 3.0: glBufferData, no persistent mapping)
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (v->cap * sizeof(float)), NULL, GL_DYNAMIC_DRAW);
        g_bot.vbo_floats[i] = v->cap;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) (v->n * sizeof(float)), v->v);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    g->ebo = g_bot.ibo;
    g->index_count = (int) count;
    return 1;
}

// The packet is for the bot we follow and still fresh
static int nav_bot_current(const Client *client) {
    (void) client;
    if (!g_bot.have || !spectate_active()) return 0;
    const int uid = spectate_target_uid();
    if (uid < 0 || uid != g_bot.packet.bot_sid) return 0;
    return nav_now_ms() - g_bot.at <= NAV_BOT_STALE_MS;
}

void nav_overlay_bot_path(Client *client, const uint8_t *body, const uint16_t length) {
    (void) client;
    NetBotPathPacket packet;
    if (!bot_path_packet_decode(body, length, &packet)) return;
    if (!g_bot.have || g_bot.packet.bot_sid != packet.bot_sid) // a new bot followed: one log line (server/tests/bot_overlay_e2e.py)
        KN_LOG_VERBOSE2("[nav] bot path: sid %d state %s points %d flags 0x%x\n", packet.bot_sid, packet.state_name, packet.n, packet.flags);
    g_bot.packet = packet;
    g_bot.have = 1;
    g_bot.at = nav_now_ms();
}

int nav_overlay_bot_status(Client *client, char *out, const size_t size) {
    if (!out || !size) return 0;
    out[0] = 0;
    if (!nav_overlay_active() || !nav_bot_current(client)) return 0;
    const NetBotPathPacket *p = &g_bot.packet;
    size_t len = (size_t) snprintf(out, size, "BOT %s", p->state_name[0] ? p->state_name : "?");
    if (!(p->flags & NET_BOT_PATH_ACTIVE)) len += (size_t) snprintf(out + len, len < size ? size - len : 0, " (dead)");
    const Player *bot = client_find_player(client, p->bot_sid);
    if ((p->flags & NET_BOT_PATH_HAS_GOAL) && bot) {
        const float d = hypotf(p->goal[0] - bot->position.x, p->goal[2] - bot->position.z);
        len += (size_t) snprintf(out + len, len < size ? size - len : 0, " \xc2\xb7 goal %.0fu%s", d,
                                 (p->flags & NET_BOT_PATH_STRANDED) ? " (unreachable)" : (p->flags & NET_BOT_PATH_PARTIAL) ? " (partial route)" : "");
    }
    if (p->n >= 2) len += (size_t) snprintf(out + len, len < size ? size - len : 0, " \xc2\xb7 %d waypoint%s%s", p->n - 1, p->n == 2 ? "" : "s",
                                           (p->flags & NET_BOT_PATH_TRUNCATED) ? "+" : "");
    if (p->flags & NET_BOT_PATH_HAS_TARGET) {
        const Player *target = client_find_player(client, p->target_sid);
        len += (size_t) snprintf(out + len, len < size ? size - len : 0, " \xc2\xb7 target %s (%s)", target && target->name[0] ? target->name : "?",
                                 (p->flags & NET_BOT_PATH_SEES_TARGET) ? "visible" : "lost, searching");
    } else if (p->flags & NET_BOT_PATH_HAS_LAST_KNOWN) {
        len += (size_t) snprintf(out + len, len < size ? size - len : 0, " \xc2\xb7 last seen enemy remembered");
    }
    return 1;
}

static void nav_bot_render(Client *client) {
    if (!nav_bot_current(client)) return;
    const NetBotPathPacket *p = &g_bot.packet;
    const Player *bot = client_find_player(client, p->bot_sid);
    const Player *target = (p->flags & NET_BOT_PATH_HAS_TARGET) ? client_find_player(client, p->target_sid) : NULL;
    const vec3 bot_feet = bot ? bot->position : (vec3) {0};
    const vec3 target_feet = target ? target->position : (vec3) {0};
    if (!nav_bot_geom_build(p, bot && bot->active ? &bot_feet : NULL, target && target->active ? &target_feet : NULL, &g_bot.geom)) return;
    for (int i = 0; i < NAV_BOT_LAYERS; i++) {
        if (!nav_bot_layer_upload(i, &g_bot.geom.layers[i])) return;
        if (!g_bot.geometry[i].index_count) continue;
        Mesh *mesh = g_bot.meshes[i];
        const Scene layer = {1, &mesh};
        scene_render(&layer, &client->camera);
    }
}

// mkMesh / mkLines for every non-empty layer
static int nav_meshes_create(const NavBuild *b) {
    for (int i = 0; i < NAV_LAYERS; i++) {
        const NavVerts *v = &b->layers[i];
        const size_t count = v->n / 3;
        if (!count) continue;
        LineMaterial *material = line_material_init();
        unsigned int *indices = malloc(count * sizeof(unsigned int));
        if (!material || !indices) {
            free(material);
            free(indices);
            nav_meshes_free();
            return 0;
        }
        for (size_t k = 0; k < count; k++) indices[k] = (unsigned int) k;
        const int hex = k_layer_hex[i];
        material->color = (vec4) {(float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f,
                                  i == NAV_FILL ? NAV_FILL_OPACITY : 1.0f};
        material->depth_test = 0;
        material->base.transparent = i == NAV_FILL; // MeshBasicMaterial transparent: true
        Geometry *g = &g_nav.geometry[i];
        g->draw_mode = i == NAV_FILL ? GL_TRIANGLES : GL_LINES;
        g->index_count = (int) count;
        glGenVertexArrays(1, &g->vao);
        glGenBuffers(1, &g_nav.vbo[i]);
        glGenBuffers(1, &g->ebo);
        glBindVertexArray(g->vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_nav.vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (v->n * sizeof(float)), v->v, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *) 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        free(indices);
        g_nav.meshes[i] = mesh_init(g, &material->base);
        if (!g_nav.meshes[i]) {
            free(material);
            nav_meshes_free();
            return 0;
        }
    }
    g_nav.shown = 1;
    return 1;
}

// ------------------------------------------------------------------------- state

static NavKey nav_key_of(const Client *client) {
    const Game *game = &client->game;
    NavKey key = {0};
    key.index = -1;
    if (!game->ready || !game->map) return key;
    key.map = game->map;
    key.raw = game->map->raw_data;
    key.objects = game->map->object_count;
    key.index = game->current_map_index;
    return key;
}

static int nav_key_equal(const NavKey a, const NavKey b) { return a.map == b.map && a.raw == b.raw && a.objects == b.objects && a.index == b.index; }

static void nav_job_cancel(void) {
    if (g_nav.job) g_nav.job_cancelled = 1; // discarded when it finishes
}

static void nav_show_build(const NavBuild *b) {
    const double t0 = nav_now_ms();
    nav_meshes_free();
    if (!nav_meshes_create(b)) {
        ui_console_printf(COL_ERROR, "nav: renderer not available");
        return;
    }
    g_nav.key = b->key;
    ui_console_printf(COL_INFO, "nav: generated source-style overlay %d areas, %d nodes, %d obstacle links", b->areas, b->nodes, b->obstacle_links);
    // KrunkNative: v7 prints the line above with 0 areas and draws nothing; say why
    if (!b->areas) ui_console_printf(COL_WARN, "nav: no walkable ground under this map's spawns (nothing to draw)");
    size_t vertices = 0;
    for (int i = 0; i < NAV_LAYERS; i++) vertices += b->layers[i].n / 3;
    KN_LOG_VERBOSE2("[nav] overlay: %d areas, %d nodes, %zu vertices; navgen %.1f ms (worker), upload %.1f ms (frame), request to screen %.1f ms\n",
           b->areas, b->nodes, vertices, b->navgen_ms, nav_now_ms() - t0, b->total_ms);
}

// The build finished (main thread)
static void nav_job_finished(void) {
    NavBuild *b = g_nav.job;
    g_nav.job = NULL;
    b->total_ms = nav_now_ms() - g_nav.job_started;
    if (g_nav.job_cancelled) {
        g_nav.job_cancelled = 0;
        nav_build_free(b);
        return;
    }
    if (!b->ok) {
        ui_console_printf(COL_ERROR, "nav: buildGeneratedNavMesh failed: out of memory");
        nav_build_free(b);
        return;
    }
    nav_build_free(g_nav.cache);
    g_nav.cache = b;
    nav_show_build(b);
}

// A finished build goes through nav_job_finished; wait = 1 blocks until the running one ends.
static void nav_job_poll(const int wait) {
    if (!g_nav.job) return;
#ifdef NAV_THREADED
    if (!wait) {
        pthread_mutex_lock(&g_nav.lock);
        const int done = g_nav.job_done;
        pthread_mutex_unlock(&g_nav.lock);
        if (!done) return;
    }
    pthread_join(g_nav.thread, NULL);
    g_nav.job_done = 0;
#else
    (void) wait;
#endif
    nav_job_finished();
}

// The map's objects / xyz / spawns: all buildGeneratedNavMesh reads, copied so the worker never
// touches a map JSON the game may free.
static cJSON *nav_map_copy(const cJSON *raw) {
    cJSON *copy = cJSON_CreateObject();
    if (!copy) return NULL;
    static const char *const keys[] = {"objects", "xyz", "spawns"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(raw, keys[i]);
        if (!item) continue;
        cJSON *dup = cJSON_Duplicate(item, 1);
        if (!dup) {
            cJSON_Delete(copy);
            return NULL;
        }
        cJSON_AddItemToObject(copy, keys[i], dup);
    }
    return copy;
}

static void nav_build_start(const Client *client) {
    const NavKey key = nav_key_of(client);
    if (g_nav.job) {
        if (nav_key_equal(g_nav.job->key, key)) { // already building this map (`off` cancelled it: keep it)
            g_nav.job_cancelled = 0;
            return;
        }
        nav_job_cancel(); // another map's build: one worker at a time
        nav_job_poll(1);
    }
    if (g_nav.cache && nav_key_equal(g_nav.cache->key, key)) { // built once per map
        g_nav.cache->total_ms = 0.0;
        nav_show_build(g_nav.cache);
        return;
    }
    NavBuild *b = calloc(1, sizeof(NavBuild));
    if (!b || !(b->map = nav_map_copy(client->game.map->raw_data))) {
        free(b);
        ui_console_printf(COL_ERROR, "nav: buildGeneratedNavMesh failed: out of memory");
        return;
    }
    b->key = key;
    g_nav.job = b;
    g_nav.job_cancelled = 0;
    g_nav.job_started = nav_now_ms();
#ifdef NAV_THREADED
    if (!g_nav.lock_ready) {
        pthread_mutex_init(&g_nav.lock, NULL);
        g_nav.lock_ready = 1;
    }
    g_nav.job_done = 0;
    if (pthread_create(&g_nav.thread, NULL, nav_build_thread, b) != 0) { // no thread: build here
        nav_build_run(b);
        nav_job_finished();
    }
#else
    nav_build_run(b); // the web build has no threads
    nav_job_finished();
#endif
}

void nav_overlay_show(Client *client) {
    if (!client || !client->game.ready || !client->game.map) {
        ui_console_printf(COL_ERROR, "nav: renderer/scene not available (are you in a game?)");
        return;
    }
    if (!client->game.map->raw_data) {
        ui_console_printf(COL_ERROR, "nav: no current map data captured yet");
        return;
    }
    // The build (and the line shader's first compile) runs from the frame loop: on the web the
    // console runs inside a page event handler, where an asset fetch (ASYNCIFY) cannot suspend.
    g_nav.restore = 0;
    g_nav.show_pending = 1;
}

void nav_overlay_clear(const int log) {
    nav_job_cancel();
    nav_meshes_free();
    nav_bot_meshes_free();
    g_nav.restore = 0;
    g_nav.show_pending = 0;
    if (log) ui_console_printf(COL_INFO, "nav: overlay cleared");
}

int nav_overlay_active(void) { return g_nav.shown || g_nav.show_pending || (g_nav.job && !g_nav.job_cancelled); }

void nav_overlay_map_switch(void) {
    if (!nav_overlay_active()) return;
    const NavKey from = g_nav.shown ? g_nav.key : g_nav.job ? g_nav.job->key : (NavKey) {0};
    nav_overlay_clear(0);
    g_nav.restore = 1;
    g_nav.restore_from = from;
    g_nav.restore_at = nav_now_ms();
}

void nav_overlay_frame(Client *client) {
    const NavKey key = nav_key_of(client);
    if (g_nav.job && !g_nav.job_cancelled && !nav_key_equal(g_nav.job->key, key)) nav_job_cancel(); // built for another map
    nav_job_poll(0);
    if (g_nav.cache && !nav_key_equal(g_nav.cache->key, key)) { // another map: the cached build is stale
        nav_build_free(g_nav.cache);
        g_nav.cache = NULL;
    }
    if (g_nav.shown && !nav_key_equal(g_nav.key, key)) nav_meshes_free(); // ensureNavOverlayOwnerCurrent
    if (g_nav.show_pending) {
        g_nav.show_pending = 0;
        if (key.map && client->game.map->raw_data) nav_build_start(client);
    }
    if (g_nav.restore) {
        // scheduleNavOverlayRebuild: once the new map is up (a mode switch may keep the same map:
        // then after 300 ms); it gives up when no map comes
        const double waited = nav_now_ms() - g_nav.restore_at;
        if (waited > 3000.0) g_nav.restore = 0;
        else if (key.map && client->game.map->raw_data && (!nav_key_equal(key, g_nav.restore_from) || waited > 300.0)) {
            g_nav.restore = 0;
            nav_build_start(client);
        }
    }
}

// One scene per layer, in v7's order (scene_render's qsort is not stable on every libc, and
// with depth testing off the draw order is what puts a layer over another)
void nav_overlay_render(Client *client) {
    if (!g_nav.shown) return;
    for (int i = 0; i < NAV_LAYERS; i++) {
        if (!g_nav.meshes[i]) continue;
        Mesh *mesh = g_nav.meshes[i];
        const Scene layer = {1, &mesh};
        scene_render(&layer, &client->camera);
    }
    nav_bot_render(client); // KrunkNative: the followed bot's route / markers, over the nav layers
}

void nav_overlay_fini(void) {
    nav_job_cancel();
    nav_job_poll(1);
    nav_meshes_free();
    nav_build_free(g_nav.cache);
    g_nav.cache = NULL;
    nav_bot_meshes_free();
    nav_bot_geom_free(&g_bot.geom);
    g_bot.have = 0;
#ifdef NAV_THREADED
    if (g_nav.lock_ready) pthread_mutex_destroy(&g_nav.lock);
    g_nav.lock_ready = 0;
#endif
    line_material_fini_program();
}
