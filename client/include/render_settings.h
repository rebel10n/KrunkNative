#pragma once
// Krunker v3.1.1 display/render/game settings on the client side
// (client/src/ui/ui_render_settings.c) and the scaled scene render target
// (client/src/render_target.c).
//
// v3.1.1 reference: Krunker.Unpacked-v3.1.1/134.js (the settings list: every key's
// `set`), 55.js (renderer: setResMlt, aspectRatio/getRes, antiA, postprocessing,
// shadow toggles, camera far), index.js gP() (updateRate frame skipping) and
// bundleFilters() (saturationn/hue/menuSaturation/menuHue CSS filters).
//
// Values arrive through client_apply_setting() (ui_settings.c), which forwards every
// key here first. Keys this file owns return 1; keys the older ClientSettings path
// also carries (resolution, shadows, showFPS, dmgScale, ...) are recorded here and
// return 0 so that path keeps running. The struct starts at v3.1.1's defaults, so a
// key the store never saved still reads its Krunker default.
//
// Readers: the renderer (this file, main.c, render_target.c) and other client systems
// (HUD, VFX, viewmodel, sky) read client_render_settings() each frame; nothing here is
// shared game state.
#include <client.h>

#define RS_COLOR_MAX 16   // "#rrggbb" (Krunker colour inputs)
#define RS_URL_MAX 256    // custom image URLs (mods tab). Stored localized: a Krunker asset URL
                          // (assets.krunker.io / krunker.io) becomes its ui/-relative copy; other
                          // remote URLs stay as-is on the web and are "" natively (no HTTP images)

// KrunkNative-only window modes (no v3.1.1 row: the browser owns fullscreen there).
enum { RS_WINDOWED = 0, RS_FULLSCREEN = 1, RS_BORDERLESS = 2 };

