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
#include <strike_ui.h> // strike_ui_active(): Krunker Strike's viewer framing
#include <ruleset.h> // game_constants_of: the constants the model is posed with
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
// Krunker Strike's menu viewer (FRVR 1.37.2 mI, assets/index-Cz2uYXJQ.js, read as text):
// - createCamAndOrbit: PerspectiveCamera(60, canvas aspect) at (15 / zoom, 0, 0), zoom 1, looking at
//   mainMenuCamTarget (0, playerHeight / 2, 0); its OrbitControls (minPolarAngle PI/2, maxPolarAngle 0)
//   clamp the polar angle to PI/2 on update(), keeping the radius: the camera ends level with the
//   target, sqrt(15^2 + (playerHeight / 2)^2) out on +X.
// - updatePlayer: the player is new DL(-1, ...) with init(0, 1, 0, "preview"), so updateMesh (id -1) puts
//   the model at y = 1; crouchVal = options.crouch (Vre default true), xDire = toRad(-70), updateHeight;
//   animate (Vre) false: no step / idle animation, the pose is static.
// - updateHeight leans a crouched body by Strike's crouchLean, -(PI * 0.1) (v10 has 0).
// - initLighting (lighting Vre INGAME): AmbientLight(0x97A1A8) from setup() plus DirectionalLight(0xF2F8FC,
//   1.2) at lightDistance * (cos n, sin n sin t, sin n cos t), t = PI (0.2 - 0.5), n = 2 PI (0.25 - 0.5).
// - addShadow: a 0x1B1B1B disc at opacity 0.2 under the feet when viewerData.showShadow (MAIN_MENU only).
#define PREVIEW_STRIKE_FOV ((float) M_PI * 60.0f / 180.0f)
#define PREVIEW_STRIKE_CAMERA_X 15.0f
#define PREVIEW_STRIKE_YAW (-(float) M_PI * 70.0f / 180.0f)
#define PREVIEW_STRIKE_PLAYER_Y 1.0f
#define PREVIEW_STRIKE_CROUCH_LEAN (-(float) M_PI * 0.1f)
#define PREVIEW_STRIKE_LIGHT_COLOR 0xF2F8FC
#define PREVIEW_STRIKE_LIGHT_INTENSITY 1.2f
// addShadow: Ir.GEOS.generateCylinder(1, 1, 1, {sides: 64, colr: 0x1B1B1B, transparent, opacity: 0.2}),
// scale (4, 0.01, 4), moveMesh(0, 0.49, 0): the cylinder stands on its origin, so a disc of radius 4 with
// its top at y = 0.5; the level camera above it only sees that top.
#define PREVIEW_STRIKE_SHADOW_SIDES 64
#define PREVIEW_STRIKE_SHADOW_RADIUS 4.0f
#define PREVIEW_STRIKE_SHADOW_Y 0.5f
#define PREVIEW_STRIKE_SHADOW_COLOR 0x1B1B1B
#define PREVIEW_STRIKE_SHADOW_OPACITY 0.2f
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
//   i.e. over black) once, PREVIEW_DUMP_FRAME frames in. A "%d" in the name (/tmp/cls-%d.ppm)
//   writes one per class instead, PREVIEW_DUMP_FRAME frames after that class's model is up
//   (client/tests/class_preview_all_e2e.py).
// - KRUNKNATIVE_PREVIEW_LOG=1 prints the canvas box in device px whenever it changes and
//   the first frame the canvas shows (client/tests/preview_hole_e2e.py).
// - KRUNKNATIVE_PREVIEW_BONES=1 prints the model's node chain (world and canvas positions) whenever
//   it changes: the pose against Krunker's, node by node.
// - KRUNKNATIVE_PREVIEW_WORLD=1 also keeps a copy of the third-person model in the
//   world scene in front of the idle menu camera, animated like an in-game player
//   (player_update_meshes(.., 0), map lighting), to compare with other players.
// Krunker v10.0.2 (v10.js _70434 / _70440): the camera keeps aspect 1.25 (the 750x600 canvas) and
// two poses: the menu (-30, 17, -35) looking at (0, 8.8, 0), and the Change Loadout window's preview
// column (#charPreviewSlot, toggleWindowCharPreview) (-44, 11, -51) looking at (0, 5.5, 0), where
// the player turns to xDire 55 degrees and follows a drag (xDire += 0.008 * dx). The column shows
// the 5:4 bitmap with object-fit: cover (main.css #charPreviewSlot canvas). v10's object-position
// 40% is centred here (the engine has no object-position): a 4% horizontal shift.
#define PREVIEW_V10_ASPECT 1.25f
#define PREVIEW_V10_MENU_X (-30.0f)
#define PREVIEW_V10_MENU_Y 17.0f
#define PREVIEW_V10_MENU_Z (-35.0f)
#define PREVIEW_V10_MENU_LOOK_Y 8.8f
#define PREVIEW_V10_SLOT_X (-44.0f)
#define PREVIEW_V10_SLOT_Y 11.0f
#define PREVIEW_V10_SLOT_Z (-51.0f)
#define PREVIEW_V10_SLOT_LOOK_Y 5.5f
#define PREVIEW_V10_SLOT_YAW ((float) M_PI * 55.0f / 180.0f)
#define PREVIEW_V10_DRAG_TURN 0.008f
static int g_v10_mode;            // 0 v7 page, 1 v10 menu, 2 v10 window column (preview_mode)
static float g_v10_xdire = PREVIEW_V10_SLOT_YAW; // _70439: kept between windows
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

