// Prop Hunt on the client (client/include/prop_hunt_client.h).
#include <gl.h>
#include <prop_hunt_client.h>
#include <prop_hunt.h>
#include <social_v7.h>
#include <net_ext.h>
#include <canvas2d.h>
#include <headless.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <sprays.h>
#include <i18n.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROP_CHAT_COLOR "#fc03ec" // addChat type 2's default

static struct {
    int open;          // propWheel: the key is held
    float timer;       // propWheelTimer (s)
    int index;         // propWheelIndex
    float cam;         // iîiïìïi: the Prop team's camera distance
    Canvas2D *canvas;
    int drawn;
    int hud;           // -1 unknown, 0 normal, 1 the prop's HUD
} g_ph = {.cam = PROP_CAM_DISTANCE, .hud = -1};

int prop_hunt_me(const Client *client) {
    const Player *me = client ? client->me : NULL;
    return me && me->active && prop_hunt_player(me);
}

// teamOptions[team - 1] == "Prop" for the local player in a prop mode (the keys' gate: v10 checks the
// team, not the prop)
static int prop_team_me(const Client *client) {
    const Player *me = client ? client->me : NULL;
    return me && me->active && client->game.mode && client->game.mode->config.prop && game_player_is_prop(&client->game, me);
}

static void prop_chat(const char *key, const char *arg) {
    char text[160];
    const char *argv[1] = {arg};
    i18n_format(text, sizeof(text), key, arg ? 1 : 0, argv);
    hud_ui_add_chat(NULL, text, PROP_CHAT_COLOR);
}

// ---------------------------------------------------------------- packets

int prop_hunt_on_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (subtype == NET_EXT_PROP_UPDATE && length >= sizeof(NetPropUpdatePacket)) {
        NetPropUpdatePacket u;
        memcpy(&u, body, sizeof(u));
        Player *player = game_find_player(&client->game, u.uid);
        if (!player) return 1;
        const int you = player == client->me;
        if (u.limit) { // the server refused a pick: out of switches
            if (you) prop_chat("modes.prophunt.limit", NULL);
            return 1;
        }
        // !skip && players.updateProp(player, propID, true): the regenerated meshes wear it
        if (!u.skip && prop_is_valid(u.prop_id)) {
            const int changed = player->prop_id != u.prop_id;
            prop_apply(player, u.prop_id);
            if (changed && player->mesh) {
                const int render_you = ((const PlayerMesh *) player->mesh)->render_you;
                client_drop_player_mesh(client, player);
                client_add_player_mesh(client, player, render_you);
            }
        }
        if (you) {
            const int index = prop_index_of(u.prop_id);
            g_ph.index = index > -1 ? index : 0; // propWheelIndex: the prop worn
            if (u.switches >= 0) { // propSwitches = n: "You have {0} switches remaining"
                player->prop_switches = u.switches;
                const int max = client->game.mode ? client->game.mode->config.prop_switches : 0;
                char left[16];
                snprintf(left, sizeof(left), "%d", max - u.switches);
                prop_chat("modes.prophunt.left", left);
            }
        }
        return 1;
    }
    if (subtype == NET_EXT_PROP_ROTATION && length >= sizeof(NetPropRotationPacket)) {
        NetPropRotationPacket r;
        memcpy(&r, body, sizeof(r));
        Player *player = game_find_player(&client->game, r.uid);
        if (!player) return 1;
        player->prop_rot = r.rot;
        player->prop_rot_saved_set = r.saved_set;
        player->prop_rot_saved = r.saved_set ? r.saved : 0.0f;
        if (player == client->me) prop_chat(r.reset ? "modes.prophunt.rotation.reset" : r.rot ? "modes.prophunt.rotation.on" : "modes.prophunt.rotation.off", NULL);
        return 1;
    }
    return 0;
}

int prop_hunt_send_pick(Client *client, const int prop_id) {
    if (!client || !prop_is_valid(prop_id)) return 0;
    const NetPropPickPacket body = {.prop_id = (int16_t) prop_id};
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_PROP_PICK, &body, sizeof(body));
    if (n) client_net_send(client, NET_PACKET_EXT, buffer, n);
    return n != 0;
}

int prop_hunt_send_rotate(Client *client, const int reset) {
    if (!client) return 0;
    const NetPropRotatePacket body = {.reset = (uint8_t) !!reset};
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_PROP_ROTATE, &body, sizeof(body));
    if (n) client_net_send(client, NET_PACKET_EXT, buffer, n);
    return n != 0;
}

