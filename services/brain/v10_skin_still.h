/*
 * services/brain/v10_skin_still.h - a video skin in the store table, sent as its still.
 *
 * In v10.0.3 a store record with `video` set and no `asset` is a video skin: the client plays
 * /videos/video_<video>.mp4 on it (getMat, with `projection` on the mesh). KrunkNative has no video
 * asset store, so /data/skins sends such a record the way v10.0.3 already draws it with the
 * "Disable Video Skins" setting (settings.quality.noVideoSkins): `video`, `projection` and `texSrc`
 * are left out, the still stays (the skin's own texture `tex`). Its `thumbnail` is the one every
 * skin gets (v10_skin_thumb.h). Records that are not video skins, and the user-asset skins (`asset`),
 * are unchanged.
 */
#ifndef KN_BRAIN_V10_SKIN_STILL_H
#define KN_BRAIN_V10_SKIN_STILL_H

#include "cJSON.h"

/* Rewrites the store array in place. Returns the number of video skins sent as stills. */
int v10_skin_still(cJSON *skins);

#endif
