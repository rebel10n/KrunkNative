// Krunker v10.0.3 video skins: the per-video GL textures and their playback (client/include/skin_video.h).
#include <gl.h>
#include "klog.h"
#include <skin_video.h>
#include <client.h>
#include <knv.h>
#include <stb_image.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SKIN_VIDEO_POLL_S 0.25 // a sidecar still downloading is looked for again this often
#define SKIN_VIDEO_MAX_ID 4096 // ids are small (store videos 0..3, the altSkin's 2)

typedef enum { SV_WAITING, SV_PLAYING, SV_MISSING } SkinVideoState;

typedef struct {
    int video;
    unsigned tex[2];      // [0] NearestFilter, [1] LinearFilter (0 = not made yet)
    int tex_w[2], tex_h[2];
    int users;            // live materials showing it (skin_video_texture - skin_video_release)
    SkinVideoState state;
    double start;         // clock of frame 0, < 0 = not started
    double next_poll;
    int64_t shown;        // frame index in the textures, -1 none
    // native: the whole .knv sidecar
    unsigned char *data;
    size_t len;
    VideoInfo info;
    const uint32_t *offsets;
    int opened;           // web: the <video> element exists
} SkinVideo;

static SkinVideo *g_videos;
static size_t g_video_count;
static int g_disabled;
static unsigned g_frames_uploaded;

void skin_video_set_disabled(const int disabled) {
    g_disabled = disabled != 0;
}

int skin_video_disabled(void) {
    return g_disabled;
}

int skin_video_alt_skin(const int id) {
    // upgradeWeapons (deob-10.0.3.js:199770-202652) ids with altSkin {projection: true, video: 2}: all but the
    // Cupids Blaster 11 (altSkin {mid: 4}: a remodel, no video) and 17 Bomb, 19 Build Tool, 20 Grappler, 23 Slimer,
    // 25 Minigun, 26 War Machine (no altSkin). The Shotgun (5), Semi Auto (7) and Akimbo Uzi (9) altSkins add a
    // remodel (mid 0: "Nova Pump" / "Nova Semi" / "Nova Uzi", viewer_weapon_alt_skin): their video lies on it.
    static const uint32_t VIDEO_ALT_SKINS = 0x1FFFFFFFu & ~((1u << 11) | (1u << 17) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 25) | (1u << 26));
    return id >= 0 && id < 29 && (VIDEO_ALT_SKINS >> id & 1u) ? 2 : -1;
}

static void sv_bind(const unsigned tex) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    g_active_texture = tex;
}

static char *sv_path(const int video, const char *suffix) {
    char rel[96];
    snprintf(rel, sizeof(rel), "videos/video_%d.mp4%s", video, suffix);
    return concat(assets_path(), rel);
}

#ifdef __EMSCRIPTEN__
// getVideo (deob-10.0.3.js:232003-232016): the hidden element, made once per id. muted / playsInline: see skin_video.h.
// A failed download is retried once on the second asset origin (Module.krunkNativeAssetFallback, as asset_url's).
// rVFC marks each presented frame (VideoTexture, deob:45113-45119).
EM_JS(void, kn_skin_video_open, (int video, const char *url_ptr, const char *path_ptr), {
    var id = 'video_' + video;
    var v = document.getElementById(id);
    if (v) return;
    v = document.createElement('video');
    v.crossOrigin = 'anonymous';
    v.id = id;
    v.src = UTF8ToString(url_ptr);
    v.style.display = 'none';
    v.loop = true;
    v.volume = 0;
    v.muted = true;
    v.playsInline = true;
    var path = UTF8ToString(path_ptr);
    v.addEventListener('error', function () {
        var base = Module.krunkNativeAssetFallback;
        if (!base || v.__knFallback) return;
        v.__knFallback = true;
        var url = base + (path.charAt(0) === '/' ? path.substring(1) : path);
        if (Module.krunkNativeAssetVersion) url += '?v=' + encodeURIComponent(Module.krunkNativeAssetVersion);
        v.src = url;
        v.play().catch(function () {});
    });
    v.__knFresh = false;
    if ('requestVideoFrameCallback' in v) {
        var onFrame = function () { v.__knFresh = true; v.requestVideoFrameCallback(onFrame); };
        v.requestVideoFrameCallback(onFrame);
    }
    document.body.appendChild(v);
    var p = v.play();
    if (p && p.catch) p.catch(function () {
        // a page that may not play yet: the next user gesture starts it
        var go = function () { v.play().catch(function () {}); window.removeEventListener('pointerdown', go, true); window.removeEventListener('keydown', go, true); };
        window.addEventListener('pointerdown', go, true);
        window.addEventListener('keydown', go, true);
    });
});
// 1 when the element has a frame the textures do not show yet (consumed by this call).
EM_JS(int, kn_skin_video_fresh, (int video), {
    var v = document.getElementById('video_' + video);
    if (!v || v.readyState < 2 || !v.videoWidth) return 0;
    if (!('requestVideoFrameCallback' in v)) return 1; // VideoTexture.update: every frame while there is data
    if (!v.__knFresh) return 0;
    v.__knFresh = false;
    return 1;
});
EM_JS(int, kn_skin_video_width, (int video), { var v = document.getElementById('video_' + video); return v ? v.videoWidth : 0; });
EM_JS(int, kn_skin_video_height, (int video), { var v = document.getElementById('video_' + video); return v ? v.videoHeight : 0; });
// The current frame into the texture bound on unit 0 (flipY as three.js textures; the full image when the size changed).
EM_JS(void, kn_skin_video_put, (int video, int realloc_), {
    var v = document.getElementById('video_' + video);
    var gl = GL.currentContext && GL.currentContext.GLctx;
    if (!v || !gl) return;
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    if (realloc_) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, v);
    else gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, v);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
});
#endif

