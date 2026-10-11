// Live third-person preview of the selected class for the HTML menu.
//
// Krunker's menu renders the selected class into `classPreviewCanvas` with a
// dedicated three.js scene (index.js: `dJ()` builds a preview player with
// `players.jLrR6(player, false, true)` and the render loop steps it with
// `players.Te7(dF, delta * 0.015, true)` + `updateMsh(dF, true)`). Our HTML page
// has no WebGL canvas of its own: the UI logic reads where its preview box is
// (menu_send_preview_rect in menu_ui.c, html_dom_get_rect) and we draw the same
// third-person PlayerMesh into that rectangle of the framebuffer, after the world
// and before html_ui_render() so the page composites on top.
//
// Everything here is client renderer state; the preview Player is a private copy
// that never joins client->game.players.
#include <gl.h>
#include <client.h>
#include <html_ui.h>
#include <ui_logic.h> // menu_preview_backdrop_alpha
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Krunker's preview scene (index.js dJ() / render loop) -----------------
// new PerspectiveCamera(15, 1, 0.1, 100); position (-10, 17, -29);
// lookAt(0, playerHeight * 0.8, 0); aspect = canvas 660x350 (the page's box).
#define PREVIEW_FOV ((float) M_PI * 15.0f / 180.0f)
#define PREVIEW_NEAR 0.1f
#define PREVIEW_FAR 100.0f
#define PREVIEW_CAMERA_X (-10.0f)
#define PREVIEW_CAMERA_Y 17.0f
#define PREVIEW_CAMERA_Z (-29.0f)
#define PREVIEW_LOOK_AT_HEIGHT 0.8f
// scene.add(new AmbientLight(0x97A1A8)); DirectionalLight(0xF2F7FC, 0.5) at (10, 17, -10).
#define PREVIEW_AMBIENT_COLOR 0x97A1A8
#define PREVIEW_AMBIENT_INTENSITY 1.0f
#define PREVIEW_LIGHT_COLOR 0xF2F7FC
#define PREVIEW_LIGHT_INTENSITY 0.5f
#define PREVIEW_LIGHT_X 10.0f
#define PREVIEW_LIGHT_Y 17.0f
#define PREVIEW_LIGHT_Z (-10.0f)
// Krunker's preview canvas is transparent and sits above the 50% black menu
// backdrop (#instructionHolder), so the scene behind the model stays dimmed. Our
// page cuts an undimmed hole at the rect instead, so we dim it ourselves first.
// The alpha comes from menu_preview_backdrop_alpha() (0.5; 0.875 under the K.P.D popup).
// The preview player is created with init(0, 0, 0, "preview") and never turned:
// yaw 0 faces -Z, i.e. towards the camera, turned slightly to its left.
#define PREVIEW_YAW 0.0f
// Krunker: stepVal += delta_ms * 0.015 * stepMlt(0.25) * stepDir, flipping at +-PI/2.
#define PREVIEW_STEP_SPEED (1000.0f * 0.015f * 0.25f)
// Debug hooks (environment variables, read once):
// - KRUNKNATIVE_PREVIEW_TEST=1 forces the rectangle below (CSS px) while the menu
//   is open, so the C side can be checked without the page sending previewRect.
// - KRUNKNATIVE_PREVIEW_DUMP=<file.ppm> writes the framebuffer right after the
//   preview draw (before the page is composited) once, PREVIEW_DUMP_FRAME frames in.
// - KRUNKNATIVE_PREVIEW_WORLD=1 also keeps a copy of the third-person model in the
//   world scene in front of the idle menu camera, animated like an in-game player
//   (player_update_meshes(.., 0), map lighting), to compare with other players.
#define PREVIEW_DUMP_FRAME 60
#define PREVIEW_WORLD_DISTANCE 30.0f
#define PREVIEW_TEST_RECT_X 1000.0f
#define PREVIEW_TEST_RECT_Y 200.0f
#define PREVIEW_TEST_RECT_W 240.0f
#define PREVIEW_TEST_RECT_H 400.0f