typedef struct {
    // ---- Render tab (cat "quality"/"editing", 134.js)
    float resolution;        // resolution: renderer.setPixelRatio(dpr * v) -> scene FBO scale (0.1..2, default 0.6)
    int frame_cap;           // updateRate: frames per second, 0 = uncapped (index.js gP)
    int aspect_w, aspect_h;  // aspectRatio "WxH": camera aspect override, 0 = window (55.js getRes)
    int anti_alias;          // antiAlias: MSAA on the scene (WebGL context antialias)
    int post_processing;     // postProcessing: composer enabled (CopyShader unless a map shader) - no visible effect
    int low_spec;            // lowSpec: 95.js skips player/weapon detail meshes (flag for player_mesh.c)
    int no_textures;         // noTex (restart): map materials use the "default" texture (flag)
    int map_details;         // mapDet (restart): detail map objects (flag)
    int particles;           // particles: particle manager active (VFX reads ui settings itself)
    float particles_dist;    // particlesDist
    float render_dist;       // renderDist: camera.far = v * 60
    int shadows, soft_shadows, high_res_shadows, dynamic_shadows; // shadow map flags; read by client/src/shadow.c (soft/dynamic not ported yet)
    int ambient_shading;     // ambientShading: ambient_0/1 AO plane groups (not generated yet: flag)
    int show_trails, your_trails, muzzle_flash; // showTrails/yourTrails/muzzleFlash (VFX)
    int bullet_tracers;      // bulletTracers (VFX)
    char bullet_tracer_color[RS_COLOR_MAX];  // bulletTracerCol
    int sniper_flap;         // sniperFlap: scope flap animation on snipers (viewmodel)
    int texture_anim;        // textureAnim: moving/framed textures (main.c client_tick_textures)
    int no_video_skins;      // noVideoSkins (v10.0.3 "Disable Video Skins", restart): skin videos off (skin_video.h)
    int object_anim;         // objectAnim: liquid wave animation (flag)
    int screen_shake;        // screenShake (camera shake, flag)
    int weapon_shine;        // weaponShine: weapons Phong instead of Lambert (flag)
    int lighting;            // lighting (restart): 0 low (Lambert->Basic), 1 normal, 2 high (Phong) (flag)
    char hud_health_high[RS_COLOR_MAX], hud_health_low[RS_COLOR_MAX]; // hudHealthHigh/Low: set(){} in v3.1.1
    float speed_lines;       // showSpdLines: speed line strength (0 = off)
    char speed_lines_color[RS_COLOR_MAX];    // spdLinesCol
    int show_popup_score;    // showPScore
    char popup_score_color[RS_COLOR_MAX];    // scoreColor
    int popup_score_shadow;  // scoreShadow (shadow 3 px when on)
    float popup_score_scale, popup_score_off_x, popup_score_off_y; // scoreScale, scoreOffX/Y (x10 = % in Krunker)
    float saturation, hue;           // saturationn, hue: in-game CSS filter on the 3D canvas (render_target.c)
    float menu_saturation, menu_hue; // menuSaturation, menuHue: CSS filter on #uiBase
    float vignette;          // vignette: #vignette opacity (1 = stylesheet value)
    int feed_limit;          // feedLimit: kill feed rows

    // ---- Display tab (cat "interface"/"chat"/"crosshair"/"hitmarker")
    int show_ui;             // showUI: UI.hideGameUI = !v
    int old_scoreboard;      // oldScoreboard: #leaderDisplay instead of the new board
    int old_browser, disable_rarity_anim, show_pfp; // menu-only flags
    int dynamic_hp;          // dynamicHP (HUD)
    int show_hit_ind;        // showHitInd: directional hit indicators (HUD)
    char hit_color[RS_COLOR_MAX];            // hitColor
    int show_dmg;            // showDMG: damage numbers (HUD)
    int dmg_color;           // dmgColor: colour the damage numbers
    char crit_color[RS_COLOR_MAX];           // critColor
    float dmg_scale;         // dmgScale (HUD: font = v * 30)
    int show_kills, show_kill_c, show_deaths, show_score, show_streak, show_ping, show_net_stat, show_fps; // DOM toggles
    float fps_rate;          // fpsRate: ms between #ingameFPS / #menuFPS rewrites (v10.js:221350, default 100)
    int dmg_type;            // dmgType: 0 "Default" Math.ceil(dmg), 1 "Float" the exact dmg.round(1) (v10.js:221015)
    int damage_number_mode;  // damageNumberDisplayMode (Krunker Strike's Display tab): 0 Stacking, 1 Individual, 2 Legacy (strike_hud_fx.h)
    int show_kd;             // showKD (v7.1.4): #kdCount
    int show_speed;          // showSpeed: speedometer (HUD)
    float speed_off_x, speed_off_y, speed_scale, speed_opacity; // speedOffX/Y, speedScale, speedOpacity
    char speed_color[RS_COLOR_MAX], speed_color_max[RS_COLOR_MAX]; // speedColor, speedColorMax
    int show_medals;         // showMedals
    int hide_names;          // hideNames: 0 everyone, 1 team, 2 enemy, 3 off (nametags)
    float nametag_opacity;   // nametagOpac
    float nametag_scale;     // nametagScale: player tags' ctx.scale *= v (v10.js:159962)
    int nametag_style;       // nametagStyle 0..3
    int health_num;          // healthNum
    char health_col_team[RS_COLOR_MAX], health_col_enemy[RS_COLOR_MAX]; // healthColT/E
    int kpd_vision;          // kpdVision (K.P.D officers spectating a case: the suspect's sight lines, nametags.c)
    char kpd_vision_col[RS_COLOR_MAX]; // kpdVisionCol: the bar colour of a player the suspect can see
    // (v3's xpBarCol / xpBarOpac are gone with the in-game XP bar: v7.1.4 has neither)
    int chat_filter, show_messages, show_unboxings, chat_text_outline; // chat flags (chat owner)
    float chat_opacity, chat_bg_opacity, chat_height, chat_timer; // chatOp (#chatList), chatBGOp, chatHeight (x100 px), chatTimer (s)
    int crosshair_type;      // crosshairSho: 0 off, 1 default, 2 custom, 3 layered, 4 image, 5 precision
    int crosshair_style;     // crosshairStyle 0..4
    char crosshair_image[RS_URL_MAX];        // crosshairImage
    float crosshair_opacity; // crosshairOpacity
    int crosshair_use_opacity, crosshair_always, crosshair_dot;
    char crosshair_color[RS_COLOR_MAX], crosshair_shadow[RS_COLOR_MAX];
    float crosshair_shadow_thickness, crosshair_thick, crosshair_len, crosshair_gap;
    float crosshair_image_w, crosshair_image_h; // crosshairImageW/H: type 4 image size, 0 = natural (v10.js:160545)
    // The throwing crosshair (cat "crosshairm"): replaces the crosshair while fully aimed with an unthrown
    // Combat Knife (v10.js:160506 `aimVal == 0 && canThrow && weapon.canThrow`; hud_ui.c). crosshairAlwaysM
    // and crosshairDotM change nothing in v10 (set to 1 / never read), so they are not kept.
    int crosshair_type_m;    // crosshairShoM: 0 off, 1 preset (crosshairStyleM, the normal colours), 2 image, 3 precision
    int crosshair_style_m;   // crosshairStyleM 0..4 (type 1)
    char crosshair_image_m[RS_URL_MAX];      // crosshairImageM (type 2)
    float crosshair_image_w_m, crosshair_image_h_m; // crosshairImageWM/HM, 0 = natural
    float crosshair_opacity_m; int crosshair_use_opacity_m; // crosshairOpacityM + its crosshairUseOpacityM switch
    char crosshair_color_m[RS_COLOR_MAX], crosshair_shadow_m[RS_COLOR_MAX]; // crosshairColorM / ShadowM (type 3)
    float crosshair_shadow_thickness_m, crosshair_thick_m, crosshair_len_m, crosshair_gap_m;
    int hitmarker;           // hitm: hit markers enabled
    char hitmarker_color[RS_COLOR_MAX], hitmarker_kill_color[RS_COLOR_MAX]; // hitmCol, hitmKCol
    float hitmarker_opacity, hitmarker_length, hitmarker_thickness, hitmarker_spacing; // hitOpac, hitmLen, hitmThick, hitmSpac
    float hitmarker_anim_size, hitmarker_anim_speed, hitmarker_fade; // hitmAnimD, hitmAnimS, hitmFad
    char custom_hitmarker[RS_URL_MAX];       // customHitmarker: #hitmarker img instead of the drawn marker (hud_ui.c)

    // ---- Game tab (cat "gameprivacy"/"viewmodel"/"mods")
    int stream_mode, anon_mode;
    float weapon_bob, weapon_lean, weapon_rot; // weaponBob, weaponLean, weaponRot (viewmodel)
    float weapon_off_x, weapon_off_y, weapon_off_z, weapon_ads_off_y;
    int left_hand, aim_anim, show_hands, round_arms;
    float weap_swap_y;       // weapSwapY (1, -2..2): the swap dip's depth (v10.js:170917 swapTime / swapTime * weapSwapY)
    float weap_reload_y;     // weapReloadY (1, 0..1): how far the weapon lowers while reloading (v10.js:170919)
    int wall_lean;           // wallLean (off): roll the camera on a wall slide / jump (v10.js:221617, camera_feel.c)
    float wall_lean_mlt;     // wallLeanMlt (0.5): lean = 0.15 * v
    int hide_ads;            // hideADS: hide the weapon while fully aimed (main.c)
    int can_load_mods, can_change_logo, auto_load_last;
    int scope_borders;       // scopeBorders: .black scope bars
    char scope_border_color[RS_COLOR_MAX];   // scopeBorderCol: .black background-color (v10.js:207204)
    float scope_border_opacity;              // scopeBorderOpac: .black opacity
    float scope_width, scope_height;         // scopeWidth/Height (6.8): #recticleImg v * 100 px, the bars around it
    float scope_opacity;                     // scopeOpac: the scope image's opacity while scoped (aimRecticle.opacMlt)
    char custom_scope[RS_URL_MAX], custom_ads_dot[RS_URL_MAX];
    float reticle_width, reticle_height; // reticleWidth/Height: #aimDot size (x10 px)
    char end_message[RS_URL_MAX];            // endMessage: sent to the chat once when the match ends (game_events.c)
    char custom_ammo[RS_URL_MAX], custom_kills[RS_URL_MAX], custom_deaths[RS_URL_MAX];
    char custom_streak[RS_URL_MAX], custom_timer[RS_URL_MAX], custom_game_overlay[RS_URL_MAX];
    int use_damage_overlay;  // useDamageOverlay (VFX)
    char custom_damage[RS_URL_MAX];          // customDamage: the blood overlay's image (vfx.c)

    // ---- General tab (cat "local"/"network"/"experimental")
    float lag_comp;          // lagComp slider (settings.tmp.lagCompensation, 0..1): ENT ent[11], the server's player.lagComp
    int net_rerouting, serv_send_r, shader_rendering; // restart-only flags (shaderRendering is set(){} in v3.1.1)

    // ---- KrunkNative-only (no v3.1.1 row; persisted in the same store)
    int vsync;               // "vsync": swap interval 1 natively (the browser always paces to the display)
    int window_mode;         // "windowMode": RS_WINDOWED / RS_FULLSCREEN / RS_BORDERLESS (native)
} ClientRenderSettings;