// dJ() / updateClassPreview: the preview wears the player's own Change Loadout look, the very one
// the ENT sends (ui_loadout_look: skins, sight, charms, wrist, cosmetics, the secondary), except that
// Random shows none (v7's `== -2 ? -1`) where the ENT rolls an owned item. The K.P.D popup's dI
// (Officer Buddy) overrides hat / back / dye / face with the police outfit (shoPolicePop: back 1487,
// hat 1890, dye 1891, face 1842) and wears none of the v7 slots.
static LoadoutLook preview_look(const int class_index) {
    LoadoutLook look;
    ui_loadout_look(class_index, 0, &look);

    if (ui_police_active()) {
        look.cosmetics.body = 1487;
        look.cosmetics.hat = 1890;
        look.cosmetics.dye = 1891;
        look.cosmetics.face = 1842;
        look.cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    }

    return look;
}

// updateHeadSize(classPreviewPlayer, tmpPlayer ? tmpPlayer.headMlt : 1): our own head size
static float preview_head_mlt(const Client *client) {
    return client->me && client->me->head_mlt > 0.0f ? client->me->head_mlt : 1.0f;
}

// Everything of the look that player_generate_meshes reads: a change rebuilds the model.
static void preview_look_apply(Player *player, const LoadoutLook *look, const float head_mlt) {
    player->cosmetics = look->cosmetics;
    player->cosmetics_v7 = look->cosmetics_v7;
    player->skins = look->skins;
    player->skin_paints = look->skin_paints; // the account's own painted copies (v10: the local model's $account.skins)
    player->secondary_index = look->secondary_index;
    player->attach_index = look->attach_index;
    player->head_mlt = head_mlt;
}

static int preview_cosmetics_changed(const Client *client, const Player *player) {
    const LoadoutLook want = preview_look(player->class_index);
    return memcmp(&player->cosmetics, &want.cosmetics, sizeof(want.cosmetics)) != 0 || memcmp(&player->skins, &want.skins, sizeof(want.skins)) != 0 ||
           memcmp(&player->skin_paints, &want.skin_paints, sizeof(want.skin_paints)) != 0 ||
           memcmp(&player->cosmetics_v7, &want.cosmetics_v7, sizeof(want.cosmetics_v7)) != 0 || player->secondary_index != want.secondary_index ||
           player->attach_index != want.attach_index || player->head_mlt != preview_head_mlt(client);
}