static void preview_player_free(Scene *scene, Player *player) {
    if (!player) return;

    if (player->mesh) {
        if (scene) scene_remove_player_mesh(scene, player->mesh, player->loadout_size);
        player_meshes_fini(player);
    }

    // Same teardown as client_clear_players() for a game player.
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

static void preview_player_destroy(Client *client) {
    ClassPreview *preview = &client->class_preview;

    preview_player_free(preview->scene, preview->player);
    preview->player = NULL;
    preview->class_index = -1;
    preview->map = NULL;
}

// dJ(): the preview wears the player's own skin colour and picks; the K.P.D popup's dI (Officer Buddy)
// overrides hat / back / dye / face with the police outfit (shoPolicePop: back 1487, hat 1890, dye 1891, face 1842).
static PlayerCosmetics preview_cosmetics(void) {
    PlayerCosmetics cosmetics;
    ui_loadout_cosmetics(&cosmetics);

    if (ui_police_active()) {
        cosmetics.back = 1487;
        cosmetics.hat = 1890;
        cosmetics.dye = 1891;
        cosmetics.face = 1842;
    }

    return cosmetics;
}

// dJ's hq / meleeIndex: the saved weapon skins of the class's slots and the melee pick (Random shows none).
static PlayerSkins preview_skins(const int class_index) {
    PlayerSkins skins;
    ui_loadout_preview_skins(class_index, &skins);
    return skins;
}

static int preview_cosmetics_changed(const Player *player) {
    const PlayerCosmetics want = preview_cosmetics();
    const PlayerSkins want_skins = preview_skins(player->class_index);
    return memcmp(&player->cosmetics, &want, sizeof(want)) != 0 || memcmp(&player->skins, &want_skins, sizeof(want_skins)) != 0;
}

// A private third-person player of the menu class, like Krunker's preview `dF`
// (init(0,0,0,"preview"), setClass, pY2(dF, 0) and swapTime = 0), added to `scene`.
static Player *preview_player_new(Client *client, Scene *scene) {
    Player *player = player_init(&client->game);
    if (!player) return NULL;

    player->uid = -1; // player_update_meshes() anchors uid -1 (Krunker's preview sid) at player->position
    player->class_index = client->menu_class_index;
    player->cosmetics = preview_cosmetics();
    player->skins = preview_skins(player->class_index);
    player_spawn(player); // loadout, ammo and weapon for the class (also picks a map spawn: reset below)

    if (!player->loadout_size || !player->weapon) {
        preview_player_free(NULL, player);
        return NULL;
    }

    memset(&player->position, 0, sizeof(player->position));
    memset(&player->last_position, 0, sizeof(player->last_position));
    memset(&player->velocity, 0, sizeof(player->velocity));
    player->direction.x = 0.0f;
    player->direction.y = 0.0f;
    player->step_val = 0.0f;
    player->step_chase = 0.0f;
    player->idle_anim = 0.0f;

    player_generate_meshes(player, 0); // third person, like other players

    if (!player->mesh) {
        preview_player_free(NULL, player);
        return NULL;
    }

    player_swap_weapon(player, 0, 1, 1, 1); // only the primary weapon's arms are visible
    player->swap_timer = 0.0f; // dF.swapTime = 0: no swap-in animation (it would pitch the upper body by -PI/4 forever)
    player_update_meshes(player, 1);
    scene_add_player_mesh(scene, player->mesh, player->loadout_size);
    return player;
}

static int preview_player_create(Client *client) {
    ClassPreview *preview = &client->class_preview;

    if (!preview->scene) {
        preview->scene = scene_init();
        if (!preview->scene) return 0;
    }

    preview->player = preview_player_new(client, preview->scene);
    if (!preview->player) return 0;

    preview->class_index = client->menu_class_index;
    preview->map = client->game.map;
    preview->step_dir = 1;
    return 1;
}

// Krunker's preview loop: Te7() walks the step cycle in place, idleAnim advances.
static void preview_animate(Player *player, int *step_dir, const float delta) {
    player->step_val += delta * PREVIEW_STEP_SPEED * (float) *step_dir;

    if ((player->step_val >= (float) M_PI / 2.0f && *step_dir == 1) ||
        (player->step_val <= (float) -M_PI / 2.0f && *step_dir == -1)) {
        *step_dir = -*step_dir;
    }

    player->step_chase += (player->step_val - player->step_chase) * 0.15f;
    player->idle_anim += game_constants.idle_anim_speed * delta;
}

// Krunker's preview camera: fixed position, lookAt the chest, 15 degree lens.
static void preview_frame(Client *client) {
    ClassPreview *preview = &client->class_preview;
    Camera *camera = &preview->camera;

    const vec3 target = {0.0f, game_constants.player_height * PREVIEW_LOOK_AT_HEIGHT, 0.0f};

    camera->fov = PREVIEW_FOV;
    camera->zoom = 1.0f;
    camera->near = PREVIEW_NEAR;
    camera->far = PREVIEW_FAR;
    camera->position = (vec3) {PREVIEW_CAMERA_X, PREVIEW_CAMERA_Y, PREVIEW_CAMERA_Z};

    vec3 dir = {target.x - camera->position.x, target.y - camera->position.y, target.z - camera->position.z};
    const float length = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir.x /= length;
    dir.y /= length;
    dir.z /= length;

    // Camera forward is (-sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch)) (camera.c).
    camera->rotation.x = asinf(dir.y);
    camera->rotation.y = atan2f(-dir.x, -dir.z);
    camera->rotation.z = 0.0f;

    Player *player = preview->player;
    player->direction.x = 0.0f;
    player->direction.y = PREVIEW_YAW;
}

// Swap g_render_lighting for Krunker's preview scene lights around the draw.
static void preview_lighting_apply(RenderLighting *saved) {
    *saved = g_render_lighting;

    vec3 light = {PREVIEW_LIGHT_X, PREVIEW_LIGHT_Y, PREVIEW_LIGHT_Z};
    const float length = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    light.x /= length;
    light.y /= length;
    light.z /= length;

    g_render_lighting.ambient_color = hex_to_vec(PREVIEW_AMBIENT_COLOR);
    g_render_lighting.ambient_intensity = PREVIEW_AMBIENT_INTENSITY;
    g_render_lighting.light_color = hex_to_vec(PREVIEW_LIGHT_COLOR);
    g_render_lighting.light_intensity = PREVIEW_LIGHT_INTENSITY;
    g_render_lighting.light_direction = light; // basic.frag: direction towards the light
    g_render_lighting.enabled = 1;
    g_render_lighting.fog_enabled = 0; // the preview is not part of the map
}

void client_class_preview_set_rect(Client *client, const int visible, const float x, const float y, const float width, const float height, const float dpr) {
    ClassPreview *preview = &client->class_preview;

    preview->has_rect = 1;
    preview->visible = visible;
    preview->x = x;
    preview->y = y;
    preview->width = width;
    preview->height = height;
    preview->dpr = dpr > 0.0f ? dpr : 1.0f;
}

// KRUNKNATIVE_PREVIEW_WORLD: a third-person dummy in the world scene, standing
// PREVIEW_WORLD_DISTANCE in front of the idle menu camera and facing it.
static void preview_world_dummy_tick(Client *client, const float delta) {
    static Player *dummy;
    static int step_dir = 1;
    static int class_index = -1;
    static const void *map;

    if (!html_ui_is_menu_open() || !client->game.ready || !client->game.map) return;

    if (dummy && (class_index != client->menu_class_index || map != client->game.map || preview_cosmetics_changed(dummy))) {
        preview_player_free(client->scene, dummy);
        dummy = NULL;
    }

    if (!dummy) {
        dummy = preview_player_new(client, client->scene);
        if (!dummy) return;
        class_index = client->menu_class_index;
        map = client->game.map;
    }

    const Camera *camera = &client->camera;
    const vec3 forward = {
        -sinf(camera->rotation.y) * cosf(camera->rotation.x),
        sinf(camera->rotation.x),
        -cosf(camera->rotation.y) * cosf(camera->rotation.x),
    };

    dummy->position.x = camera->position.x + forward.x * PREVIEW_WORLD_DISTANCE;
    dummy->position.y = camera->position.y + forward.y * PREVIEW_WORLD_DISTANCE - game_constants.player_height * 0.6f;
    dummy->position.z = camera->position.z + forward.z * PREVIEW_WORLD_DISTANCE;
    dummy->direction.y = camera->rotation.y + (float) M_PI;

    preview_animate(dummy, &step_dir, delta);
    player_update_meshes(dummy, 0); // in-game third-person path
}

static void preview_dump(const char *path, const int width, const int height) {
    // GL_RGBA/GL_UNSIGNED_BYTE: the one read-back format OpenGL ES guarantees (KN_GLES)
    unsigned char *pixels = malloc((size_t) width * height * 4);
    FILE *file = pixels ? fopen(path, "wb") : NULL;

    if (file) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        fprintf(file, "P6\n%d %d\n255\n", width, height);
        for (int row = height - 1; row >= 0; row--) {
            const unsigned char *src = pixels + (size_t) row * width * 4;
            for (int x = 0; x < width; x++) fwrite(src + (size_t) x * 4, 1, 3, file);
        }
        fclose(file);
        printf("[preview] framebuffer written to %s\n", path);
    }

    free(pixels);
}