#ifndef __EMSCRIPTEN__
static void sv_upload(SkinVideo *v, const unsigned char *rgba, const int w, const int h) {
    for (int k = 0; k < 2; k++) {
        if (!v->tex[k]) continue;
        sv_bind(v->tex[k]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (w == v->tex_w[k] && h == v->tex_h[k]) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            v->tex_w[k] = w;
            v->tex_h[k] = h;
        }
    }
    g_frames_uploaded++;
}

// 1 present, 0 still downloading, -1 not to be had (as map_video.c)
static int sv_state_of(const char *path) {
    const int st = asset_prefetch(path);
    if (st != 1) return st;
    FILE *f = fopen(path, "rb"); // 1 also means "not managed": the file decides
    if (!f) return -1;
    fclose(f);
    return 1;
}

static void sv_poll(SkinVideo *v) {
    char *knv = sv_path(v->video, ".knv");
    const int st = knv ? sv_state_of(knv) : -1;
    if (st == 1) {
        unsigned char *data = NULL;
        size_t len = 0;
        if (!read_file(knv, &data, &len) && video_parse(data, len, &v->info, &v->offsets)) {
            v->data = data;
            v->len = len;
            v->state = SV_PLAYING;
            v->start = -1.0;
            v->shown = -1;
            KN_LOG_VERBOSE2("[video] video_%d: %u frames %dx%d at %.3g fps\n", v->video, v->info.frame_count, v->info.frame_w,
                            v->info.frame_h, (double) v->info.fps_num / (double) v->info.fps_den);
        } else {
            free(data);
            fprintf(stderr, "[video] %s: not a video sidecar\n", knv);
            v->state = SV_MISSING;
        }
    } else if (st < 0) {
        v->state = SV_MISSING;
    }
    free(knv);
}
#endif

static SkinVideo *sv_find(const int video) {
    for (size_t i = 0; i < g_video_count; i++)
        if (g_videos[i].video == video) return &g_videos[i];
    return NULL;
}

static SkinVideo *sv_get(const int video) {
    SkinVideo *v = sv_find(video);
    if (v) return v;
    SkinVideo *grown = realloc(g_videos, (g_video_count + 1) * sizeof(*grown));
    if (!grown) return NULL;
    g_videos = grown;
    v = &g_videos[g_video_count++];
    memset(v, 0, sizeof(*v));
    v->video = video;
    v->start = -1.0;
    v->shown = -1;
#ifdef __EMSCRIPTEN__
    char *path = sv_path(video, "");
    char *url = path ? asset_url(path) : NULL;
    if (url) {
        kn_skin_video_open(video, url, path);
        v->opened = 1;
        v->state = SV_PLAYING;
    } else {
        v->state = SV_MISSING;
    }
    free(url);
    free(path);
#else
    sv_poll(v);
    if (v->state == SV_MISSING) printf("[video] video_%d: no frame sidecar (videos/video_%d.mp4.knv): the regular texture\n", video, video);
#endif
    return v;
}