// ---------------------------------------------------------------- keys

void prop_hunt_wheel_press(Client *client) {
    if (!prop_team_me(client)) return;
    sprays_wheel_close(); // sprayWheelClose(), propWheelOpen()
    g_ph.open = 1;
}

void prop_hunt_wheel_release(Client *client) {
    if (!g_ph.open) return;
    if (prop_team_me(client)) {
        const int id = prop_at(g_ph.index); // getPropIndex().id
        if (id >= 0 && client->me->prop_id != id) prop_hunt_send_pick(client, id);
    }
    g_ph.open = 0; // propWheelClose
    g_ph.timer = 0.0f;
}

void prop_hunt_random(Client *client) {
    if (!prop_team_me(client)) return;
    const int n = prop_count();
    if (n <= 0) return;
    const int id = prop_at(rand() % n); // pickRandomProp: randInt(0, length - 1)
    if (client->me->prop_id != id) prop_hunt_send_pick(client, id);
}

void prop_hunt_rotate(Client *client, const int reset) {
    if (prop_team_me(client)) prop_hunt_send_rotate(client, reset);
}

int prop_hunt_wheel_opened(void) {
    return g_ph.open && g_ph.timer >= PROP_WHEEL_DELAY; // propWheelOpened
}

int prop_hunt_wheel_index(void) {
    return g_ph.index;
}

int prop_hunt_scroll(Client *client, const double dy) {
    if (!dy) return 0;
    const int step = dy > 0.0 ? -1 : 1; // the wheel's direction as the spray wheel's
    if (prop_hunt_wheel_opened()) { // propWheelChange: wraps at propWheelMax
        const int max = prop_count() - 1;
        int i = g_ph.index + step;
        if (i > max) i = 0;
        if (i < 0) i = max;
        g_ph.index = i;
        return 1;
    }
    if (prop_hunt_me(client)) { // iîiïìïi += 5 * scrollDelta, clamped to [10, 100]
        g_ph.cam = fminf(PROP_CAM_MAX, fmaxf(PROP_CAM_MIN, g_ph.cam + PROP_CAM_STEP * (float) step));
    }
    return 0;
}

float prop_hunt_camera_distance(void) {
    return g_ph.cam;
}

void prop_hunt_reset(void) {
    g_ph.cam = PROP_CAM_DISTANCE;
    g_ph.open = 0;
    g_ph.timer = 0.0f;
}

// ---------------------------------------------------------------- HUD

void prop_hunt_frame(Client *client, const float delta_seconds) {
    if (g_ph.open && !prop_team_me(client)) g_ph.open = 0, g_ph.timer = 0.0f; // a death closes it
    if (g_ph.open) g_ph.timer += delta_seconds;                              // propWheelUpdate
    if (!html_ui_available() || !html_ui_is_ready()) return;
    // the "0" of our own player: botRightHider hidden and propControlHolder shown for the Prop team
    const int hud = prop_team_me(client);
    if (hud == g_ph.hud) return;
    g_ph.hud = hud;
    const HtmlElem bot = html_dom_by_id("botRightHider");
    if (bot) html_dom_set_display(bot, hud ? "none" : "block");
    const HtmlElem prop = html_dom_by_id("propControlHolder");
    if (prop) html_dom_set_display(prop, hud ? "block" : "none");
}

// ---------------------------------------------------------------- the wheel

static double wheel_ui_scale(const float vw, const float vh) {
    if (ui_setting_is_false("uiScaling")) return 1.0;
    double scale_ui;
    if (!ui_setting_number("scaleUI", &scale_ui) || scale_ui != scale_ui) scale_ui = 0.8;
    const double custom = 2.0 - scale_ui;
    const double sx = vw / (custom * 1700.0), sy = vh / (custom * 900.0);
    return sy < sx ? sy : sx;
}

static C2DImage *prop_thumbnail(Canvas2D *c, const int prop_id) {
    char rel[48];
    snprintf(rel, sizeof(rel), "img/props/prop_%d.png", prop_id); // UTILS.assetsUrl("/img/props/prop_" + id + ".png")
    char *full = concat(ui_default_path(), rel);
    C2DImage *img = full ? c2d_image(c, full) : NULL;
    free(full);
    return img;
}