// A private third-person player of the menu class, like Krunker's preview `dF`
// (init(0,0,0,"preview"), setClass, pY2(dF, 0) and swapTime = 0), added to `scene`.
static Player *preview_player_new(Client *client, Scene *scene) {
    Player *player = player_init(&client->game);
    if (!player) return NULL;

    player->uid = -1; // player_update_meshes() anchors uid -1 (Krunker's preview sid) at player->position
    player->class_index = client->menu_class_index;
    const LoadoutLook look = preview_look(player->class_index);
    preview_look_apply(player, &look, preview_head_mlt(client));
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

// Krunker Strike: FRVR's viewer shadow (addShadow), shown where viewerData.showShadow is set (MAIN_MENU).
// Its geometry, material and mesh belong to the preview (strike_shadow_free).
static Mesh *g_strike_shadow;
static unsigned int g_strike_shadow_vbo;

static Mesh *strike_shadow_create(void) {
    enum { SIDES = PREVIEW_STRIKE_SHADOW_SIDES };
    vertex v[SIDES + 1];
    unsigned int idx[SIDES * 3];
    v[0] = (vertex) {{0.0f, PREVIEW_STRIKE_SHADOW_Y, 0.0f}, {0.5f, 0.5f}};
    for (int i = 0; i < SIDES; i++) {
        const float theta = 2.0f * (float) M_PI * (float) i / (float) SIDES;
        v[1 + i] = (vertex) {{PREVIEW_STRIKE_SHADOW_RADIUS * sinf(theta), PREVIEW_STRIKE_SHADOW_Y, PREVIEW_STRIKE_SHADOW_RADIUS * cosf(theta)},
                             {cosf(theta) * 0.5f + 0.5f, sinf(theta) * 0.5f + 0.5f}};
        idx[i * 3 + 0] = (unsigned int) (1 + i);
        idx[i * 3 + 1] = (unsigned int) (1 + (i + 1) % SIDES);
        idx[i * 3 + 2] = 0;
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    BasicMaterial *material = basic_material_init();
    if (!geometry || !material) {
        free(geometry);
        free(material);
        return NULL;
    }
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &g_strike_shadow_vbo);
    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_strike_shadow_vbo);
    glBufferData(GL_ARRAY_BUFFER, (long) sizeof(v), v, GL_STATIC_DRAW);
    geometry_set_bounds(geometry, v, SIDES + 1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long) sizeof(idx), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    geometry->index_count = SIDES * 3;

    material->color = hex_to_vec(PREVIEW_STRIKE_SHADOW_COLOR);
    material->color.w = PREVIEW_STRIKE_SHADOW_OPACITY;
    material->color_bottom = material->color;
    material->base.transparent = 1;
    material->fog_enabled = 0;
    material->use_flat_normal = 1; // a horizontal disc: (0, 1, 0)
    return mesh_init(geometry, &material->base);
}

static void strike_shadow_free(Scene *scene) {
    if (!g_strike_shadow) return;
    if (scene) scene_remove_mesh(scene, g_strike_shadow);
    Geometry *geometry = g_strike_shadow->geometry;
    mesh_fini(g_strike_shadow); // frees the material, not the geometry
    g_strike_shadow = NULL;
    if (geometry) {
        glDeleteVertexArrays(1, &geometry->vao);
        geometry_free_wire(geometry);
        glDeleteBuffers(1, &geometry->ebo);
        free(geometry);
    }
    if (g_strike_shadow_vbo) glDeleteBuffers(1, &g_strike_shadow_vbo);
    g_strike_shadow_vbo = 0;
}

