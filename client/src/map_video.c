// Video textures of map objects (client/include/map_video.h).
//
// Parity notes (render.js map textures, assetTexture with "v"):
// - Krunker's VideoTexture follows a <video> element that starts playing when it is created; here the
//   sidecar starts at the frame it is first shown after it arrived (both: the map's load, roughly).
// - The sidecar holds at most the first 120 s of the video, at 15 fps and 512 px wide (the brain's
//   user_assets.video_*); the browser plays all of it at its own rate and size. Silent, as Krunker's
//   (volume 0).
// - Krunker shows only the video (black until it plays); the texture.png fallback for an asset whose
//   frames cannot be had is KrunkNative's.
#include <gl.h>
#include "klog.h"
#include <map_video.h>
#include <client.h>
#include <knv.h>
#include <stb_image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_VIDEO_POLL_S 0.25 // a sidecar still downloading is looked for again this often

typedef enum { MV_WAITING, MV_PLAYING, MV_FALLBACK_WAITING, MV_DONE } MapVideoState;

typedef struct {
    char id[24];
    unsigned tex;
    MapVideoState state;
    unsigned char *data; // the whole sidecar
    size_t len;
    VideoInfo info;
    const uint32_t *offsets;
    double start;        // clock of frame 0, < 0 = not started
    double next_poll;
    int64_t shown;       // frame index in the texture, -1 none
    int tex_w, tex_h;
    int fallback;        // the texture holds texture.png
    int parked;          // the map is out of the scene (map_video_park)
} MapVideo;

static MapVideo *g_videos;
static size_t g_video_count;

static char *mv_path(const MapVideo *v, const char *file) {
    char rel[96];
    snprintf(rel, sizeof(rel), "user-assets/%s/%s", v->id, file);
    return concat(assets_path(), rel);
}

static void mv_bind(const unsigned tex) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    g_active_texture = tex;
}

static void mv_upload(MapVideo *v, const unsigned char *rgba, const int w, const int h) {
    mv_bind(v->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w == v->tex_w && h == v->tex_h) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        v->tex_w = w;
        v->tex_h = h;
    }
}

// 1 present, 0 still downloading, -1 not to be had
static int mv_state_of(const char *path) {
    const int st = asset_prefetch(path);
    if (st != 1) return st;
    FILE *f = fopen(path, "rb"); // 1 also means "not managed": the file decides
    if (!f) return -1;
    fclose(f);
    return 1;
}

static void mv_fallback(MapVideo *v) {
    char *png = mv_path(v, "texture.png");
    const int st = png ? mv_state_of(png) : -1;
    if (st == 0) {
        v->state = MV_FALLBACK_WAITING;
    } else {
        v->state = MV_DONE;
        if (st == 1) {
            int w, h, n;
            stbi_set_flip_vertically_on_load_thread(1); // as load_texture
            unsigned char *rgba = stbi_load(png, &w, &h, &n, 4);
            stbi_set_flip_vertically_on_load_thread(0);
            if (rgba) {
                mv_upload(v, rgba, w, h);
                stbi_image_free(rgba);
                v->fallback = 1;
                printf("[video] %s: no video frames, showing its texture.png\n", v->id);
            }
        }
        if (!v->fallback) printf("[video] %s: no video frames and no texture.png: black\n", v->id);
    }
    free(png);
}

static void mv_poll(MapVideo *v) {
    if (v->state == MV_FALLBACK_WAITING) {
        mv_fallback(v);
        return;
    }
    char *knv = mv_path(v, "video.mp4.knv");
    const int st = knv ? mv_state_of(knv) : -1;
    if (st == 1) {
        unsigned char *data = NULL;
        size_t len = 0;
        if (!read_file(knv, &data, &len) && video_parse(data, len, &v->info, &v->offsets)) {
            v->data = data;
            v->len = len;
            v->state = MV_PLAYING;
            v->start = -1.0;
            KN_LOG_VERBOSE2("[video] %s: %u frames %dx%d at %.3g fps\n", v->id, v->info.frame_count, v->info.frame_w, v->info.frame_h,
                   (double) v->info.fps_num / (double) v->info.fps_den);
        } else {
            free(data);
            fprintf(stderr, "[video] %s: not a video sidecar\n", knv);
            mv_fallback(v);
        }
    } else if (st < 0) {
        mv_fallback(v);
    }
    free(knv);
}