const ClientRenderSettings *client_render_settings(void);

// A localized image setting's path (custom_damage, ...: ui/-relative) as a file the GL loaders can open
// (the ui/ directory or KRUNKNATIVE_UI_DIR). 0 for "" or a remote URL (the web keeps those for the DOM).
int client_render_ui_file(const char *relative, char *full, size_t n);

// Apply one store value (string form, as client_apply_setting gets it). Returns 1 when
// the key is owned here, 0 to let the caller handle it (unknown or shared keys).
int client_apply_render_setting(Client *client, const char *key, const char *value);

// main.c: once after the window/GL context exist and before the page pushes the store
// (camera far from renderDist, swap interval, environment/CLI overrides).
void client_render_settings_init(Client *client);
// main.c: force a window mode for this session (--windowed/--fullscreen/--borderless);
// a stored "windowMode" is then ignored.
void client_render_force_window_mode(int mode);
// F11: windowed <-> fullscreen (or borderless when that was the chosen mode); persisted.
void client_render_toggle_fullscreen(Client *client);
// The window mode in effect (RS_WINDOWED / RS_FULLSCREEN / RS_BORDERLESS): what the toggle acts on, which
// a forced mode (--windowed etc.) can differ from the stored windowMode. The Strike Display tab's button text.
int client_render_window_mode(void);