void prop_hunt_wheel_render(Client *client, const float delta_seconds) {
    (void) delta_seconds;
    const int count = prop_count();
    const int show = prop_hunt_wheel_opened() && count > 0 && prop_team_me(client) && !html_ui_is_menu_open() && !hud_ui_end_screen_up();
    if (!show && !g_ph.drawn) return;

    GLFWwindow *window = headless_current_window();
    int fb_w = 0, fb_h = 0;
    if (window) glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0) return;
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    if (vw <= 0 || vh <= 0) vw = (float) fb_w, vh = (float) fb_h;
    if (!g_ph.canvas) g_ph.canvas = c2d_create();
    Canvas2D *c = g_ph.canvas;
    if (!c) return;
    if (c2d_width(c) != (int) vw || c2d_height(c) != (int) vh) c2d_set_size(c, (int) vw, (int) vh, fb_w, fb_h);
    c2d_clear_rect(c, 0, 0, vw, vh);
    g_ph.drawn = show;
    if (!show) return;

    // game-overlay :160817-160863
    const double scale = wheel_ui_scale(vw, vh);
    const double w = vw / scale, h = vh / scale;
    c2d_save(c);
    c2d_scale(c, scale, scale);
    c2d_translate(c, w / 2, h / 2);
    const double radius = h / 2 * 0.4;
    c2d_set_global_alpha(c, 0.7);
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, radius, 0, M_PI * 2, 0);
    c2d_set_fill_color(c, 0x17 / 255.0f, 0x17 / 255.0f, 0x17 / 255.0f, 1); // #171717
    c2d_fill(c);
    const double ring = radius * 1.33, ring_w = radius / 1.9;
    c2d_set_global_alpha(c, 1);
    c2d_set_line_width(c, ring_w);
    c2d_set_stroke_color(c, 0xcf / 255.0f, 0xcf / 255.0f, 0xcf / 255.0f, 1); // #cfcfcf
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, ring, 0, M_PI * 2, 0);
    c2d_stroke(c);
    const int index = g_ph.index >= 0 && g_ph.index < count ? g_ph.index : 0;
    const double step = M_PI * 2 / count, start = step * index;
    c2d_set_stroke_color(c, 0x55 / 255.0f, 0xac / 255.0f, 0xee / 255.0f, 1); // #55acee: the chosen segment
    c2d_begin_path(c);
    c2d_arc(c, 0, 0, ring, start, start + step, 0);
    c2d_stroke(c);

    const int chosen = prop_at(index);
    const double offset = 50.0;
    // iiíiìïî(prefab.name) (:23872): lowercased, split at "_", each word capitalized, joined by spaces
    char name[32];
    size_t n = 0;
    int word = 1;
    for (const char *p = prop_name(chosen); *p && n + 1 < sizeof(name); p++) {
        if (*p == '_') {
            name[n++] = ' ';
            word = 1;
            continue;
        }
        name[n++] = (char) (word ? toupper((unsigned char) *p) : tolower((unsigned char) *p));
        word = 0;
    }
    name[n] = 0;
    c2d_set_fill_color(c, 0xdb / 255.0f, 0xdb / 255.0f, 0xdb / 255.0f, 1); // #dbdbdb
    c2d_set_font(c, 40, "GameFont");
    c2d_fill_text(c, name, -c2d_measure_text(c, name) / 2, 40.0 / 2 + offset, 0, 0);
    const int max = client->game.mode ? client->game.mode->config.prop_switches : 0;
    char left[48];
    snprintf(left, sizeof(left), "%d switches left", max - client->me->prop_switches);
    c2d_set_fill_color(c, 0xa3 / 255.0f, 0xa3 / 255.0f, 0xa3 / 255.0f, 1); // #a3a3a3
    c2d_set_font(c, 25, "GameFont");
    c2d_fill_text(c, left, -c2d_measure_text(c, left) / 2, 25.0 / 2 + offset + 50, 0, 0);
    C2DImage *img = prop_thumbnail(c, chosen);
    if (img && c2d_image_state(img) == 1) c2d_draw_image(c, img, -64, -64 - 1.5 * offset, 128, 128);
    // img/scroll_2.png beside the ring: v10 ships no such file (the image never loads)

    double angle = -(step / 2);
    for (int i = 0; i < count; i++) {
        angle += step;
        img = prop_thumbnail(c, prop_at(i));
        if (img && c2d_image_state(img) == 1) c2d_draw_image(c, img, ring * cos(angle) - 50, ring * sin(angle) - 50, 100, 100);
    }
    c2d_restore(c);
    c2d_composite(c);
}

void prop_hunt_fini(void) {
    if (g_ph.canvas) c2d_destroy(g_ph.canvas);
    g_ph.canvas = NULL;
    g_ph.drawn = 0;
    g_ph.hud = -1;
}
