#pragma once
// Video textures of map objects (client/src/map_video.c): Krunker's "at": "<id>v" (render.js: a hidden
// <video> of user-assets/<id>/video.mp4, loop, volume 0, played on a THREE.VideoTexture shared by every
// object with that asset). KrunkNative has no video decoder: it plays user-assets/<id>/video.mp4.knv,
// the video's JPEG frames (shared/include/knv.h) that the brain's user-asset mirror makes
// (services/brain/user_assets.c), on the web and natively alike. Until the frames are in the texture is
// black (a VideoTexture before its first frame); without them (no sidecar anywhere) it shows the
// asset's texture.png when there is one, else stays black.

// The GL texture of asset `id` (its digits): one per asset for the session (textures are cached for
// the process like load_texture's), owned here until map_video_clear().
// 0 only when GL cannot make one.
unsigned map_video_texture(const char *id);
// Each frame: sidecars that arrived start playing (time 0 = their arrival), playing ones show the
// frame of `now` (seconds, any monotonic clock; looped).
void map_video_frame(double now);
// The map leaves the scene (user_asset_map_unload): decoding stops and the sidecars' bytes are freed;
// the textures stay (a new round on the same map keeps its materials). map_video_resume (the map is in
// the scene again) reads them again and restarts playback.
void map_video_park(void);
void map_video_resume(void);
// Every texture deleted (only when nothing can draw them any more).
void map_video_clear(void);
// How many videos play / fell back to their texture.png (tests, the console).
void map_video_stats(int *playing, int *fallback, int *waiting);
