// .knv video sidecars (shared/include/knv.h).
#include <knv.h>

#include <math.h>
#include <string.h>

static uint32_t rd32(const unsigned char *p) { return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
static int rd16(const unsigned char *p) { return (int) ((uint32_t) p[0] | (uint32_t) p[1] << 8); }
static void wr32(unsigned char *p, const uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}
static void wr16(unsigned char *p, const int v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
}

int video_parse(const unsigned char *data, const size_t len, VideoInfo *info, const uint32_t **offsets) {
    VideoInfo vi;
    if (!data || len < KNV_HEADER_SIZE || memcmp(data, "KNVF", 4) != 0 || rd32(data + 4) != 1) return 0;
    vi.natural_w = rd16(data + 8);
    vi.natural_h = rd16(data + 10);
    vi.frame_w = rd16(data + 12);
    vi.frame_h = rd16(data + 14);
    vi.fps_num = rd32(data + 16);
    vi.fps_den = rd32(data + 20);
    vi.frame_count = rd32(data + 24);
    vi.duration_ms = rd32(data + 28);
    if (vi.natural_w <= 0 || vi.natural_h <= 0 || vi.frame_w <= 0 || vi.frame_h <= 0 || !vi.fps_num || !vi.fps_den ||
        !vi.frame_count || vi.frame_count > KNV_MAX_FRAMES)
        return 0;
    const size_t table = KNV_HEADER_SIZE + 4u * ((size_t) vi.frame_count + 1u);
    if (len < table) return 0;
    // offsets ascending, inside the file; read as bytes (no alignment assumption)
    uint32_t prev = (uint32_t) table;
    for (uint32_t i = 0; i <= vi.frame_count; i++) {
        const uint32_t o = rd32(data + KNV_HEADER_SIZE + 4u * i);
        if (o < prev || o > len) return 0;
        prev = o;
    }
    if (info) *info = vi;
    if (offsets) *offsets = (const uint32_t *) (const void *) (data + KNV_HEADER_SIZE);
    return 1;
}

uint32_t video_frame_at(const VideoInfo *info, const double t, const int loop, int *ended) {
    if (ended) *ended = 0;
    if (!info || !info->frame_count) return 0;
    if (!(t > 0.0)) return 0;
    const double f = floor(t * (double) info->fps_num / (double) info->fps_den + 1e-6);
    if (loop) return (uint32_t) fmod(f, (double) info->frame_count);
    if (f >= (double) info->frame_count - 1.0) {
        if (ended) *ended = f >= (double) info->frame_count;
        return info->frame_count - 1;
    }
    return (uint32_t) f;
}

uint32_t knv_offset(const uint32_t *offsets, const uint32_t i) {
    return rd32((const unsigned char *) offsets + 4u * i);
}

void knv_write_header(unsigned char out[KNV_HEADER_SIZE], const VideoInfo *info) {
    memcpy(out, "KNVF", 4);
    wr32(out + 4, 1);
    wr16(out + 8, info->natural_w);
    wr16(out + 10, info->natural_h);
    wr16(out + 12, info->frame_w);
    wr16(out + 14, info->frame_h);
    wr32(out + 16, info->fps_num);
    wr32(out + 20, info->fps_den);
    wr32(out + 24, info->frame_count);
    wr32(out + 28, info->duration_ms);
}
