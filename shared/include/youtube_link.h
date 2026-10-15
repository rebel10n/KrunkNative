#pragma once
// A map's YouTube video (published_maps.map_video, the editor's "Youtube Link (Optional)" field).
// Pure C, shared by kn_brain (services/brain/creations.c stores the video id when a map is
// published) and the client (client/src/ui/social_ui.c links the stored video from the map's
// featured card and its page).
//
// Krunker v10.0.2 keeps the video id and embeds https://www.youtube.com/embed/<id> (map_videolink,
// v10:213292); its link cleaner (v10:25541) accepts youtu.be/<id> and any youtube.com /
// youtube-nocookie.com URL whose id follows a non-word character. KrunkNative has no iframe, so the
// map shows a link to https://www.youtube.com/watch?v=<id> instead; the id is checked here, so the
// link never carries anything but an 11-character video id.
#include <stddef.h>

#define KN_YOUTUBE_ID_LEN 11

// The video id of a link or a bare id: "dQw4w9WgXcQ", "https://youtu.be/dQw4w9WgXcQ",
// "youtube.com/watch?v=dQw4w9WgXcQ&t=1", "https://www.youtube.com/embed/dQw4w9WgXcQ", /shorts/,
// /live/, /v/, m. / music. hosts, youtube-nocookie.com. out gets the id (NUL-terminated, cap >=
// 12). 1 = found, 0 = not a YouTube video (out = "").
int kn_youtube_id(const char *link, char *out, size_t cap);

// The watch link of a stored map_video value (an id, or a link stored before ids were kept):
// "https://www.youtube.com/watch?v=<id>". 1 = written, 0 = no video (out = "").
int kn_youtube_watch_url(const char *stored, char *out, size_t cap);
