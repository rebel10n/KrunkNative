// render_view / render_multiview / render_topdown: KrunkNative's own renderer, headless.
//
// File projects: client_bin runs as a subprocess (KRUNKNATIVE_HEADLESS=1: no window, an EGL
// pbuffer on the GPU or llvmpipe) on a temporary copy of the map with the camera pinned
// (KRUNKNATIVE_CAMERA / FOV / ORTHO / NO_FOG / HIDE_UI, client/src/debug_capture.c) and writes
// one PNG. GL never enters kn_editor_mcp. Live editor projects render in the editor process
// through the bus (render op) with the editor's own renderer.
#define _GNU_SOURCE
#include "jsonw.h"
#include "mcp.h"
#include "objops.h"
#include "proc.h"
#include "schema.h"
#include "util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name;
    double pos[3];
    double yaw, pitch; // radians, Krunker camera convention
    double fov_deg;
    double ortho;      // half height, 0 = perspective
    double far;
    int width, height;
    int no_fog;
} View;

// forward = (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch) (client/src/camera.c)
static void look_at(View *v, const double pos[3], const double target[3]) {
    double d[3] = {target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]};
    const double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    memcpy(v->pos, pos, sizeof(v->pos));
    if (len < 1e-9) return;
    for (int k = 0; k < 3; k++) d[k] /= len;
    v->pitch = asin(fmax(-1.0, fmin(1.0, d[1])));
    v->yaw = atan2(-d[0], -d[2]);
}

static void forward_of(const double yaw, const double pitch, double f[3]) {
    f[0] = -sin(yaw) * cos(pitch);
    f[1] = sin(pitch);
    f[2] = -cos(yaw) * cos(pitch);
}

static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

// Scene bounds: the given ids; else the visible objects (+ spawns). core: objects whose centres
// lie inside the 4th..96th percentile box of all centres (maps often scatter skyline / backdrop
// pieces far from the playable area), always including the spawns.
static int scene_bounds_mode(Doc *doc, const int *ids, const int n, const int core, double mn[3], double mx[3]) {
    for (int k = 0; k < 3; k++) {
        mn[k] = 1e30;
        mx[k] = -1e30;
    }
    const int total = doc_object_count(doc);
    double lo[3] = {-1e30, -1e30, -1e30}, hi[3] = {1e30, 1e30, 1e30};
    if (!n && core && total >= 20) {
        double *c[3];
        int m = 0;
        for (int k = 0; k < 3; k++) c[k] = malloc(sizeof(double) * (size_t) total);
        for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
            if (!obj_visible(o)) continue;
            double a[3], b[3];
            obj_bounds(doc, o, a, b);
            for (int k = 0; k < 3; k++) c[k][m] = (a[k] + b[k]) / 2;
            m++;
        }
        if (m >= 20) {
            for (int k = 0; k < 3; k++) {
                qsort(c[k], (size_t) m, sizeof(double), cmp_d);
                lo[k] = c[k][(int) (m * 0.04)];
                hi[k] = c[k][(int) (m * 0.96)];
            }
        }
        for (int k = 0; k < 3; k++) free(c[k]);
    }
    int found = 0;
    for (const cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        if (n) {
            int hit = 0;
            for (int i = 0; i < n && !hit; i++) hit = ids[i] == o->valueint;
            if (!hit) continue;
        } else if (!obj_visible(o)) continue;
        double a[3], b[3];
        obj_bounds(doc, o, a, b);
        if (!n) {
            int inside = 1;
            for (int k = 0; k < 3; k++) {
                const double cc = (a[k] + b[k]) / 2;
                if (cc < lo[k] || cc > hi[k]) inside = 0;
            }
            if (!inside) continue;
            // a huge floor / backdrop must not set the frame alone: clip it to the percentile box
            for (int k = 0; k < 3; k++) {
                if (lo[k] > -1e29 && a[k] < lo[k] - (hi[k] - lo[k]) * 0.1) a[k] = lo[k] - (hi[k] - lo[k]) * 0.1;
                if (hi[k] < 1e29 && b[k] > hi[k] + (hi[k] - lo[k]) * 0.1) b[k] = hi[k] + (hi[k] - lo[k]) * 0.1;
            }
        }
        for (int k = 0; k < 3; k++) {
            if (a[k] < mn[k]) mn[k] = a[k];
            if (b[k] > mx[k]) mx[k] = b[k];
        }
        found++;
    }
    if (!n) {
        cJSON *form = doc_map_form(doc);
        const cJSON *s;
        cJSON_ArrayForEach(s, js_get(form, "spawns")) {
            double p[3];
            if (!js_vec3(s, p)) continue;
            for (int k = 0; k < 3; k++) {
                if (p[k] < mn[k]) mn[k] = p[k];
                if (p[k] + (k == 1 ? 11 : 0) > mx[k]) mx[k] = p[k] + (k == 1 ? 11 : 0);
            }
            found++;
        }
        cJSON_Delete(form);
    }
    if (!found) {
        for (int k = 0; k < 3; k++) {
            mn[k] = -50;
            mx[k] = 50;
        }
    }
    return found;
}

