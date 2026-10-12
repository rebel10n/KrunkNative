// GAME.OVERLAY call scripts (tools/canvas2d/scenes/*.txt): parsed once, replayed through
// ks_overlay_call(). Used by tools/canvas2d/canvas2d_test.c and by the client's
// KRUNKNATIVE_OVERLAY_SCENE debug hook (ks_overlay_client.c). Grammar in ks_overlay.h.
#include <ks_overlay.h>
#include <shared.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_MAX_ARGS 12

typedef struct {
    int line, outside;
    KsOvMethod method;
    int argc;
    KsOvArg args[SCENE_MAX_ARGS];
    char *text; // the line's own copy: KS_OV_STR args point into it
} SceneCall;

struct KsOverlayScene {
    KsOverlaySceneInfo info;
    SceneCall *calls;
    int ncalls, cap;
};

static const KsOverlayScene *g_resolver_scene;

static char *parse_string(char **p) {
    char *src = *p + 1, *out = *p + 1, *start = out;
    while (*src && *src != '"') {
        if (*src == '\\' && src[1]) {
            src++;
            *out++ = *src == 'n' ? '\n' : *src;
            src++;
        } else {
            *out++ = *src++;
        }
    }
    if (*src == '"') src++;
    *out = 0;
    *p = src;
    return start;
}

static void parse_directive(KsOverlayScene *s, const char *p, int *outside) {
    KsOverlaySceneInfo *in = &s->info;
    char key[32];
    if (sscanf(p, "@%31s", key) != 1) return;
    if (!strcmp(key, "size")) sscanf(p, "@size %d %d", &in->width, &in->height);
    else if (!strcmp(key, "scale")) sscanf(p, "@scale %lf", &in->scale);
    else if (!strcmp(key, "dpr")) sscanf(p, "@dpr %lf", &in->dpr);
    else if (!strcmp(key, "outside")) *outside = 1;
    else if (!strcmp(key, "font")) {
        const char *v = p + 5;
        while (*v == ' ') v++;
        snprintf(in->font, sizeof(in->font), "%s", v);
        in->font[strcspn(in->font, "\r\n")] = 0;
    }
    else if (!strcmp(key, "background")) {
        char hex[16];
        float rgba[4];
        if (sscanf(p, "@background %15s", hex) == 1 && c2d_parse_color(hex, rgba)) memcpy(in->background, rgba, sizeof(in->background));
    } else if (!strcmp(key, "image") && in->image_count < KS_OVERLAY_SCENE_MAX_IMAGES) {
        const int i = in->image_count;
        if (sscanf(p, "@image %lf %255s", &in->image_id[i], in->image_path[i]) == 2) in->image_count++;
    }
}

