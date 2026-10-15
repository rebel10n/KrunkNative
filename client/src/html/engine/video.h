/*
 * video.h - <video> playback for the native HTML engine, without a video decoder.
 *
 * The few videos Krunker v10.0.2's page plays (the Eterno Shop header
 * videos/limited_shop_banner.webm, the Guide's videos/tutorial/basics.mp4, the
 * jumpScare(1) scares/1.mp4) are transcoded offline into "<file>.knv" sidecars
 * next to them (tools/ui-import/video_frames.py: a header and baseline JPEG
 * frames), which the stb_image already in the tree decodes one frame at a time
 * as playback reaches it.  The web build plays the real file in the browser
 * (web_dom.c) and never uses this module.
 *
 * Semantics (HTML media elements, as the page uses them): the source is the
 * `src` attribute, else the first <source src> child; `autoplay` starts
 * playback as soon as the clip is loaded (also after a later `src` change: the
 * load algorithm restarts it), `loop` wraps at the end, otherwise the last
 * frame stays (ended).  Without autoplay the first frame is shown, paused.
 * Natively videos are silent (no audio track is decoded).  A missing sidecar
 * paints the `poster` image when there is one, else nothing (a video with no
 * frame is transparent, like Chrome before the first frame).  Before the
 * sidecar is known the box is 300 x 150; afterwards the source video's size
 * (the header's natural size) is its intrinsic size.
 *
 * Time comes from the caller (the engine clock, which --render-scenario
 * virtualises), so frame sequences are reproducible.
 *
 * GL: video_players_frame/_tick upload into one GL_TEXTURE_2D per player
 * (RGBA8, opaque, so premultiplied as is; LINEAR, MAX_LEVEL 0, CLAMP_TO_EDGE);
 * the texture binding and GL_UNPACK_ALIGNMENT are restored.  The GL context
 * must be current; textures are freed by video_player_forget / _destroy.
 */
#ifndef KN_VIDEO_H
#define KN_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { VIDEO_AUTOPLAY = 1, VIDEO_LOOP = 2 };

/* Header of a parsed .knv sidecar. */
typedef struct VideoInfo {
    int natural_w, natural_h;   /* the source video's size (intrinsic size)     */
    int frame_w, frame_h;       /* stored frames                                 */
    uint32_t fps_num, fps_den;
    uint32_t frame_count;
    uint32_t duration_ms;
} VideoInfo;

/* Parses a .knv image held in memory (not copied).  1 on success. */
int video_parse(const unsigned char *data, size_t len, VideoInfo *info, const uint32_t **offsets);
/* Frame index shown at `t` seconds of playback (looping or clamped). */
uint32_t video_frame_at(const VideoInfo *info, double t, int loop, int *ended);

/* The sidecar path of a media URL as written in the page: `url` resolved against
 * `base_url` like an image (image_resolve_url; "/assets/..." = the game asset
 * tree), plus ".knv".  `ui_dir` is the document root.  0 when rejected. */
int video_sidecar_path(const char *ui_dir, const char *url, const char *base_url, char *out, size_t cap);
/* Natural size from the sidecar header (reads only the header).  0 when missing. */
int video_probe(const char *sidecar_path, int *natural_w, int *natural_h);

typedef struct VideoPlayers VideoPlayers;

VideoPlayers *video_players_create(void);
void video_players_destroy(VideoPlayers *v);

/* The current frame of the player for element `key` playing `sidecar_path`
 * (created on first call; a different path or flags restarts it at `now`).
 * Returns 1 with the texture and its size, 0 while there is no frame. */
int video_players_frame(VideoPlayers *v, uintptr_t key, const char *sidecar_path, int flags, double now,
                        unsigned *tex, int *w, int *h);
/* Advances every player to `now` (decoding and uploading only frames that
 * changed, into the same texture: a display list that holds it shows the new
 * frame at its next submit); players whose `alive` callback returns 0 are
 * dropped.  Returns the number of players whose sidecar just loaded (their
 * natural size and first frame are new: relayout and repaint).  `next_in` (may be NULL) receives
 * the seconds until the next frame change of a playing video (< 0: none). */
int video_players_tick(VideoPlayers *v, double now, int (*alive)(void *user, uintptr_t key), void *user,
                       double *next_in);
/* Drops the player of `key` (its texture too). */
void video_players_forget(VideoPlayers *v, uintptr_t key);
int video_players_count(const VideoPlayers *v);

#ifdef __cplusplus
}
#endif

#endif /* KN_VIDEO_H */