static int g_framing_core = 1;

static int scene_bounds(Doc *doc, const int *ids, const int n, double mn[3], double mx[3]) {
    return scene_bounds_mode(doc, ids, n, g_framing_core, mn, mx);
}

static void framing_arg(const cJSON *args) { g_framing_core = strcmp(js_str(args, "framing", "core"), "all") != 0; }

static char *render_one(Ctx *ctx, const char *map_path, const char *tmp, const View *v, char *err, const size_t errsz) {
    char client[PATH_MAX];
    if (proc_find_client(ctx, client, sizeof(client)) != 0) {
        snprintf(err, errsz, "client_bin not found under %s (build it with `make`, or pass --client PATH)", ctx->root);
        return NULL;
    }
    char out_png[PATH_MAX + 64], e_cam[256], e_fov[64], e_size[64], e_shot[PATH_MAX + 64], e_cfg[PATH_MAX + 64], e_ortho[64], e_far[64];
    snprintf(out_png, sizeof(out_png), "%s/%s.png", tmp, v->name);
    unlink(out_png);
    snprintf(e_cam, sizeof(e_cam), "KRUNKNATIVE_CAMERA=%.4f,%.4f,%.4f,%.6f,%.6f", v->pos[0], v->pos[1], v->pos[2], v->pitch, v->yaw);
    snprintf(e_fov, sizeof(e_fov), "KRUNKNATIVE_FOV=%.3f", v->fov_deg);
    snprintf(e_size, sizeof(e_size), "KRUNKNATIVE_WINDOW_SIZE=%dx%d", v->width, v->height);
    snprintf(e_shot, sizeof(e_shot), "KRUNKNATIVE_SCREENSHOT=%s", out_png);
    snprintf(e_cfg, sizeof(e_cfg), "XDG_CONFIG_HOME=%s/cfg", tmp);
    snprintf(e_ortho, sizeof(e_ortho), "KRUNKNATIVE_ORTHO=%.4f", v->ortho);
    snprintf(e_far, sizeof(e_far), "KRUNKNATIVE_CAMERA_FAR=%.1f", v->far > 0 ? v->far : 6000.0);
    char *env[] = {"KRUNKNATIVE_HEADLESS=1", "KRUNKNATIVE_HIDE_UI=1", "KRUNKNATIVE_CAPTURE_FRAMES=8", "KRUNKNATIVE_AUDIO=0",
                   "KRUNKNATIVE_NO_BROWSER=1", "KRUNKNATIVE_DISCORD=0", e_cam, e_fov, e_size, e_shot, e_cfg, e_ortho, e_far,
                   v->no_fog ? "KRUNKNATIVE_NO_FOG=1" : "KRUNKNATIVE_NO_FOG=0", NULL};
    char *argv[] = {client, "--headless", "--offline", (char *) map_path, NULL};
    ProcResult pr;
    if (proc_run(ctx, argv, env, ctx->root, ctx->render_timeout_s, &pr) != 0) {
        snprintf(err, errsz, "could not start client_bin");
        return NULL;
    }
    log_lines("render", LOG_DEBUG, pr.output);
    size_t n = 0;
    char *png = file_read(out_png, &n);
    if (!png || n < 8) {
        free(png);
        const char *tail = pr.output ? pr.output + (strlen(pr.output) > 600 ? strlen(pr.output) - 600 : 0) : "";
        snprintf(err, errsz, "client_bin produced no image (exit %d%s, %.1f s): %s", pr.exit_code, pr.timed_out ? ", timed out" : "", pr.seconds, tail);
        proc_result_free(&pr);
        return NULL;
    }
    log_msg(LOG_DEBUG, "render", "%s %dx%d in %.2f s", v->name, v->width, v->height, pr.seconds);
    proc_result_free(&pr);
    char *b64 = base64_encode((const unsigned char *) png, n, NULL);
    free(png);
    unlink(out_png);
    return b64;
}

