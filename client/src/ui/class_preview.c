// Live third-person preview of the selected class for the HTML menu.
//
// Krunker's menu renders the selected class into `classPreviewCanvas` with a
// dedicated three.js scene (index.js: `dJ()` builds a preview player with
// `players.jLrR6(player, false, true)` and the render loop steps it with
// `players.Te7(dF, delta * 0.015, true)` + `updateMsh(dF, true)`) and a transparent
// WebGLRenderer. We do the same: the third-person PlayerMesh is rendered into an
// offscreen render target at the canvas's device-pixel size (transparent where the
// model isn't, premultiplied alpha), and the page shows it as the bitmap of its
// #classPreviewCanvas (html_dom_canvas_gl). The canvas therefore keeps its place in
// the page's paint order, as in Krunker: it lies above #instructionHolder's 50% shade
// and under whatever covers it (#popupHolder's backdrop), and the page needs no holes.
// menu_send_preview_rect (menu_ui.c) reports the canvas box (html_dom_get_rect).
//
// Everything here is client renderer state; the preview Player is a private copy
// that never joins client->game.players.
#include <gl.h>
#include <client.h>
#include <html_ui.h>
#include <html_dom.h> // html_dom_viewport
#include <ui_logic.h> // ui_loadout_*, ui_police_active
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
// The preview player is created with init(0, 0, 0, "preview") and never turned:
// yaw 0 faces -Z, i.e. towards the camera, turned slightly to its left.
#define PREVIEW_YAW 0.0f
// Krunker: stepVal += delta_ms * 0.015 * stepMlt(0.25) * stepDir, flipping at +-PI/2.
#define PREVIEW_STEP_SPEED (1000.0f * 0.015f * 0.25f)
// Debug hooks (environment variables, read once):
// - KRUNKNATIVE_PREVIEW_TEST=1 forces the rectangle below (CSS px) while the menu
//   is open, so the C side can be checked without the page sending previewRect.
// - KRUNKNATIVE_PREVIEW_DUMP=<file.ppm> writes the preview's render target (premultiplied,
//   i.e. over black) once, PREVIEW_DUMP_FRAME frames in.
// - KRUNKNATIVE_PREVIEW_LOG=1 prints the canvas box in device px whenever it changes and
//   the first frame the canvas shows (client/tests/preview_hole_e2e.py).
// - KRUNKNATIVE_PREVIEW_WORLD=1 also keeps a copy of the third-person model in the
//   world scene in front of the idle menu camera, animated like an in-game player
//   (player_update_meshes(.., 0), map lighting), to compare with other players.
#define PREVIEW_DUMP_FRAME 60
#define PREVIEW_MAX_SIZE 4096 // render target edge limit (device px)
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
        cosmetics.body = 1487;
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

// WP-F: the v7 slots (head, back, shoes, pet, wrist, charms; Random shows none); none for Officer Buddy
static PlayerCosmeticsV7 preview_cosmetics_v7(const int class_index) {
    PlayerCosmeticsV7 v7 = PLAYER_COSMETICS_V7_NONE;
    if (!ui_police_active()) ui_loadout_cosmetics_v7(class_index, &v7, 0);
    return v7;
}

static int preview_cosmetics_changed(const Player *player) {
    const PlayerCosmetics want = preview_cosmetics();
    const PlayerCosmeticsV7 want_v7 = preview_cosmetics_v7(player->class_index);
    const PlayerSkins want_skins = preview_skins(player->class_index);
    return memcmp(&player->cosmetics, &want, sizeof(want)) != 0 || memcmp(&player->skins, &want_skins, sizeof(want_skins)) != 0 ||
           memcmp(&player->cosmetics_v7, &want_v7, sizeof(want_v7)) != 0;
}

// A private third-person player of the menu class, like Krunker's preview `dF`
// (init(0,0,0,"preview"), setClass, pY2(dF, 0) and swapTime = 0), added to `scene`.
static Player *preview_player_new(Client *client, Scene *scene) {
    Player *player = player_init(&client->game);
    if (!player) return NULL;

    player->uid = -1; // player_update_meshes() anchors uid -1 (Krunker's preview sid) at player->position
    player->class_index = client->menu_class_index;
    player->cosmetics = preview_cosmetics();
    player->cosmetics_v7 = preview_cosmetics_v7(player->class_index);
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
    // The bound framebuffer (the preview's target). GL_RGBA/GL_UNSIGNED_BYTE: the one
    // read-back format OpenGL ES guarantees (KN_GLES)
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
        printf("[preview] render target written to %s\n", path);
    }

    free(pixels);
}

// The preview's render target: colour texture + depth renderbuffer at the canvas's device
// size. Returns 0 when it can't be (re)allocated.
static int preview_target(ClassPreview *preview, const int width, const int height) {
    if (preview->fbo && preview->target_width == width && preview->target_height == height) return 1;

    GLint prev_fbo = 0, prev_tex = 0, prev_rb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);

    if (!preview->fbo) {
        glGenFramebuffers(1, &preview->fbo);
        glGenTextures(1, &preview->color_tex);
        glGenRenderbuffers(1, &preview->depth_rb);
    }
    glBindTexture(GL_TEXTURE_2D, preview->color_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); // complete for the page's mipmap filters too
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer(GL_RENDERBUFFER, preview->depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, preview->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, preview->color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, preview->depth_rb);
    const int complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint) prev_tex);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint) prev_rb);

    preview->target_width = complete ? width : 0;
    preview->target_height = complete ? height : 0;
    if (!complete) fprintf(stderr, "[preview] render target %dx%d incomplete\n", width, height);
    return complete;
}

