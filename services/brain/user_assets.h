#pragma once
/* services/brain/user_assets.c: GET /api/user-assets/<id>/<file>, the clients' mirror of Krunker's
 * user-asset host (community maps' textures, models, sounds, resource packs). */
struct Brain;
int user_assets_init(struct Brain *b);
void user_assets_shutdown(struct Brain *b);