// temp dir + the project's map form written for client_bin
static int prepare(Project *p, char *tmp, const size_t tmpsz, char *map_path, const size_t mpsz, char *err, const size_t errsz) {
    snprintf(tmp, tmpsz, "/tmp/kn-editor-mcp-render-XXXXXX");
    if (!mkdtemp(tmp)) {
        snprintf(err, errsz, "cannot create a temp dir");
        return -1;
    }
    snprintf(map_path, mpsz, "%s/map.json", tmp);
    cJSON *m = doc_map_form(p->doc);
    char *text = jsonw_print(m, JSONW_COMPACT);
    cJSON_Delete(m);
    const int rc = text ? file_write(map_path, text, strlen(text)) : -1;
    free(text);
    if (rc != 0) {
        snprintf(err, errsz, "cannot write the temporary map");
        return -1;
    }
    return 0;
}

static void cleanup(const char *tmp) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/map.json", tmp);
    unlink(path);
    // cfg/ keeps client settings files: remove the tree without a shell
    snprintf(path, sizeof(path), "%s/cfg/krunknative", tmp);
    char cmdpath[PATH_MAX + 64];
    const char *files[] = {"settings.json", "client_settings.json", "storage.json", "keybinds.json", NULL};
    for (int i = 0; files[i]; i++) {
        snprintf(cmdpath, sizeof(cmdpath), "%s/%s", path, files[i]);
        unlink(cmdpath);
    }
    rmdir(path);
    snprintf(path, sizeof(path), "%s/cfg", tmp);
    rmdir(path);
    rmdir(tmp);
}

static void size_args(const cJSON *args, int *w, int *h, const int dw, const int dh) {
    *w = js_int(args, "width", dw);
    *h = js_int(args, "height", dh);
    if (*w < 64) *w = 64;
    if (*h < 64) *h = 64;
    if (*w > 2048) *w = 2048;
    if (*h > 2048) *h = 2048;
}

static cJSON *view_json(const View *v) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "view", v->name);
    cJSON_AddItemToObject(o, "position", js_vec3_new(v->pos));
    cJSON_AddNumberToObject(o, "yaw_deg", js_round(v->yaw * 180 / M_PI, 2));
    cJSON_AddNumberToObject(o, "pitch_deg", js_round(v->pitch * 180 / M_PI, 2));
    if (v->ortho > 0) cJSON_AddNumberToObject(o, "ortho_half_height", js_round(v->ortho, 2));
    else cJSON_AddNumberToObject(o, "fov_deg", v->fov_deg);
    cJSON_AddNumberToObject(o, "width", v->width);
    cJSON_AddNumberToObject(o, "height", v->height);
    return o;
}

