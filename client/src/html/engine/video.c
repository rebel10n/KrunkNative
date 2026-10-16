/*
 * video.c - <video> playback from offline-transcoded JPEG frame sidecars.  See
 * video.h (format: tools/ui-import/video_frames.py).
 */
#include "video.h"
#include "image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl.h>
#include <shared.h>
#include <stb_image.h>

#define VIDEO_HEADER_SIZE 32u
#define VIDEO_RETRY_S 1.0          /* a missing sidecar is looked for again (native asset fetch) */

static uint32_t rd32(const unsigned char *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static int rd16(const unsigned char *p) { return (int) ((uint32_t) p[0] | (uint32_t) p[1] << 8); }


int video_sidecar_path(const char *ui_dir, const char *url, const char *base_url, char *out, size_t cap)
{
    char key[512];
    if (!url || !*url || !out || !cap || !image_resolve_url(url, base_url, key, sizeof(key))) return 0;
    int n;
    if (!strncmp(key, "assets/", 7)) {
        const char *assets = assets_path();
        if (!assets) return 0;
        n = snprintf(out, cap, "%s%s.knv", assets, key + 7);
    } else {
        const size_t dl = ui_dir ? strlen(ui_dir) : 0;
        n = snprintf(out, cap, "%s%s%s.knv", ui_dir ? ui_dir : "", dl && ui_dir[dl - 1] != '/' ? "/" : "", key);
    }
    return n > 0 && (size_t) n < cap;
}

/* The sidecar's bytes (a managed asset is fetched on demand first, like images). */
static int read_sidecar(const char *path, unsigned char **data, size_t *len)
{
    const char *assets = assets_path();
    if (assets && !strncmp(path, assets, strlen(assets))) ensure_asset_file(path);
    return read_file(path, data, len) == 0;
}

int video_probe(const char *sidecar_path, int *natural_w, int *natural_h)
{
    unsigned char head[VIDEO_HEADER_SIZE];
    FILE *f;
    const char *assets = assets_path();
    if (!sidecar_path) return 0;
    if (assets && !strncmp(sidecar_path, assets, strlen(assets))) ensure_asset_file(sidecar_path);
    f = fopen(sidecar_path, "rb");
    if (!f) return 0;
    const size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (got != sizeof(head) || memcmp(head, "KNVF", 4) != 0 || rd32(head + 4) != 1) return 0;
    if (natural_w) *natural_w = rd16(head + 8);
    if (natural_h) *natural_h = rd16(head + 10);
    return rd16(head + 8) > 0 && rd16(head + 10) > 0;
}

/* ------------------------------------------------------------------------- */
/* players                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct VideoPlayer {
    uintptr_t key;
    char *path;
    int flags;
    unsigned char *data;       /* the whole sidecar (NULL: not loaded / missing) */
    size_t len;
    VideoInfo info;
    const uint32_t *offsets;   /* into data                                      */
    double start;              /* clock of playback time 0                       */
    double retry_at;           /* missing sidecar: next look                     */
    int64_t shown;             /* frame index in the texture, -1 none            */
    unsigned tex;
    int tex_w, tex_h;
} VideoPlayer;

struct VideoPlayers {
    VideoPlayer *p;
    int n, cap;
};

VideoPlayers *video_players_create(void)
{
    return calloc(1, sizeof(VideoPlayers));
}

static void player_free(VideoPlayer *pl)
{
    if (pl->tex) {
        GLuint t = pl->tex;
        glDeleteTextures(1, &t);
    }
    free(pl->data);
    free(pl->path);
    memset(pl, 0, sizeof(*pl));
}

void video_players_destroy(VideoPlayers *v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) player_free(&v->p[i]);
    free(v->p);
    free(v);
}

int video_players_count(const VideoPlayers *v) { return v ? v->n : 0; }

static VideoPlayer *player_find(VideoPlayers *v, uintptr_t key)
{
    for (int i = 0; i < v->n; i++)
        if (v->p[i].key == key) return &v->p[i];
    return NULL;
}

void video_players_forget(VideoPlayers *v, uintptr_t key)
{
    VideoPlayer *pl = v ? player_find(v, key) : NULL;
    if (!pl) return;
    player_free(pl);
    *pl = v->p[--v->n];
}