unsigned map_video_texture(const char *id) {
    if (!id || !id[0]) return 0;
    for (size_t i = 0; i < g_video_count; i++)
        if (!strcmp(g_videos[i].id, id)) return g_videos[i].tex;
    MapVideo *grown = realloc(g_videos, (g_video_count + 1) * sizeof(*grown));
    if (!grown) return 0;
    g_videos = grown;
    MapVideo *v = &g_videos[g_video_count];
    memset(v, 0, sizeof(*v));
    snprintf(v->id, sizeof(v->id), "%s", id);
    v->shown = -1;
    glGenTextures(1, &v->tex);
    if (!v->tex) return 0;
    g_video_count++;
    mv_bind(v->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); // map textures: NearestFilter
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    static const unsigned char black[4] = {0, 0, 0, 255};
    mv_upload(v, black, 1, 1);
    mv_poll(v);
    return v->tex;
}

void map_video_frame(const double now) {
    for (size_t i = 0; i < g_video_count; i++) {
        MapVideo *v = &g_videos[i];
        if (v->parked) continue;
        if (v->state == MV_WAITING || v->state == MV_FALLBACK_WAITING) {
            if (now < v->next_poll) continue;
            v->next_poll = now + MAP_VIDEO_POLL_S;
            mv_poll(v);
            if (v->state != MV_PLAYING) continue;
        }
        if (v->state != MV_PLAYING) continue;
        if (v->start < 0.0) v->start = now;
        const uint32_t idx = video_frame_at(&v->info, now - v->start, 1, NULL);
        if ((int64_t) idx == v->shown) continue;
        const uint32_t a = knv_offset(v->offsets, idx), b = knv_offset(v->offsets, idx + 1);
        int w, h, n;
        stbi_set_flip_vertically_on_load_thread(1);
        unsigned char *rgba = b > a ? stbi_load_from_memory(v->data + a, (int) (b - a), &w, &h, &n, 4) : NULL;
        stbi_set_flip_vertically_on_load_thread(0);
        v->shown = idx; // a bad frame is skipped, not retried every frame
        if (!rgba) continue;
        mv_upload(v, rgba, w, h);
        stbi_image_free(rgba);
    }
}

void map_video_park(void) {
    for (size_t i = 0; i < g_video_count; i++) {
        MapVideo *v = &g_videos[i];
        if (v->state != MV_PLAYING) continue;
        free(v->data);
        v->data = NULL;
        v->offsets = NULL;
        v->state = MV_WAITING; // the sidecar is on disk: read again by the next poll after resume
        v->parked = 1;
    }
}

void map_video_resume(void) {
    for (size_t i = 0; i < g_video_count; i++) {
        g_videos[i].parked = 0;
        g_videos[i].next_poll = 0.0;
        g_videos[i].shown = -1;
    }
}

void map_video_clear(void) {
    for (size_t i = 0; i < g_video_count; i++) {
        if (g_videos[i].tex) {
            const GLuint tex = g_videos[i].tex;
            if (g_active_texture == tex) g_active_texture = 0;
            glDeleteTextures(1, &tex);
        }
        free(g_videos[i].data);
    }
    free(g_videos);
    g_videos = NULL;
    g_video_count = 0;
}

void map_video_stats(int *playing, int *fallback, int *waiting) {
    int p = 0, f = 0, w = 0;
    for (size_t i = 0; i < g_video_count; i++) {
        p += g_videos[i].state == MV_PLAYING;
        f += g_videos[i].fallback;
        w += g_videos[i].state == MV_WAITING || g_videos[i].state == MV_FALLBACK_WAITING;
    }
    if (playing) *playing = p;
    if (fallback) *fallback = f;
    if (waiting) *waiting = w;
}