unsigned skin_video_texture(const int video, const int linear) {
    if (video < 0 || video > SKIN_VIDEO_MAX_ID) return 0;
    SkinVideo *v = sv_get(video);
    if (!v || v->state == SV_MISSING) return 0;
    const int k = linear ? 1 : 0;
    if (!v->tex[k]) {
        glGenTextures(1, &v->tex[k]);
        if (!v->tex[k]) return 0;
        sv_bind(v->tex[k]);
        // loadTexture: RepeatWrapping; VideoTexture: generateMipmaps false (deob-10.0.3.js:45111, :231860-231866)
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        static const unsigned char black[4] = {0, 0, 0, 255}; // a VideoTexture before its first frame
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
        v->tex_w[k] = v->tex_h[k] = 1;
        v->shown = -1; // the new texture gets the current frame on the next skin_video_frame
    }
    v->users++;
    return v->tex[k];
}

void skin_video_release(const int video) {
    SkinVideo *v = sv_find(video);
    if (v && v->users > 0) v->users--;
}

void skin_video_frame(const double now) {
    for (size_t i = 0; i < g_video_count; i++) {
        SkinVideo *v = &g_videos[i];
        if (v->state == SV_MISSING) continue;
#ifdef __EMSCRIPTEN__
        // the element plays on its own (never paused, as v10's); a frame is uploaded while a material shows it
        if (!v->users || !v->opened || !kn_skin_video_fresh(v->video)) continue;
        const int w = kn_skin_video_width(v->video), h = kn_skin_video_height(v->video);
        for (int k = 0; k < 2; k++) {
            if (!v->tex[k]) continue;
            sv_bind(v->tex[k]);
            const int realloc_ = w != v->tex_w[k] || h != v->tex_h[k];
            kn_skin_video_put(v->video, realloc_);
            v->tex_w[k] = w;
            v->tex_h[k] = h;
        }
        g_frames_uploaded++;
        (void) now;
#else
        if (v->state == SV_WAITING) {
            if (now < v->next_poll) continue;
            v->next_poll = now + SKIN_VIDEO_POLL_S;
            sv_poll(v);
            if (v->state != SV_PLAYING) continue;
        }
        if (v->start < 0.0) v->start = now; // play() when it is first shown (the browser starts at its first frame)
        if (!v->users) continue;              // no material shows it: nothing is decoded
        const uint32_t idx = video_frame_at(&v->info, now - v->start, 1, NULL);
        if ((int64_t) idx == v->shown) continue;
        const uint32_t a = knv_offset(v->offsets, idx), b = knv_offset(v->offsets, idx + 1);
        int w, h, n;
        stbi_set_flip_vertically_on_load_thread(1); // flipY, as load_texture
        unsigned char *rgba = b > a ? stbi_load_from_memory(v->data + a, (int) (b - a), &w, &h, &n, 4) : NULL;
        stbi_set_flip_vertically_on_load_thread(0);
        v->shown = idx; // a bad frame is skipped, not retried every frame
        if (!rgba) continue;
        sv_upload(v, rgba, w, h);
        stbi_image_free(rgba);
#endif
    }
}

void skin_video_clear(void) {
    for (size_t i = 0; i < g_video_count; i++) {
        for (int k = 0; k < 2; k++) {
            if (!g_videos[i].tex[k]) continue;
            const GLuint tex = g_videos[i].tex[k];
            if (g_active_texture == tex) g_active_texture = 0;
            glDeleteTextures(1, &tex);
        }
        free(g_videos[i].data);
    }
    free(g_videos);
    g_videos = NULL;
    g_video_count = 0;
}

void skin_video_stats(int *videos, int *in_use, unsigned *frames_uploaded) {
    int n = 0, u = 0;
    for (size_t i = 0; i < g_video_count; i++) {
        n += g_videos[i].tex[0] || g_videos[i].tex[1];
        u += g_videos[i].users > 0;
    }
    if (videos) *videos = n;
    if (in_use) *in_use = u;
    if (frames_uploaded) *frames_uploaded = g_frames_uploaded;
}