static void player_load(VideoPlayer *pl, double now)
{
    unsigned char *data = NULL;
    size_t len = 0;
    pl->retry_at = now + VIDEO_RETRY_S;
    if (!read_sidecar(pl->path, &data, &len)) return;
    if (!video_parse(data, len, &pl->info, &pl->offsets)) {
        fprintf(stderr, "[html_engine] video %s: not a KNVF v1 sidecar\n", pl->path);
        free(data);
        pl->retry_at = 1e300;   /* a broken file is not looked at again */
        return;
    }
    pl->data = data;
    pl->len = len;
    pl->start = now;           /* the load algorithm: autoplay starts at the first frame */
}

/* Decodes frame `idx` into the player's texture. */
static int player_show(VideoPlayer *pl, uint32_t idx)
{
    if ((int64_t) idx == pl->shown && pl->tex) return 0;
    const uint32_t o0 = rd32((const unsigned char *) pl->offsets + 4u * idx);
    const uint32_t o1 = rd32((const unsigned char *) pl->offsets + 4u * (idx + 1u));
    int w = 0, h = 0, comp = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    unsigned char *px = stbi_load_from_memory(pl->data + o0, (int) (o1 - o0), &w, &h, &comp, 4);
    if (!px) return 0;
    GLint prev_tex = 0, prev_align = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    const int fresh = !pl->tex || pl->tex_w != w || pl->tex_h != h;
    if (!pl->tex) {
        GLuint t = 0;
        glGenTextures(1, &t);
        pl->tex = t;
    }
    glBindTexture(GL_TEXTURE_2D, pl->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (fresh) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    stbi_image_free(px);
    pl->tex_w = w;
    pl->tex_h = h;
    pl->shown = idx;
    return 1;
}

static uint32_t player_index(const VideoPlayer *pl, double now, int *ended)
{
    if (!(pl->flags & VIDEO_AUTOPLAY)) {
        if (ended) *ended = 1;
        return 0;
    }
    return video_frame_at(&pl->info, now - pl->start, (pl->flags & VIDEO_LOOP) != 0, ended);
}

int video_players_frame(VideoPlayers *v, uintptr_t key, const char *sidecar_path, int flags, double now,
                        unsigned *tex, int *w, int *h)
{
    if (!v || !sidecar_path) return 0;
    VideoPlayer *pl = player_find(v, key);
    if (pl && (strcmp(pl->path, sidecar_path) != 0 || pl->flags != flags)) {
        video_players_forget(v, key);
        pl = NULL;
    }
    if (!pl) {
        if (v->n == v->cap) {
            const int cap = v->cap ? v->cap * 2 : 4;
            VideoPlayer *grown = realloc(v->p, (size_t) cap * sizeof(*grown));
            if (!grown) return 0;
            v->p = grown;
            v->cap = cap;
        }
        pl = &v->p[v->n++];
        memset(pl, 0, sizeof(*pl));
        pl->key = key;
        pl->flags = flags;
        pl->shown = -1;
        const size_t pn = strlen(sidecar_path);
        pl->path = malloc(pn + 1);
        if (!pl->path) {
            v->n--;
            return 0;
        }
        memcpy(pl->path, sidecar_path, pn + 1);
        player_load(pl, now);
    }
    if (!pl->data && now >= pl->retry_at) player_load(pl, now);
    if (!pl->data) return 0;
    player_show(pl, player_index(pl, now, NULL));
    if (!pl->tex) return 0;
    *tex = pl->tex;
    *w = pl->tex_w;
    *h = pl->tex_h;
    return 1;
}

int video_players_tick(VideoPlayers *v, double now, int (*alive)(void *user, uintptr_t key), void *user,
                       double *next_in)
{
    int loaded = 0;
    double next = -1.0;
    if (!v) return 0;
    for (int i = v->n - 1; i >= 0; i--) {
        VideoPlayer *pl = &v->p[i];
        if (alive && !alive(user, pl->key)) {
            player_free(pl);
            v->p[i] = v->p[--v->n];
            continue;
        }
        if (!pl->data) {
            if (now >= pl->retry_at) {
                player_load(pl, now);
                if (pl->data) loaded++;    /* natural size known: layout, then the paint hook shows it */
            }
            continue;
        }
        int ended = 0;
        player_show(pl, player_index(pl, now, &ended)); /* same texture: the display list samples it */
        if (!ended) {
            const double fps = (double) pl->info.fps_num / (double) pl->info.fps_den;
            const double t = (now - pl->start) * fps;
            const double in = (floor(t + 1e-6) + 1.0 - t) / fps;
            if (next < 0.0 || in < next) next = in;
        }
    }
    if (next_in) *next_in = next;
    return loaded;
}