// Native window state across F4 / relaunch / restarts (window_state.h). main.c, before
// glfwCreateWindow: reads and clears KRUNKNATIVE_RESTORE_WINDOW, resolves the startup
// window (restore > CLI flag > stored windowMode; KRUNKNATIVE_WINDOW_SIZE > the kept rect)
// and sets the creation size. Returns 1 when the window must be created hidden
// (GLFW_VISIBLE false) for client_render_window_created; enabled = 0 (headless, the scenario
// renderer) only clears the variable. Web: no-ops.
int client_render_window_prepare(int *width, int *height, int enabled);
// Right after glfwCreateWindow: place (the kept position, else centred on the work area:
// client_window_centered_pos), switch to the mode / maximize, then show the window (before any frame),
// and start tracking the windowed rect. It is never shown first and moved after.
void client_render_window_created(Client *client);
// Natively: the client-area position that centres a w x h window (window: its decorations, NULL =
// none) on the work area of the monitor holding (near_x, near_y) when has_near, else the primary
// one (window_state_center). 0 = no position (Wayland, no monitor, the web).
int client_window_centered_pos(GLFWwindow *window, int w, int h, int has_near, int near_x, int near_y, int *x, int *y);
// 1 when the window came from KRUNKNATIVE_RESTORE_WINDOW (its size wins over Resolution).
int client_render_window_restored(void);
// Right before exec (F4 app_reload, the Settings relaunch): KRUNKNATIVE_RESTORE_WINDOW for
// the new process, the kept rect, and the desktop video mode back.
void client_render_window_before_exec(Client *client);
// Normal exit: keep the windowed rect + maximized flag, the desktop video mode back.
void client_render_window_exit(Client *client);

// Frame pacing (main.c client_frame). Web: Krunker's rAF frame skipping - returns 0
// when this animation frame must be skipped. Native: always 1.
int client_render_frame_due(void);
// Native: after the swap - keeps the swap interval in sync with vsync/frame cap and
// sleeps out the rest of a capped frame. Also counts frames for the FPS readouts.
void client_render_frame_end(Client *client);
// Measured frames in the last second (Krunker's cS: timestamps within 1000 ms).
int client_render_fps(void);

// Per frame, after the HUD/menu logic and before the page renders: keeps the DOM
// pieces these settings control (counters, ping, xp bar, vignette, chat opacity, ...)
// in the state the settings ask for, whatever the HUD's visibility pass wrote.
void client_render_settings_tick(Client *client);

// 1 when the in-hand weapon model is hidden by hideADS (fully aimed).
int client_render_hide_weapon_ads(const Client *client);

// ---- scaled scene target (client/src/render_target.c)
// Around the world + first-person passes. begin() binds an offscreen framebuffer at
// resolution x window size (aspectRatio-shaped, MSAA with antiAlias) and returns 1, or
// returns 0 and leaves the default framebuffer bound when no setting needs one.
// end() resolves it and composites it over the whole default framebuffer with linear
// filtering and the saturationn/hue filter; the HUD, overlay and page draw after it at
// native resolution.
int render_target_begin(Client *client);
void render_target_end(Client *client);
void render_target_fini(void);
// Size of the last scene target (0 x 0 when the default framebuffer was used).
void render_target_size(int *width, int *height);
