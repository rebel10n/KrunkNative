/*
 * test_video.c - <video> frame sidecars (video.c, tools/ui-import/video_frames.py): the .knv
 * header and offset table, playback time -> frame index (loop / ended), sidecar path
 * resolution, and decoding of the real sidecars when they are present (assets/videos,
 * assets/scares: the importer's output, not in git).
 *
 * Build: add_executable(test_video tests/test_video.c video.c image.c svg.c util.c
 *        client/src/stb_image.c shared/src/utils.c glad.c) with the engine's include dirs
 *        (no GL context is made: only the parsing / decoding paths run).  Run from the
 *        repository root.
 */
#include "video.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shared.h>
#include <stb_image.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); }
static void wr32(unsigned char *p, unsigned v) { wr16(p, v & 0xffff); wr16(p + 2, v >> 16); }

/* A KNVF v1 image of `frames` frames of `fsize` bytes each at fps_num/fps_den. */
static unsigned char *make_knv(unsigned frames, unsigned fsize, unsigned fps_num, unsigned fps_den, size_t *len)
{
    const size_t table = 32 + 4 * ((size_t) frames + 1);
    *len = table + (size_t) frames * fsize;
    unsigned char *d = calloc(1, *len);
    memcpy(d, "KNVF", 4);
    wr32(d + 4, 1);
    wr16(d + 8, 900); wr16(d + 10, 132);
    wr16(d + 12, 450); wr16(d + 14, 66);
    wr32(d + 16, fps_num); wr32(d + 20, fps_den);
    wr32(d + 24, frames);
    wr32(d + 28, (unsigned) (1000.0 * frames * fps_den / fps_num));
    for (unsigned i = 0; i <= frames; i++) wr32(d + 32 + 4 * i, (unsigned) (table + (size_t) i * fsize));
    return d;
}

static void t_parse(void)
{
    size_t len;
    VideoInfo vi;
    const uint32_t *off = NULL;
    unsigned char *d = make_knv(10, 16, 30, 1, &len);
    CHECK(video_parse(d, len, &vi, &off) && off, "a well-formed sidecar parses");
    CHECK(vi.natural_w == 900 && vi.natural_h == 132 && vi.frame_w == 450 && vi.frame_h == 66, "sizes");
    CHECK(vi.fps_num == 30 && vi.fps_den == 1 && vi.frame_count == 10, "rate and count");
    CHECK(!video_parse(d, len - 1, &vi, NULL), "a truncated file (last offset past the end) is rejected");
    CHECK(!video_parse(d, 20, &vi, NULL), "a header-only fragment is rejected");
    d[0] = 'X';
    CHECK(!video_parse(d, len, &vi, NULL), "bad magic");
    d[0] = 'K';
    wr32(d + 4, 2);
    CHECK(!video_parse(d, len, &vi, NULL), "unknown version");
    wr32(d + 4, 1);
    wr32(d + 32 + 4 * 3, 0);
    CHECK(!video_parse(d, len, &vi, NULL), "descending offsets");
    free(d);
    d = make_knv(4, 8, 0, 1, &len);
    CHECK(!video_parse(d, len, &vi, NULL), "fps 0");
    free(d);
}

static void t_frame_at(void)
{
    VideoInfo vi;
    int ended = 0;
    memset(&vi, 0, sizeof(vi));
    vi.fps_num = 30; vi.fps_den = 1; vi.frame_count = 30;   /* 1 s */
    CHECK(video_frame_at(&vi, 0.0, 0, &ended) == 0 && !ended, "t = 0: the first frame");
    CHECK(video_frame_at(&vi, -1.0, 0, NULL) == 0, "before the start: the first frame");
    CHECK(video_frame_at(&vi, 0.5, 0, &ended) == 15 && !ended, "t = 0.5 s: frame 15");
    CHECK(video_frame_at(&vi, 1.0 / 30.0, 0, NULL) == 1, "exactly on a frame boundary: that frame");
    CHECK(video_frame_at(&vi, 0.99, 0, &ended) == 29 && !ended, "the last frame is still playing");
    CHECK(video_frame_at(&vi, 1.5, 0, &ended) == 29 && ended, "past the end: the last frame stays (ended)");
    CHECK(video_frame_at(&vi, 1.5, 1, &ended) == 15 && !ended, "loop wraps");
    CHECK(video_frame_at(&vi, 10.0, 1, NULL) == 0, "loop: whole periods");
    vi.fps_num = 30000; vi.fps_den = 1001;                   /* 29.97 */
    CHECK(video_frame_at(&vi, 1.0, 0, NULL) == 29, "NTSC rate: 1 s is frame 29");
}