void client_class_preview_render(Client *client) {
    ClassPreview *preview = &client->class_preview;

    static int test_rect = -1;
    static int world_dummy;
    static const char *dump_path;
    static int frames;

    if (test_rect < 0) {
        test_rect = getenv("KRUNKNATIVE_PREVIEW_TEST") != NULL;
        world_dummy = getenv("KRUNKNATIVE_PREVIEW_WORLD") != NULL;
        dump_path = getenv("KRUNKNATIVE_PREVIEW_DUMP");
    }

    if (test_rect) {
        client_class_preview_set_rect(client, 1, PREVIEW_TEST_RECT_X, PREVIEW_TEST_RECT_Y, PREVIEW_TEST_RECT_W, PREVIEW_TEST_RECT_H, 1.0f);
    }

    const double now = glfwGetTime();
    float delta = preview->last_time > 0.0 ? (float) (now - preview->last_time) : 0.0f;
    preview->last_time = now;
    if (delta > 0.1f) delta = 0.1f;

    if (world_dummy) preview_world_dummy_tick(client, delta); // drawn by the next frame's world pass

    if (!html_ui_is_menu_open() || !preview->has_rect || !preview->visible) return;
    if (preview->width <= 0.0f || preview->height <= 0.0f) return;
    if (!client->game.ready || !client->game.map) return; // meshes need map config and weapons

    if (preview->player && (preview->class_index != client->menu_class_index || preview->map != client->game.map || preview_cosmetics_changed(preview->player))) {
        preview_player_destroy(client);
    }

    if (!preview->player && !preview_player_create(client)) return;

    // CSS px -> framebuffer px. Natively CSS px == window px (the page is rendered at
    // the framebuffer size), so scale by framebuffer/window for HiDPI; on the web
    // the canvas is sized in device pixels, i.e. CSS px * devicePixelRatio.
    int fb_width, fb_height;
    glfwGetFramebufferSize(client->window, &fb_width, &fb_height);
    if (fb_width <= 0 || fb_height <= 0) return;

    float scale_x = preview->dpr;
    float scale_y = preview->dpr;

#ifndef __EMSCRIPTEN__
    int window_width, window_height;
    glfwGetWindowSize(client->window, &window_width, &window_height);

    if (window_width > 0 && window_height > 0) {
        scale_x = (float) fb_width / (float) window_width;
        scale_y = (float) fb_height / (float) window_height;
    }
#endif

    const int rect_x = (int) lroundf(preview->x * scale_x);
    const int rect_top = (int) lroundf(preview->y * scale_y);
    const int rect_width = (int) lroundf(preview->width * scale_x);
    const int rect_height = (int) lroundf(preview->height * scale_y);
    const int rect_y = fb_height - rect_top - rect_height; // GL viewports are bottom-up

    if (rect_width <= 0 || rect_height <= 0) return;

    preview_animate(preview->player, &preview->step_dir, delta);
    preview_frame(client);
    player_update_meshes(preview->player, 1);

    // Dim the world inside the box like Krunker's menu backdrop does (UI coords are
    // framebuffer pixels from the top-left; ui_update() ran with the full viewport).
    // Leave the destination alpha alone: on the web the WebGL canvas has an alpha
    // channel that the browser composites (premultiplied) over the page, so a
    // blended quad writing alpha 0.75 would let the page background through and
    // lighten the box instead of darkening it.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    ui_fill_rect(client->ui, (vec4) {0.0f, 0.0f, 0.0f, menu_preview_backdrop_alpha()}, (float) rect_x, (float) rect_top, (float) rect_width, (float) rect_height);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Draw into the rectangle only: viewport gives the projection its aspect (660/350 in Krunker),
    // scissor keeps the depth clear and the draws inside the box.
    glViewport(rect_x, rect_y, rect_width, rect_height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rect_x, rect_y, rect_width, rect_height);
    glClear(GL_DEPTH_BUFFER_BIT);

    RenderLighting saved_lighting;
    preview_lighting_apply(&saved_lighting);
    scene_render(preview->scene, &preview->camera); // aspect comes from the viewport
    g_render_lighting = saved_lighting;

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_width, fb_height); // back to the full framebuffer (resize_viewport())

    if (dump_path && ++frames == PREVIEW_DUMP_FRAME) preview_dump(dump_path, fb_width, fb_height);
}

void client_class_preview_fini(Client *client) {
    ClassPreview *preview = &client->class_preview;

    preview_player_destroy(client);

    if (preview->scene) {
        scene_fini(preview->scene);
        preview->scene = NULL;
    }

    preview->has_rect = 0;
    preview->visible = 0;
}