KsOverlayScene *ks_overlay_scene_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    KsOverlayScene *s = calloc(1, sizeof(KsOverlayScene));
    if (!s) {
        fclose(f);
        return NULL;
    }
    s->info.width = 800;
    s->info.height = 600;
    s->info.scale = 1;
    s->info.dpr = 1;
    char raw[4096];
    int line = 0, outside = 0;
    while (fgets(raw, sizeof(raw), f)) {
        line++;
        char *p = raw;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || !strncmp(p, "//", 2)) continue;
        if (*p == '@') {
            parse_directive(s, p, &outside);
            continue;
        }
        char name[64];
        int n = 0;
        if (sscanf(p, "%63s%n", name, &n) != 1) continue;
        const int m = ks_overlay_method_lookup(name);
        if (m < 0) {
            fprintf(stderr, "%s:%d: unknown GAME.OVERLAY method %s\n", path, line, name);
            continue;
        }
        if (s->ncalls == s->cap) {
            const int cap = s->cap ? s->cap * 2 : 64;
            SceneCall *calls = realloc(s->calls, (size_t) cap * sizeof(SceneCall));
            if (!calls) break;
            s->calls = calls;
            s->cap = cap;
        }
        SceneCall *c = &s->calls[s->ncalls++];
        memset(c, 0, sizeof(*c));
        c->line = line;
        c->outside = outside;
        c->method = (KsOvMethod) m;
        c->text = strdup(p + n);
        if (!c->text) break;
        char *q = c->text;
        while (c->argc < SCENE_MAX_ARGS) {
            while (*q == ' ' || *q == '\t' || *q == ',') q++;
            if (!*q || *q == '\n' || *q == '\r') break;
            KsOvArg *a = &c->args[c->argc++];
            if (*q == '"') {
                a->type = KS_OV_STR;
                a->str = parse_string(&q);
                continue;
            }
            char tok[128] = "";
            int tn = 0;
            sscanf(q, "%127[^ \t\r\n,]%n", tok, &tn);
            q += tn ? tn : 1;
            if (!strcmp(tok, "true") || !strcmp(tok, "false")) {
                a->type = KS_OV_BOOL;
                a->boolean = tok[0] == 't';
            } else if (!strcmp(tok, "null")) a->type = KS_OV_NULL;
            else if (!strcmp(tok, "undefined")) a->type = KS_OV_UNDEF;
            else if (!strcmp(tok, "{}")) a->type = KS_OV_OBJ;
            else {
                a->type = KS_OV_NUM;
                a->num = !strcmp(tok, "NaN") ? NAN : strtod(tok, NULL);
            }
        }
    }
    fclose(f);
    return s;
}

const KsOverlaySceneInfo *ks_overlay_scene_info(const KsOverlayScene *s) { return &s->info; }
int ks_overlay_scene_call_count(const KsOverlayScene *s) { return s->ncalls; }

void ks_overlay_scene_replay(const KsOverlayScene *s, const int in_render, FILE *log) {
    int inside = in_render;
    if (s->info.font[0] && ks_overlay_canvas()) c2d_set_font_css(ks_overlay_canvas(), s->info.font);
    for (int i = 0; i < s->ncalls; i++) {
        const SceneCall *c = &s->calls[i];
        if (!in_render) { // the caller is not inside render: emulate the render bracket
            if (!c->outside && !inside) {
                ks_overlay_render_begin();
                inside = 1;
            } else if (c->outside && inside) {
                ks_overlay_render_end();
                inside = 0;
            }
        }
        const KsOvResult r = ks_overlay_call(c->method, c->args, c->argc);
        if (!log) continue;
        const char *name = ks_overlay_method_name(c->method);
        if (r.type == KS_OV_RET_NUM) fprintf(log, "%d: %s -> %.3f\n", c->line, name, r.num);
        else if (r.type == KS_OV_RET_SIZE) fprintf(log, "%d: %s -> %.3f x %.3f\n", c->line, name, r.width, r.height);
        else if (r.type == KS_OV_RET_ERROR) fprintf(log, "%d: %s -> error\n", c->line, name);
    }
    if (!in_render && inside) ks_overlay_render_end();
}

static int scene_resolver(const double id, char *path, const size_t size) {
    const KsOverlaySceneInfo *in = g_resolver_scene ? &g_resolver_scene->info : NULL;
    for (int i = 0; in && i < in->image_count; i++) {
        if (in->image_id[i] == id) {
            // "assets/..." is relative to the asset root (also on the web, where it is /assets/)
            const char *p = in->image_path[i];
            if (!strncmp(p, "assets/", 7)) snprintf(path, size, "%s%s", assets_path(), p + 7);
            else snprintf(path, size, "%s", p);
            return 1;
        }
    }
    return 0;
}

void ks_overlay_scene_use_images(const KsOverlayScene *s) {
    g_resolver_scene = s;
    ks_overlay_set_image_resolver(s ? scene_resolver : NULL);
}

void ks_overlay_scene_free(KsOverlayScene *s) {
    if (!s) return;
    if (g_resolver_scene == s) ks_overlay_scene_use_images(NULL);
    for (int i = 0; i < s->ncalls; i++) free(s->calls[i].text);
    free(s->calls);
    free(s);
}
