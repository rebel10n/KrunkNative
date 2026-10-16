// First-person camera feel, ported from Krunker v3.1.1:
//   src/index.js gameLoop: after player.procInputs(), CONTROLS.rotateCam(shakeY,
//     shakeX + recoilAnimY * recoilMlt + landBobY * 0.1, 0) offsets the camera inside
//     the yaw/pitch objects, and CONTROLS.moveCam(x, y + height - cameraHeight, z).
//   src/data/players.js: updateAimZoom (renderer.zoom(1 + (zoom - 1) * aim * adsFovMlt))
//     while a non-scope weapon aims; toggleAim snaps the zoom for `scope` weapons when
//     fully aimed / when un-aiming starts.
//   src/libs/render.js shake/updateShake and index.js explosion handler (explosionRange 200).
#include <camera_feel.h>
#include <render_settings.h>
#include <ui_logic.h>
#include <math.h>
#include <stdlib.h>

#define CAMERA_EXPLOSION_RANGE 200.0f  // config.explosionRange
#define CAMERA_EXPLOSION_SHAKE 0.14f
#define CAMERA_SHAKE_DECAY 0.99f       // per ms
#define CAMERA_SHAKE_MIN 0.01f

static struct {
    float amount; // render.js `al`
    float phase;  // render.js `ak`
    float yaw;    // RENDER.shakeY (cos)
    float pitch;  // RENDER.shakeX (sin)
    int enabled;  // last screenShake value seen (toggling resets the offsets)
} g_shake = {.enabled = 1};

void camera_feel_shake(const float amount) {
    g_shake.amount = amount;
}

void camera_feel_explosion(const Camera *camera, const vec3 position) {
    const float dx = position.x - camera->position.x;
    const float dy = position.y - camera->position.y;
    const float dz = position.z - camera->position.z;
    const float strength = 1.0f - sqrtf(dx * dx + dy * dy + dz * dz) / CAMERA_EXPLOSION_RANGE;

    if (strength > 0.0f) camera_feel_shake(strength * CAMERA_EXPLOSION_SHAKE);
}

void camera_feel_shake_offset(float *pitch, float *yaw) {
    if (pitch) *pitch = g_shake.pitch;
    if (yaw) *yaw = g_shake.yaw;
}

void camera_feel_update(const float delta) {
    const int enabled = client_render_settings()->screen_shake;

    if (enabled != g_shake.enabled) { // screenShake.set: shakeY = shakeX = 0
        g_shake.enabled = enabled;
        g_shake.yaw = g_shake.pitch = 0.0f;
    }

    if (!g_shake.amount || !enabled) return;

    g_shake.amount *= powf(CAMERA_SHAKE_DECAY, delta * 1000.0f);
    g_shake.phase += ((float) rand() / (float) RAND_MAX * 2.0f - 1.0f) * (float) M_PI; // randFloat(-PI, PI)
    g_shake.pitch = sinf(g_shake.phase) * g_shake.amount;
    g_shake.yaw = cosf(g_shake.phase) * g_shake.amount;

    if (g_shake.amount <= CAMERA_SHAKE_MIN) {
        g_shake.amount = 0.0f;
        g_shake.yaw = g_shake.pitch = 0.0f;
    }
}

// renderer.zoom value for the local player's aim (KrunkNative aim_val 1 = aimed,
// Krunker aimVal 0 = aimed).
static float camera_feel_zoom(const Player *me) {
    const Weapon *weapon = me->weapon;
    if (!weapon || !weapon->zoom) return 1.0f;

    const float ads_fov_mlt = menu_ui()->settings.ads_fov_mlt;
    // v7.1.4: weapon.zoom * attach.zoomMlt (the Acog Scope x1.35; FOV only)
    const float zoom = weapon_aim_zoom(weapon, weapon_attachment(weapon, player_attach_index(me)));
    // toggleAim: scopes jump straight to the full zoom once aimed (and back at the
    // first un-aim tick); everything else eases with aimVal (updateAimZoom).
    if (weapon->scope) return me->aim_val >= 1.0f ? 1.0f + (zoom - 1.0f) * ads_fov_mlt : 1.0f;
    return 1.0f + (zoom - 1.0f) * me->aim_val * ads_fov_mlt;
}

// wallLean / wallLeanMlt (v10.js:221617, the game loop after moveCam): while wall sliding (onWall, the
// class can wall jump, crouched) the camera rolls towards lean = 0.15 * wallLeanMlt, signed by the wall's
// side and scaled by the look's component along it, easing in at dt / 30 per ms; off the wall it eases
// back by 1 / 40 per frame and snaps to 0 under 1e-4. Not with a detached (KrunkScript) camera.
static float g_wall_roll;

void camera_feel_wall_lean(Client *client, const float delta_ms) {
    const ClientRenderSettings *rs = client_render_settings();
    const Player *me = client->me;
    const float lean = rs->wall_lean ? 0.15f * rs->wall_lean_mlt : 0.0f;
    if (!me || lean <= 0.0f) {
        g_wall_roll = 0.0f;
        return;
    }
    if (me->on_wall && me->wall_jump && me->crouch_val) {
        const int side = me->on_wall == 3 || me->on_wall == 4;
        const float yaw = me->direction.y, pitch = me->direction.x;
        const float dir = side ? -sinf(yaw) * cosf(pitch) : -cosf(yaw) * cosf(pitch); // getWorldDirection().x / .z
        const float target = (side ? -lean : lean) * (me->on_wall % 2 ? -1.0f : 1.0f) * dir;
        g_wall_roll += fminf(delta_ms, 33.0f) * (target - g_wall_roll) / 30.0f;
    } else {
        g_wall_roll += (0.0f - g_wall_roll) / 40.0f;
        if (fabsf(g_wall_roll) < 0.0001f) g_wall_roll = 0.0f;
    }
    client->camera.rotation.z = g_wall_roll;
}

float camera_feel_wall_roll(void) {
    return g_wall_roll;
}

void camera_feel_apply(Client *client) {
    const Player *me = client->me;
    if (!me) return;

    client->camera.position = me->position;
    client->camera.position.y += me->height - game_constants.camera_height;

    client->camera.rotation.x = me->direction.x + g_shake.pitch + me->recoil_anim_y * game_constants.recoil_mlt + me->land_bob_y * 0.1f;
    client->camera.rotation.y = me->direction.y + g_shake.yaw;

    client->camera.zoom = camera_feel_zoom(me);
}
