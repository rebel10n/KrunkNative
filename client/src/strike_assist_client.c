// Krunker Strike's mobile aim assist and auto-fire in the client (the core: client/src/strike_assist.c):
// the gameLoop pass over the game's players (FRVR 1.37.2 vbe: active, not you, not a teammate, the
// tracked one only once locked; gbe: the body centre y + (playerHeight - crouchVal * crouchDst) / 2,
// projected through the camera, in the frustum and visible), its look pull for updateMouseInp, and the
// shoot key auto-fire drives. Runs only on a phone (ui_mobile_active) under a flavor with Strike's
// rules; the base game never gets here.
#include <strike_assist_client.h>
#include <strike_assist.h>
#include <flavor.h>
#include <ai.h>        // game_ai_player_can_see: Game.canSee from the player's eye
#include <ruleset.h>     // game_constants_of
#include <html_dom.h>  // html_dom_viewport: innerWidth / innerHeight
#include <ui_logic.h>  // ui_setting_value
#include <ui_mobile.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define ASSIST_TARGETS_MAX 64

static StrikeAssist g_assist = {.uid = -1};
static double g_last_look_ms = -1.0;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static const StrikeAimAssist *rules(void) {
    const StrikeRules *r = flavor_current()->rules;
    return r && ui_mobile_active() ? &r->aim_assist : NULL;
}

static int setting_on(const char *key) { // the Strike defaults are on (strike_rules.h settings)
    const cJSON *v = ui_setting_value(key);
    if (!v) return 1;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v) != 0.0;
    if (cJSON_IsString(v)) return strcmp(cJSON_GetStringValue(v), "false") != 0 && strcmp(cJSON_GetStringValue(v), "0") != 0;
    return 1;
}

// THREE Vector3.project + camera.frustum.containsPoint
static int project(const Camera *camera, const vec3 p, float *sx, float *sy) {
    float view[4], clip[4];
    const float in[4] = {p.x, p.y, p.z, 1.0f};
    for (int r = 0; r < 4; r++) {
        view[r] = camera->world_inverse_matrix[r * 4] * in[0] + camera->world_inverse_matrix[r * 4 + 1] * in[1] +
                  camera->world_inverse_matrix[r * 4 + 2] * in[2] + camera->world_inverse_matrix[r * 4 + 3] * in[3];
    }
    for (int r = 0; r < 4; r++) {
        clip[r] = camera->projection_matrix[r * 4] * view[0] + camera->projection_matrix[r * 4 + 1] * view[1] +
                  camera->projection_matrix[r * 4 + 2] * view[2] + camera->projection_matrix[r * 4 + 3] * view[3];
    }
    if (!(clip[3] > 0.0f)) return 0;
    const float x = clip[0] / clip[3], y = clip[1] / clip[3], z = clip[2] / clip[3];
    *sx = (x + 1.0f) * 0.5f;
    *sy = (y + 1.0f) * 0.5f;
    return x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f && z >= -1.0f && z <= 1.0f;
}

int strike_assist_client_tick(Client *client) {
    const StrikeAimAssist *k = rules();
    if (!k || !client) {
        if (g_assist.uid >= 0 || g_assist.has_dist || g_assist.autofiring) strike_assist_reset(&g_assist);
        return -1;
    }
    Game *game = &client->game;
    Player *me = client->me;
    float w = 0.0f, h = 0.0f, dpr = 1.0f;
    html_dom_viewport(&w, &h, &dpr);
    StrikeAssistFrame f = {
        .width = w, .height = h,
        .zoom = client->camera.zoom > 0.0f ? client->camera.zoom : 1.0f,
        .mobile = 1,
        .aim_assist = (unsigned char) setting_on("aimAssist"),
        .auto_fire = (unsigned char) setting_on("autoFireMbl"),
        .joystick_active = (unsigned char) ui_mobile_touch_active(),
        .semi_auto = (unsigned char) (me && me->weapon && me->weapon->no_auto),
        .friendly = (unsigned char) (game->mode && game->mode->config.friendly),
        .now_ms = now_ms(),
    };
    StrikeAssistTarget targets[ASSIST_TARGETS_MAX];
    size_t n = 0;
    const int teams = game->mode && game->mode->config.teams;
    const GameConstants *c = game_constants_of(game);
    for (size_t i = 0; me && me->active && i < game->player_count && n < ASSIST_TARGETS_MAX; i++) {
        Player *p = game->players[i];
        if (!p || p == me || !p->active || (teams && p->team == me->team)) continue;
        vec3 point = p->position;
        point.y += (c->player_height - p->crouch_val * c->crouch_distance) / k->aim_point_div;
        StrikeAssistTarget *t = &targets[n];
        t->uid = p->uid;
        t->in_frustum = (unsigned char) project(&client->camera, point, &t->sx, &t->sy);
        const vec3 cam = client->camera.position;
        t->dist = sqrtf((point.x - cam.x) * (point.x - cam.x) + (point.y - cam.y) * (point.y - cam.y) + (point.z - cam.z) * (point.z - cam.z));
        t->visible = (unsigned char) (t->in_frustum && game_ai_player_can_see(game, me, point));
        t->on_ground = p->on_ground;
        n++;
    }
    return strike_assist_frame(k, &g_assist, &f, targets, n);
}

int strike_assist_client_look(float *look_x, float *look_y, float *mlt) {
    const StrikeAimAssist *k = rules();
    const double t = now_ms();
    const double dt_ms = g_last_look_ms < 0.0 ? 0.0 : fmin(t - g_last_look_ms, game_constants.max_delta * 1000.0); // On = min(On, dltMx)
    g_last_look_ms = t;
    *look_x = *look_y = 0.0f;
    *mlt = 1.0f;
    if (!k) return 0;
    float w = 0.0f, h = 0.0f, dpr = 1.0f;
    html_dom_viewport(&w, &h, &dpr);
    strike_assist_pull(k, &g_assist, (float) (dt_ms / 1000.0), w, h, look_x, look_y);
    *mlt = strike_assist_look_mlt(k, &g_assist);
    return 1;
}