static cJSON *live_camera(const View *v) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddItemToObject(c, "pos", js_vec3_new(v->pos));
    cJSON_AddNumberToObject(c, "yaw", v->yaw);
    cJSON_AddNumberToObject(c, "pitch", v->pitch);
    cJSON_AddNumberToObject(c, "fov", v->fov_deg);
    if (v->ortho > 0) cJSON_AddNumberToObject(c, "ortho", v->ortho);
    return c;
}

static int live_render(Ctx *ctx, Project *p, const char *mode, const View *v, const cJSON *focus, ToolResult *r, cJSON *meta) {
    char err[512];
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "mode", mode);
    cJSON_AddNumberToObject(a, "width", v->width);
    cJSON_AddNumberToObject(a, "height", v->height);
    if (!strcmp(mode, "view")) cJSON_AddItemToObject(a, "camera", live_camera(v));
    else cJSON_AddTrueToObject(a, "split");
    cJSON_AddBoolToObject(a, "no_fog", v->no_fog);
    cJSON_AddTrueToObject(a, "hide_helpers");
    if (focus) cJSON_AddItemToObject(a, "focus", cJSON_Duplicate(focus, 1));
    cJSON *res = live_call(ctx, p->id, "render", a, 120000, err, sizeof(err));
    if (!res) {
        cJSON_Delete(meta);
        return tr_error(r, "%s", err);
    }
    const cJSON *images = js_get(res, "images");
    int count = 0;
    if (cJSON_IsArray(images)) {
        const cJSON *im;
        cJSON_ArrayForEach(im, images) {
            const char *b64 = js_str(im, "png_base64", NULL);
            if (!b64) continue;
            tr_text(r, "%s view", js_str(im, "view", "?"));
            tr_image(r, b64, "image/png");
            count++;
        }
    } else if (js_str(res, "png_base64", NULL)) {
        tr_image(r, js_str(res, "png_base64", ""), "image/png");
        count++;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(res, "png_base64");
    cJSON_DeleteItemFromObjectCaseSensitive(res, "images");
    cJSON_AddItemToObject(meta, "editor", res);
    cJSON_AddNumberToObject(meta, "images", count);
    cJSON_AddStringToObject(meta, "renderer", "live editor");
    tr_json(r, meta);
    if (!count) r->is_error = 1;
    return 0;
}

static int t_render_view(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    framing_arg(args);
    View v = {.name = "view", .fov_deg = js_num(args, "fov", 70), .no_fog = !js_bool(args, "fog", 1)};
    size_args(args, &v.width, &v.height, 1024, 576);
    double pos[3], target[3], mn[3], mx[3];
    int n = 0;
    int *ids = js_get(args, "ids") ? tool_ids(js_get(args, "ids"), &n) : NULL;
    scene_bounds(p->doc, ids, n, mn, mx);
    free(ids);
    const double c[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
    const double radius = 0.5 * sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) + (mx[2] - mn[2]) * (mx[2] - mn[2]));
    const int has_pos = js_vec3(js_get(args, "position"), pos), has_target = js_vec3(js_get(args, "target"), target);
    if (has_pos && has_target) look_at(&v, pos, target);
    else if (has_pos) {
        memcpy(v.pos, pos, sizeof(pos));
        v.yaw = js_num(args, "yaw", 0) * M_PI / 180;
        v.pitch = js_num(args, "pitch", 0) * M_PI / 180;
    } else {
        // frame the ids / the map from the yaw / pitch angles (default a 3/4 view)
        v.yaw = js_num(args, "yaw", 45) * M_PI / 180;
        v.pitch = js_num(args, "pitch", -30) * M_PI / 180;
        double f[3];
        forward_of(v.yaw, v.pitch, f);
        const double dist = fmax(radius, 5) / sin(v.fov_deg * M_PI / 360) * 1.05;
        const double* t = has_target ? target : c;
        for (int k = 0; k < 3; k++) v.pos[k] = t[k] - f[k] * dist;
        v.far = dist + radius * 2 + 100;
    }
    if (cJSON_IsNumber(js_get(args, "ortho"))) v.ortho = js_num(args, "ortho", 0);
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddItemToObject(meta, "camera", view_json(&v));
    if (p->kind == PROJ_LIVE) return live_render(ctx, p, "view", &v, NULL, r, meta);
    char tmp[PATH_MAX], map_path[PATH_MAX + 32], err[1024];
    if (prepare(p, tmp, sizeof(tmp), map_path, sizeof(map_path), err, sizeof(err)) != 0) {
        cJSON_Delete(meta);
        return tr_error(r, "%s", err);
    }
    char *b64 = render_one(ctx, map_path, tmp, &v, err, sizeof(err));
    cleanup(tmp);
    if (!b64) {
        cJSON_Delete(meta);
        return tr_error(r, "%s", err);
    }
    tr_image(r, b64, "image/png");
    free(b64);
    cJSON_AddStringToObject(meta, "renderer", "client_bin (headless)");
    tr_json(r, meta);
    return 0;
}