static void t_paths(void)
{
    char out[1024];
    CHECK(video_sidecar_path("/ui", "img/a.webm", "index.html", out, sizeof(out)) && !strcmp(out, "/ui/img/a.webm.knv"),
          "a page-relative URL: under the document root: %s", out);
    CHECK(video_sidecar_path("/ui/", "/assets/videos/limited_shop_banner.webm", "index.html", out, sizeof(out))
          && strstr(out, "videos/limited_shop_banner.webm.knv") && !strstr(out, "/ui/"),
          "/assets/...: the game asset tree: %s", out);
    CHECK(!video_sidecar_path("/ui", "", "index.html", out, sizeof(out)), "an empty URL");
    CHECK(!video_sidecar_path("/ui", "img/a.webm", "index.html", out, 8), "too small a buffer");
}

/* The importer's sidecars, when present: every frame table entry decodes as a JPEG of the
 * stored size (first, middle, last), and the probe reads the natural size. */
static void t_real(const char *rel, int nat_w, int nat_h)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s%s.knv", assets_path(), rel);
    unsigned char *data = NULL;
    size_t len = 0;
    if (read_file(path, &data, &len) != 0) {
        printf("skip %s (no sidecar: python3 -I tools/ui-import/video_frames.py)\n", path);
        return;
    }
    VideoInfo vi;
    const uint32_t *off = NULL;
    int w = 0, h = 0;
    CHECK(video_parse(data, len, &vi, &off), "%s parses", rel);
    CHECK(video_probe(path, &w, &h) && w == nat_w && h == nat_h, "%s natural size %dx%d (got %dx%d)", rel, nat_w, nat_h, w, h);
    CHECK(vi.natural_w == nat_w && vi.natural_h == nat_h, "%s header natural size", rel);
    const uint32_t picks[3] = { 0, vi.frame_count / 2, vi.frame_count - 1 };
    for (int i = 0; i < 3 && off; i++) {
        const unsigned char *t = (const unsigned char *) off + 4u * picks[i];
        const uint32_t o0 = (uint32_t) t[0] | (uint32_t) t[1] << 8 | (uint32_t) t[2] << 16 | (uint32_t) t[3] << 24;
        const uint32_t o1 = (uint32_t) t[4] | (uint32_t) t[5] << 8 | (uint32_t) t[6] << 16 | (uint32_t) t[7] << 24;
        int fw = 0, fh = 0, comp = 0;
        unsigned char *px = stbi_load_from_memory(data + o0, (int) (o1 - o0), &fw, &fh, &comp, 4);
        CHECK(px && fw == vi.frame_w && fh == vi.frame_h, "%s frame %u decodes at %dx%d (got %dx%d)", rel, picks[i],
              vi.frame_w, vi.frame_h, fw, fh);
        stbi_image_free(px);
    }
    free(data);
}

int main(void)
{
    t_parse();
    t_frame_at();
    t_paths();
    t_real("videos/limited_shop_banner.webm", 900, 132);   /* v10 Eterno Shop header */
    t_real("videos/tutorial/basics.mp4", 1280, 720);        /* v10 Guide "Basics" */
    t_real("scares/1.mp4", 384, 480);                       /* v10 jumpScare(1) */
    printf("test_video: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