// Shows FRVR's viewer shadow on MAIN_MENU (yc {showShadow: true}); every other viewer has none.
static void strike_shadow_update(Scene *scene, const int strike) {
    const int show = strike && strike_screen_top() == STRIKE_SCREEN_MAIN_MENU;
    if (show && !g_strike_shadow && scene) {
        g_strike_shadow = strike_shadow_create();
        if (g_strike_shadow) scene_add_mesh(scene, g_strike_shadow);
    }
    if (g_strike_shadow) g_strike_shadow->visible = show;
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
    // Krunker Strike (FRVR's viewer, yc / mI.createCamAndOrbit): a 60 degree lens 15 units out on
    // +X, level with the middle of the model (playerHeight / 2); the model faces that camera.
    const int strike = strike_ui_active();

    camera->fov = strike ? PREVIEW_STRIKE_FOV : PREVIEW_FOV;
    camera->zoom = 1.0f;
    camera->near = PREVIEW_NEAR;
    camera->far = PREVIEW_FAR;
    camera->position = (vec3) {PREVIEW_CAMERA_X, PREVIEW_CAMERA_Y, PREVIEW_CAMERA_Z};
    vec3 look = target;
    if (strike) {
        // FRVR's orbit-clamped camera: level with (0, playerHeight / 2, 0) at the radius of (15, 0, 0)
        const float mid = game_constants.player_height * 0.5f;
        camera->position = (vec3) {sqrtf(PREVIEW_STRIKE_CAMERA_X * PREVIEW_STRIKE_CAMERA_X + mid * mid), mid, 0.0f};
        look = (vec3) {0.0f, mid, 0.0f};
    } else if (g_v10_mode == 1) {
        camera->position = (vec3) {PREVIEW_V10_MENU_X, PREVIEW_V10_MENU_Y, PREVIEW_V10_MENU_Z};
        look = (vec3) {0.0f, PREVIEW_V10_MENU_LOOK_Y, 0.0f};
    } else if (g_v10_mode == 2) {
        camera->position = (vec3) {PREVIEW_V10_SLOT_X, PREVIEW_V10_SLOT_Y, PREVIEW_V10_SLOT_Z};
        look = (vec3) {0.0f, PREVIEW_V10_SLOT_LOOK_Y, 0.0f};
    }

    vec3 dir = {look.x - camera->position.x, look.y - camera->position.y, look.z - camera->position.z};
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
    player->direction.y = strike ? PREVIEW_STRIKE_YAW : g_v10_mode == 2 ? g_v10_xdire : PREVIEW_YAW;
    // FRVR: classPlayer = new DL(-1, ...), init(0, 1, 0, "preview"): id -1 moves the mesh to (x, y, z)
    player->position.y = strike ? PREVIEW_STRIKE_PLAYER_Y : 0.0f;
}

// The v10 page's pose for this frame: its window column when the canvas sits in #charPreviewSlot.
static int preview_mode(const HtmlElem canvas) {
    char id[32] = "";
    const HtmlElem parent = canvas ? html_dom_parent(canvas) : 0;
    return parent && html_dom_get_attr(parent, "id", id, sizeof(id)) >= 0 && !strcmp(id, "charPreviewSlot") ? 2 : 1;
}

// v10 #charPreviewSlot pointermove while dragged: xDire += 0.008 * dx (CSS px)
void client_class_preview_turn(Client *client, const float dx) {
    (void) client;
    g_v10_xdire += PREVIEW_V10_DRAG_TURN * dx;
}

