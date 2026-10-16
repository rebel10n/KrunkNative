#pragma once
// The .knv video sidecar: a header and baseline JPEG frames that stb_image decodes one at a time,
// KrunkNative's way of playing a video without a video decoder (tools/ui-import/video_frames.py writes
// the page's; services/brain/user_assets.c a map's video asset). Used by the HTML engine's <video>
// (client/src/html/engine/video.c) and map video textures (client/src/map_video.c). Pure.
//
// Format (little-endian):
//   "KNVF"  u32 version (1)
//   u16 natural_w, natural_h      the source video's size
//   u16 frame_w, frame_h          the stored frames' size
//   u32 fps_num, fps_den          frame rate of the stored frames
//   u32 frame_count, duration_ms
//   u32 offsets[frame_count + 1]  byte offsets of each JPEG from the file start (last = file size)
//   JPEG frames
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNV_HEADER_SIZE 32u
#define KNV_MAX_FRAMES 100000u

typedef struct VideoInfo {
    int natural_w, natural_h;   // the source video's size (intrinsic size)
    int frame_w, frame_h;       // stored frames
    uint32_t fps_num, fps_den;
    uint32_t frame_count;
    uint32_t duration_ms;
} VideoInfo;

// Parses a .knv image held in memory (not copied). 1 on success. offsets: frame_count + 1 entries,
// little-endian u32 (read them with knv_offset: no alignment is assumed).
int video_parse(const unsigned char *data, size_t len, VideoInfo *info, const uint32_t **offsets);
// Frame index shown at `t` seconds of playback (looping or clamped).
uint32_t video_frame_at(const VideoInfo *info, double t, int loop, int *ended);
// Byte offset i of a parsed sidecar's table.
uint32_t knv_offset(const uint32_t *offsets, uint32_t i);
// The 32-byte header for these values (the offset table and frames follow it).
void knv_write_header(unsigned char out[KNV_HEADER_SIZE], const VideoInfo *info);

#ifdef __cplusplus
}
#endif
