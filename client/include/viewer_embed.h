#pragma once
// The store item view's 3D showcase. v7 index.js viewItem (and social.js on the hub) puts
// <iframe src='/viewer.html?nobg&showcase=<index>'> in #itemViewPop. The web build's browser
// loads that page itself (the viewer page of the web client); natively the HTML engine lays the
// iframe out as an empty box, and this draws the viewer's showcase scene (viewer_scene.c, the
// same build as the viewer page's showcase mode) into its rectangle over the composited page,
// with the iframe's transparent background (nobg). The iframe's OrbitControls drag is the
// pointer's over that box (viewer_embed_pointer): the item auto-rotates as in v7 until it is dragged.
//
// v10.0.7's Skin Manager has the same iframe in its editor (#knSmPreview here, v10_skin_manager.c), with the edited
// skin itself in the query (showcase=<JSON>, viewer.c state_from_params); its Capture Thumbnail takes the shown
// showcase as an image (viewer_embed_capture_png: the page posts capturePreview {size} to the iframe,
// deob-10.0.7.js:124566-124570, and the iframe answers previewCaptured {dataURL}, :124764-124790).
#include <client.h>

// Once per frame after html_ui_render(): opens / follows / closes the showcase as such an
// iframe appears, changes item or goes away. No-op on the web and while the viewer page runs.
void viewer_embed_frame(Client *client, float delta);
void viewer_embed_fini(void);
// The showcase on screen drawn again into a size x size RGBA image (top row first, transparent background), as the
// viewer page's canvas.toDataURL after a render; malloc'd, NULL when none is shown. Native only (NULL on the web).
unsigned char *viewer_embed_capture(int size);
// Capture Thumbnail: the shown showcase as a size x size PNG, handed to `done` (which frees it). Natively done runs
// before this returns. On the web this posts capturePreview to the #knSmPreview iframe, and done runs when its
// previewCaptured answer arrives, or never (v10's page then gives up after 5 s). 0 when there is no showcase to ask.
typedef void (*ViewerEmbedCaptured)(unsigned char *png, size_t n);
int viewer_embed_capture_png(int size, ViewerEmbedCaptured done);
// The pointer over the page (CSS px; action 0 move, 1 left press, 2 left release): a press on the showcase's box (the
// iframe the topmost element there) is the showcase's (the Skin Manager's placement tools, else the orbit drag) until
// its release. 1 when the showcase took it (the page does not see it). Native only (0 on the web).
int viewer_embed_pointer(int action, double x, double y);
// The showcase iframe's answers to its page (the Skin Manager's attachChanged {attach} / charmChanged {charm},
// deob-10.0.7.js:124749-124762): `type` and its data, as the page's message listener. One listener; NULL removes it.
typedef void (*ViewerEmbedListener)(const char *type, const cJSON *data);
void viewer_embed_listen(ViewerEmbedListener listener);