// Swap g_render_lighting for Krunker's preview scene lights around the draw.
static void preview_lighting_apply(RenderLighting *saved) {
    *saved = g_render_lighting;

    vec3 light = {PREVIEW_LIGHT_X, PREVIEW_LIGHT_Y, PREVIEW_LIGHT_Z};
    const int strike = strike_ui_active();
    if (strike) { // FRVR's INGAME sun: (cos n, sin n sin t, sin n cos t)
        const float t = (float) M_PI * (0.2f - 0.5f), n = 2.0f * (float) M_PI * (0.25f - 0.5f);
        light = (vec3) {cosf(n), sinf(n) * sinf(t), sinf(n) * cosf(t)};
    }
    const float length = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    light.x /= length;
    light.y /= length;
    light.z /= length;

    g_render_lighting.ambient_color = hex_to_vec(PREVIEW_AMBIENT_COLOR);
    g_render_lighting.ambient_intensity = PREVIEW_AMBIENT_INTENSITY;
    g_render_lighting.light_color = hex_to_vec(strike ? PREVIEW_STRIKE_LIGHT_COLOR : PREVIEW_LIGHT_COLOR);
    g_render_lighting.light_intensity = strike ? PREVIEW_STRIKE_LIGHT_INTENSITY : PREVIEW_LIGHT_INTENSITY;
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

    if (dummy && (class_index != client->menu_class_index || map != client->game.map || preview_cosmetics_changed(client, dummy))) {
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

// KRUNKNATIVE_PREVIEW_BONES: the model's node chain in world space and on the canvas (device px), once
// per change, to compare the pose with Krunker's (the same nodes: objInstances, lowerBody, upperBody,
// the head, the arms and the assault rifle's muzzle at (0, muzOffY, -muzOff) of the arms anchor).
static void preview_log_bones(const ClassPreview *preview, const int width, const int height) {
    static char last[1024];
    const Player *player = preview->player;
    const PlayerMesh *mesh = player->mesh;
    const Camera *cam = &preview->camera;
    const PlayerArms *arms = mesh->arms ? mesh->arms[player->loadout_index] : NULL;
    struct { const char *name; const MeshTransform *node; vec3 local; } nodes[] = {
        {"objInstances", mesh->anchor, {0, 0, 0}},
        {"lowerBody", mesh->body_anchor, {0, 0, 0}},
        {"upperBody", mesh->upper_body_anchor, {0, 0, 0}},
        {"head", mesh->head ? &mesh->head->transform : NULL, {0, 0, 0}},
        {"arms", arms ? &arms->anchor : NULL, {0, 0, 0}},
        {"muzzle", arms ? &arms->anchor : NULL, {0, -0.05f, -5.1f}}, // FRVR's assault rifle muzOffY / muzOff
    };
    char buf[1024];
    int len = 0;
    for (size_t i = 0; i < sizeof nodes / sizeof nodes[0] && len < (int) sizeof buf - 160; i++) {
        if (!nodes[i].node) continue;
        const vec3 w = transform_point_to(nodes[i].node, nodes[i].local, NULL);
        const float *v = cam->world_inverse_matrix, *pm = cam->projection_matrix; // row-major (camera.c)
        const float cx = v[0] * w.x + v[1] * w.y + v[2] * w.z + v[3], cy = v[4] * w.x + v[5] * w.y + v[6] * w.z + v[7];
        const float cz = v[8] * w.x + v[9] * w.y + v[10] * w.z + v[11];
        const float px = pm[0] * cx + pm[1] * cy + pm[2] * cz + pm[3], py = pm[4] * cx + pm[5] * cy + pm[6] * cz + pm[7];
        const float pw = pm[12] * cx + pm[13] * cy + pm[14] * cz + pm[15];
        len += snprintf(buf + len, sizeof buf - (size_t) len, " %s=(%.3f %.3f %.3f)@(%.1f %.1f)", nodes[i].name, (double) w.x, (double) w.y,
                        (double) w.z, (double) ((px / pw * 0.5f + 0.5f) * (float) width), (double) ((0.5f - py / pw * 0.5f) * (float) height));
    }
    if (!strcmp(buf, last)) return;
    snprintf(last, sizeof last, "%s", buf);
    printf("[preview] bones%s\n", buf);
    fflush(stdout);
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
    static int bones_log;
    static int frames;

    if (test_rect < 0) {
        test_rect = getenv("KRUNKNATIVE_PREVIEW_TEST") != NULL;
        rect_log = getenv("KRUNKNATIVE_PREVIEW_LOG") != NULL;
        world_dummy = getenv("KRUNKNATIVE_PREVIEW_WORLD") != NULL;
        dump_path = getenv("KRUNKNATIVE_PREVIEW_DUMP");
        bones_log = getenv("KRUNKNATIVE_PREVIEW_BONES") != NULL;
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
    const HtmlElem page_canvas = html_dom_by_id("classPreviewCanvas");
    // Krunker Strike's viewer is no v10 menu preview: its canvas is the screen (FRVR's renderer.setSize at
    // the canvas's own size, aspect = clientWidth / clientHeight), not v10's 5:4 bitmap
    g_v10_mode = strike_ui_active() ? 0 : preview_mode(page_canvas);
    const int height = (int) lroundf(preview->height * preview->dpr);
    // v10: a 5:4 bitmap whatever the box (renderer.setSize(750, 600)); the page covers the box with it
    const int width = g_v10_mode ? (int) lroundf((float) height * PREVIEW_V10_ASPECT) : (int) lroundf(preview->width * preview->dpr);
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

    if (preview->player && (preview->class_index != client->menu_class_index || preview->map != client->game.map || preview_cosmetics_changed(client, preview->player))) {
        preview_player_destroy(client);
    }

    if (!preview->player && !preview_player_create(client)) {
        preview_hide(preview);
        return;
    }

    const HtmlElem canvas = html_dom_by_id("classPreviewCanvas");
    if (!canvas || !preview_target(preview, width, height)) return;

    const int strike = strike_ui_active();
    if (!strike) preview_animate(preview->player, &preview->step_dir, delta); // FRVR's viewer: animate false
    strike_shadow_update(preview->scene, strike);
    preview_frame(client);
    preview->player->crouch_val = strike ? 1.0f : 0.0f; // FRVR: crouchVal = options.crouch (true)
    player_update_meshes(preview->player, 1);
    if (strike && preview->player->mesh) {
        // updateHeight's lean with Strike's crouchLean in place of the one the model was posed with
        // (the game's rules: Strike's own since its data-driven constants, then nothing is added): the
        // lower body tilts by it and the upper body (arms, weapon) turns back by as much
        const float lean = (PREVIEW_STRIKE_CROUCH_LEAN - game_constants_of(preview->player->game)->crouch_lean) * preview->player->crouch_val;
        PlayerMesh *mesh = preview->player->mesh;
        mesh->body_anchor->rotation.x += lean;
        mesh->upper_body_anchor->rotation.x -= lean;
    }

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

    gl_release_texture(preview->color_tex); // the page samples it (natively): not while drawing into it
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
    if (bones_log && preview->player->mesh) preview_log_bones(preview, width, height);
    scene_set_alpha_target(0);
    g_render_lighting = saved_lighting;

    if (dump_path && strstr(dump_path, "%d")) { // one dump per class, PREVIEW_DUMP_FRAME frames after it is built
        static int dumped_class = -1;
        if (preview->class_index != dumped_class) {
            dumped_class = preview->class_index;
            frames = 0;
        }
        if (++frames == PREVIEW_DUMP_FRAME) {
            char path[512];
            const char *mark = strstr(dump_path, "%d");
            snprintf(path, sizeof(path), "%.*s%d%s", (int) (mark - dump_path), dump_path, preview->class_index, mark + 2);
            preview_dump(path, width, height);
        }
    } else if (dump_path && ++frames == PREVIEW_DUMP_FRAME) {
        preview_dump(dump_path, width, height);
    }

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
    strike_shadow_free(preview->scene);

    if (preview->scene) {
        scene_fini(preview->scene);
        preview->scene = NULL;
    }

    preview->has_rect = 0;
    preview->visible = 0;
}