// The page's canvas shows nothing (no frame yet, or none any more).
static void preview_hide(ClassPreview *preview) {
    if (!preview->shown) return;
    const HtmlElem canvas = html_dom_by_id("classPreviewCanvas");
    if (canvas) html_dom_canvas_gl(canvas, 0, 0, 0, 0);
    preview->shown = 0;
}

static void preview_target_free(ClassPreview *preview) {
    preview_hide(preview);
    if (preview->fbo) glDeleteFramebuffers(1, &preview->fbo);
    if (preview->color_tex) glDeleteTextures(1, &preview->color_tex);
    if (preview->depth_rb) glDeleteRenderbuffers(1, &preview->depth_rb);
    preview->fbo = preview->color_tex = preview->depth_rb = 0;
    preview->target_width = preview->target_height = 0;
}

void client_class_preview_render(Client *client) {
    ClassPreview *preview = &client->class_preview;

    static int test_rect = -1;
    static int rect_log;
    static int world_dummy;
    static const char *dump_path;
    static int frames;

    if (test_rect < 0) {
        test_rect = getenv("KRUNKNATIVE_PREVIEW_TEST") != NULL;
        rect_log = getenv("KRUNKNATIVE_PREVIEW_LOG") != NULL;
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

    // Hidden menu / preview box: nothing to draw (the page doesn't paint the canvas either).
    if (!html_ui_is_menu_open() || !preview->has_rect || !preview->visible) return;
    if (preview->width <= 0.0f || preview->height <= 0.0f) return;

    // The canvas's device-pixel size, like three.js's renderer.setSize(css) * setPixelRatio(dpr):
    // menu_send_preview_rect hands over whole device px / dpr (natively the page is laid out in
    // framebuffer px, dpr 1; on the web dpr = devicePixelRatio), so the page draws the bitmap
    // 1:1 over the canvas box.
    const int width = (int) lroundf(preview->width * preview->dpr);
    const int height = (int) lroundf(preview->height * preview->dpr);
    if (width <= 0 || height <= 0 || width > PREVIEW_MAX_SIZE || height > PREVIEW_MAX_SIZE) return;
    if (rect_log) { // KRUNKNATIVE_PREVIEW_LOG: the canvas box in device px whenever it changes
        static int last[4];
        const int box[4] = {(int) lroundf(preview->x * preview->dpr), (int) lroundf(preview->y * preview->dpr), width, height};
        if (memcmp(last, box, sizeof(last)) != 0) {
            memcpy(last, box, sizeof(last));
            printf("[preview] rect %d,%d %dx%d (dpr %g)\n", box[0], box[1], box[2], box[3], preview->dpr);
            fflush(stdout); // in order with the capture's stderr lines (preview_hole_e2e.py)
        }
    }

    // Until the model can be built (map config and weapons still loading, the web's on-demand
    // fetches) the canvas shows nothing: the page's own shade covers the box like the rest.
    if (!client->game.ready || !client->game.map) {
        preview_hide(preview);
        return;
    }

    if (preview->player && (preview->class_index != client->menu_class_index || preview->map != client->game.map || preview_cosmetics_changed(preview->player))) {
        preview_player_destroy(client);
    }

    if (!preview->player && !preview_player_create(client)) {
        preview_hide(preview);
        return;
    }

    const HtmlElem canvas = html_dom_by_id("classPreviewCanvas");
    if (!canvas || !preview_target(preview, width, height)) return;

    preview_animate(preview->player, &preview->step_dir, delta);
    preview_frame(client);
    player_update_meshes(preview->player, 1);

    // Render into the target: transparent where the model isn't, premultiplied alpha (three.js
    // WebGLRenderer({alpha: true}) on Krunker's classPreviewCanvas). Save the state touched here.
    GLint prev_fbo = 0, prev_viewport[4];
    GLfloat prev_clear[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear);
    const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean prev_blend = glIsEnabled(GL_BLEND);
    const GLboolean prev_depth = glIsEnabled(GL_DEPTH_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, preview->fbo);
    glViewport(0, 0, width, height); // the projection's aspect (660/350 in Krunker's box)
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    RenderLighting saved_lighting;
    preview_lighting_apply(&saved_lighting);
    scene_set_alpha_target(1);
    scene_render(preview->scene, &preview->camera); // aspect comes from the viewport
    scene_set_alpha_target(0);
    g_render_lighting = saved_lighting;

    if (dump_path && ++frames == PREVIEW_DUMP_FRAME) preview_dump(dump_path, width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_blend) glEnable(GL_BLEND);
    else glDisable(GL_BLEND);
    if (prev_depth) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);

    // The page paints the canvas box with this frame (natively it samples the texture at
    // html_ui_render; the web copies it into the real canvas).
    html_dom_canvas_gl(canvas, preview->color_tex, preview->fbo, width, height);
    if (rect_log && !preview->shown) {
        printf("[preview] first frame %dx%d\n", width, height);
        fflush(stdout);
    }
    preview->shown = 1;
}

void client_class_preview_fini(Client *client) {
    ClassPreview *preview = &client->class_preview;

    preview_player_destroy(client);
    preview_target_free(preview);

    if (preview->scene) {
        scene_fini(preview->scene);
        preview->scene = NULL;
    }

    preview->has_rect = 0;
    preview->visible = 0;
}