static int t_render_multiview(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    framing_arg(args);
    int w, h;
    size_args(args, &w, &h, 640, 400);
    int n = 0;
    int *ids = js_get(args, "ids") ? tool_ids(js_get(args, "ids"), &n) : NULL;
    double mn[3], mx[3];
    scene_bounds(p->doc, ids, n, mn, mx);
    free(ids);
    const double c[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
    const double ext[3] = {fmax(mx[0] - mn[0], 4), fmax(mx[1] - mn[1], 4), fmax(mx[2] - mn[2], 4)};
    const double radius = 0.5 * sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    const double aspect = (double) w / h;
    const int no_fog = !js_bool(args, "fog", 0);
    View views[4];
    memset(views, 0, sizeof(views));
    // iso: perspective 3/4 view
    views[0] = (View) {.name = "iso", .fov_deg = 50, .width = w, .height = h, .no_fog = no_fog};
    views[0].yaw = M_PI / 4;
    views[0].pitch = -35.264 * M_PI / 180;
    double f[3];
    forward_of(views[0].yaw, views[0].pitch, f);
    const double dist = radius / sin(25 * M_PI / 180) * 1.02;
    for (int k = 0; k < 3; k++) views[0].pos[k] = c[k] - f[k] * dist;
    views[0].far = dist + radius * 2 + 200;
    // front (looking -Z), top (looking -Y, screen up -Z), right (looking -X): orthographic
    static const char *names[3] = {"front", "top", "right"};
    for (int i = 1; i < 4; i++) {
        View *v = &views[i];
        *v = (View) {.name = names[i - 1], .fov_deg = 50, .width = w, .height = h, .no_fog = 1};
        double sw, sh, depth_half;
        double pos[3] = {c[0], c[1], c[2]};
        if (i == 1) {
            sw = ext[0];
            sh = ext[1];
            depth_half = ext[2] / 2;
            pos[2] = mx[2] + 50;
            v->yaw = 0;
            v->pitch = 0;
        } else if (i == 2) {
            sw = ext[0];
            sh = ext[2];
            depth_half = ext[1] / 2;
            pos[1] = mx[1] + 50;
            v->yaw = 0;
            v->pitch = -M_PI / 2;
        } else {
            sw = ext[2];
            sh = ext[1];
            depth_half = ext[0] / 2;
            pos[0] = mx[0] + 50;
            v->yaw = M_PI / 2;
            v->pitch = 0;
        }
        memcpy(v->pos, pos, sizeof(pos));
        v->ortho = fmax(sh / 2, sw / 2 / aspect) * 1.06;
        v->far = 50 + depth_half * 2 + 100;
    }
    cJSON *meta = cJSON_CreateObject();
    cJSON *vs = cJSON_AddArrayToObject(meta, "views");
    for (int i = 0; i < 4; i++) cJSON_AddItemToArray(vs, view_json(&views[i]));
    cJSON *b = cJSON_AddObjectToObject(meta, "bounds");
    cJSON_AddItemToObject(b, "min", js_vec3_new(mn));
    cJSON_AddItemToObject(b, "max", js_vec3_new(mx));
    if (p->kind == PROJ_LIVE) {
        const cJSON *focus = js_get(args, "ids");
        return live_render(ctx, p, "multiview", &views[0], focus, r, meta);
    }
    char tmp[PATH_MAX], map_path[PATH_MAX + 32], err[1024];
    if (prepare(p, tmp, sizeof(tmp), map_path, sizeof(map_path), err, sizeof(err)) != 0) {
        cJSON_Delete(meta);
        return tr_error(r, "%s", err);
    }
    int ok = 0;
    for (int i = 0; i < 4; i++) {
        char *b64 = render_one(ctx, map_path, tmp, &views[i], err, sizeof(err));
        if (!b64) {
            cleanup(tmp);
            cJSON_Delete(meta);
            return tr_error(r, "%s view: %s", views[i].name, err);
        }
        tr_text(r, "%s view%s", views[i].name, i ? " (orthographic)" : " (perspective)");
        tr_image(r, b64, "image/png");
        free(b64);
        ok++;
    }
    cleanup(tmp);
    cJSON_AddNumberToObject(meta, "images", ok);
    cJSON_AddStringToObject(meta, "renderer", "client_bin (headless)");
    tr_json(r, meta);
    return 0;
}

static int t_render_topdown(Ctx *ctx, const cJSON *args, ToolResult *r) {
    Project *p = tool_project(ctx, args, r);
    if (!p) return 0;
    framing_arg(args);
    double mn[3], mx[3];
    scene_bounds(p->doc, NULL, 0, mn, mx);
    double cx = cJSON_IsNumber(js_get(args, "x")) ? js_num(args, "x", 0) : (mn[0] + mx[0]) / 2;
    double cz = cJSON_IsNumber(js_get(args, "z")) ? js_num(args, "z", 0) : (mn[2] + mx[2]) / 2;
    double ww = js_num(args, "width", mx[0] - mn[0] + 20), wd = js_num(args, "depth", mx[2] - mn[2] + 20);
    if (ww < 1) ww = 1;
    if (wd < 1) wd = 1;
    double ppu = js_num(args, "pixels_per_unit", 0);
    if (ppu <= 0) ppu = fmin(1024.0 / ww, 1024.0 / wd);
    int w = (int) lround(ww * ppu), h = (int) lround(wd * ppu);
    if (w > 2048 || h > 2048) {
        const double s = fmin(2048.0 / w, 2048.0 / h);
        w = (int) (w * s);
        h = (int) (h * s);
    }
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    const double top = cJSON_IsNumber(js_get(args, "y_max")) ? js_num(args, "y_max", 0) : mx[1] + 20;
    const double bottom = cJSON_IsNumber(js_get(args, "y_min")) ? js_num(args, "y_min", 0) : mn[1] - 20;
    View v = {.name = "topdown", .fov_deg = 50, .width = w, .height = h, .no_fog = !js_bool(args, "fog", 0)};
    v.pos[0] = cx;
    v.pos[1] = top;
    v.pos[2] = cz;
    v.pitch = -M_PI / 2;
    v.ortho = fmax(wd / 2, ww / 2 / ((double) w / h));
    v.far = top - bottom + 1;
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddItemToObject(meta, "camera", view_json(&v));
    cJSON *rect = cJSON_AddObjectToObject(meta, "rect");
    cJSON_AddNumberToObject(rect, "x", cx);
    cJSON_AddNumberToObject(rect, "z", cz);
    cJSON_AddNumberToObject(rect, "width", ww);
    cJSON_AddNumberToObject(rect, "depth", wd);
    cJSON_AddNumberToObject(rect, "pixels_per_unit", js_round((double) w / ww, 4));
    cJSON_AddStringToObject(meta, "orientation", "screen right = +X, screen up = -Z");
    if (p->kind == PROJ_LIVE) return live_render(ctx, p, "view", &v, NULL, r, meta);
    char tmp[PATH_MAX], map_path[PATH_MAX + 32], err[1024];
    if (prepare(p, tmp, sizeof(tmp), map_path, sizeof(map_path), err, sizeof(err)) != 0) {
        cJSON_Delete(meta);
        return tr_error(r, "%s", err);
    }
    char *b64 = render_one(ctx, map_path, tmp, &v, err, sizeof(err));
    cleanup(tmp);
    if (!b64) {
        cJSON_Delete(meta);
        return tr_error(r, "%s", err);
    }
    tr_image(r, b64, "image/png");
    free(b64);
    tr_json(r, meta);
    return 0;
}

// playtest.c: screenshot from a spawn
char *render_spawn_shot(Ctx *ctx, Project *p, const double pos[3], const double yaw, char *err, const size_t errsz) {
    View v = {.name = "spawn", .fov_deg = 75, .width = 800, .height = 450};
    v.pos[0] = pos[0];
    v.pos[1] = pos[1] + 11;
    v.pos[2] = pos[2];
    v.yaw = yaw;
    v.pitch = -0.1;
    char tmp[PATH_MAX], map_path[PATH_MAX + 32];
    if (prepare(p, tmp, sizeof(tmp), map_path, sizeof(map_path), err, errsz) != 0) return NULL;
    char *b64 = render_one(ctx, map_path, tmp, &v, err, errsz);
    cleanup(tmp);
    return b64;
}

const ToolDef g_tools_render[] = {
    {"render_view", "Render a view",
     "Render the map with KrunkNative's renderer (headless client_bin; live projects: the editor's renderer) and return a PNG. "
     "Camera: position + target, or position + yaw/pitch (degrees), or none: frames ids (or the whole map) from yaw/pitch "
     "(default 45 / -30). fov degrees (default 70), ortho = orthographic half height, fog:false disables fog, width/height "
     "(default 1024x576, max 2048).",
     S({"type":"object","properties":{PROJECT_ARG,"position":{"type":"array","items":{"type":"number"}},"target":{"type":"array","items":{"type":"number"}},"yaw":{"type":"number"},"pitch":{"type":"number"},"fov":{"type":"number"},"ortho":{"type":"number"},"ids":{"type":"array","items":{"type":"integer"}},"framing":{"type":"string","enum":["core","all"]},"fog":{"type":"boolean"},"width":{"type":"integer"},"height":{"type":"integer"}}}),
     1, 0, 1, 0, t_render_view},
    {"render_multiview", "Render 4 views",
     "Four images framed on ids (or the whole map), like Unity's scene multiview: iso (perspective 3/4), front (orthographic, "
     "looking -Z), top (orthographic, looking down, screen up = -Z), right (orthographic, looking -X). Fog off unless fog:true. "
     "width/height per image (default 640x400).",
     S({"type":"object","properties":{PROJECT_ARG,"ids":{"type":"array","items":{"type":"integer"}},"framing":{"type":"string","enum":["core","all"],"description":"without ids: core (default) frames the playable area, all every object"},"fog":{"type":"boolean"},"width":{"type":"integer"},"height":{"type":"integer"}}}),
     1, 0, 1, 0, t_render_multiview},
    {"render_topdown", "Render top-down rect",
     "Orthographic top-down capture of a world rectangle (like Unity's Capture2DScene): centre x/z, width (X) / depth (Z) in "
     "world units (default: the whole map), pixels_per_unit (default fits 1024 px), y_min/y_max clip. Screen right = +X, up = -Z.",
     S({"type":"object","properties":{PROJECT_ARG,"x":{"type":"number"},"z":{"type":"number"},"width":{"type":"number"},"depth":{"type":"number"},"pixels_per_unit":{"type":"number"},"y_min":{"type":"number"},"y_max":{"type":"number"},"framing":{"type":"string","enum":["core","all"]},"fog":{"type":"boolean"}}}),
     1, 0, 1, 0, t_render_topdown},
    {0},
};
