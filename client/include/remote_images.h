#pragma once
// Remote images for the native HTML engine (engine/image.h "remote images"): <img src="https://...">
// and url(https://...) on the page, Krunker's user-assets thumbnails among them. The engine asks
// remote_images.c for the URL's file; it answers from its disk cache, or downloads the URL on the
// media lane (backend_http_get_media) and the image pops in when the file lands
// (asset_arrival_bump -> image_cache_retry_missing). The web build's browser loads remote images
// itself: every function is a no-op there.
//
// Limits: http:// and https:// only; a body of at most REMOTE_IMAGE_MAX_BYTES that stb_image can
// read (PNG, JPEG, GIF, BMP, ...) at most REMOTE_IMAGE_MAX_SIDE px a side; REMOTE_IMAGE_TIMEOUT_MS per
// request, REMOTE_IMAGE_MAX_QUEUED requests on the lane at once (the rest wait their turn), at most
// REMOTE_IMAGE_MAX_URLS distinct URLs per run. A failed URL is not asked again for
// REMOTE_IMAGE_RETRY_S.
//
// Disk cache: KRUNKNATIVE_REMOTE_IMAGE_CACHE, else $XDG_CACHE_HOME/krunknative/remote-images, else
// ~/.cache/krunknative/remote-images (%LOCALAPPDATA%\krunknative\remote-images on Windows); one file
// per URL, named by its 64-bit FNV-1a hash. A file older than REMOTE_IMAGE_MAX_AGE_S is still shown
// and fetched again in the background (the next run shows the new one).
//
// --render-scenario never reaches the network: state.remoteImages {url: file} answers those URLs from
// local files and every other remote URL is refused (remote_images_scenario).
#include <cJSON.h>

#define REMOTE_IMAGE_MAX_BYTES (2u << 20)
#define REMOTE_IMAGE_MAX_SIDE 4096
#define REMOTE_IMAGE_TIMEOUT_MS 10000
#define REMOTE_IMAGE_MAX_QUEUED 8
#define REMOTE_IMAGE_MAX_URLS 1024
#define REMOTE_IMAGE_RETRY_S 300
#define REMOTE_IMAGE_MAX_AGE_S (7 * 24 * 3600)

void remote_images_init(void);                    // startup: the engine's fetcher (html_ui_set_remote_image_fetcher)
void remote_images_scenario(const cJSON *map);   // --render-scenario state.remoteImages {url: local file}
const char *remote_images_cache_dir(void);       // "" when none (web, no HOME)
