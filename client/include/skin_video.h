#pragma once
// Krunker v10.0.3 video skins (client/src/skin_video.c): the `video` texture of a store item (the
// Deep Space and Glitch dyes, the Mysterious Orb melee) and of the upgraded weapons' altSkin, and
// the Quality setting noVideoSkins ("Disable Video Skins") that turns them off.
//
// v10.0.3 (deob-10.0.3.js):
// - getMat (:232037-232050): an item with `video` and no `asset` takes "/videos/video_<n>" as its
//   texture, then loadTexture (:231859-231889) puts a VideoTexture of getVideo(item) in the map:
//   RepeatWrapping, repeat (repeatX || 1, repeatY || 1); without `projection` min/mag filter
//   tFilter || NearestFilter (weapons and melees pass LinearFilter: :194628, :194706), with it the
//   VideoTexture's own LinearFilter (:45106-45110, 1006). `projection` maps it in screen space
//   (:232252-232266, basic.frag projection_uv) on a DoubleSide material.
// - getVideo (:232003-232016): one hidden <video id="video_<n>"> per id, src videos/video_<n>.mp4
//   (UTILS asset URL), crossOrigin anonymous, loop, volume 0, play() when it is created. It is never
//   paused or removed: every material with that id shows the same playback.
// - VideoTexture (:45106-45148): no mipmaps; needsUpdate on every presented frame
//   (requestVideoFrameCallback, else whenever readyState >= HAVE_CURRENT_DATA).
// - noVideoSkins (settings :237521-237536, val false, needsRestart, perf true; renderer flag
//   :231033): getMat's first step (:231704-231710, :232038) drops `video`, `projection` and `texSrc`
//   from any material options with a video that are not a user asset, so the item shows its
//   regular (model) texture, a dye only its colours. It is read when a material is made: meshes
//   built before a change keep what they had ("Restart required").
//
// KrunkNative plays the same files: natively from videos/video_<n>.mp4.knv, the video's JPEG
// frames (shared/include/knv.h, `make video-frames`), decoded with the stb_image already in the
// tree (no video decoder); on the web from a hidden <video> element as v10 does, uploaded into the
// GL texture per presented frame. Differences, deliberate: natively a frame is decoded only while
// a live material shows that video (skin_video_texture / skin_video_release); the playback clock
// still runs from the video's creation, so it is where v10's always-playing element would be. The
// web element is also muted (volume 0 alone does not let a page play before a user gesture; v10
// plays it after the click that entered the game, the KN menu preview can come first) and
// playsInline (iOS).

// The GL texture of video `video` (videos/video_<n>.mp4), one per (video, filter) for the session,
// owned here. linear: LinearFilter (projection or a tFilter LinearFilter), else NearestFilter.
// Counts one user: pair it with skin_video_release (skin_fx does). Black until the first frame;
// 0 when GL cannot make one (the caller keeps its texture).
unsigned skin_video_texture(int video, int linear);
// A material showing `video` went away (skin_fx_forget).
void skin_video_release(int video);
// Each frame: the playing videos show the frame of `now` (seconds, gltf_clock(); looped).
void skin_video_frame(double now);
// noVideoSkins (client_apply_render_setting): read when a material is made.
void skin_video_set_disabled(int disabled);
int skin_video_disabled(void);
// The altSkin video of upgradeWeapons[id] (deob-10.0.3.js:199770-202652: {projection: true,
// video: 2, outline: 0, outlineS: 1.05} on every upgraded weapon with an altSkin but the Cupids
// Blaster, a remodel), -1 for none.
int skin_video_alt_skin(int upgraded_weapon_id);
// Every texture deleted (shutdown).
void skin_video_clear(void);
// Videos with a texture / decoding (native: frames uploaded so far), for tests and the console.
void skin_video_stats(int *videos, int *in_use, unsigned *frames_uploaded);
