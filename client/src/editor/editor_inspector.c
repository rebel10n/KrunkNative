// Map editor object panel: Krunker v7.1.4 ui/editor.js `panel.right.object` (UI:1267-5831, the
// object inspector), plus the T-key list pickers (Editor.viewTextureList / viewModelList and
// selectTex / selectModelVariant / selectSpray / selectSkin, E:6813-6957). doc/v7/editor.md WP3.
//
// The panel is built in the blueprint's key order (a JS object: insertion order; a reassigned key
// keeps its first position). Controls bind ObjectInstance properties by name (ed_prop_get /
// ed_prop_set over editor_model.c's table); the renderer (client/src/prefab.c, basic.frag) reads
// the same serialized keys.
//
// Intentional differences from v7.1.4:
// - Folder state keys are the blueprint paths ("object.technical.respawn"), shared by every object
//   of the panel, as v7's folderStore.
// - The Trigger Event folder is shown (WP4's ed_triggers_panel): v7's page never renders it, the
//   hook script's Object.prototype.events accessor swallows `vO234.events`.
// - AI_SPAWNER "Offset Shot" edits the object's shot offset ("shof"): v7 binds the misspelt
//   objConfig.shoOffH, so its switch always shows off and never reaches ObjectInstance.shotOffH.
// - ENVIRONMENT_ZONE "Emissive" shows the zone's skyDomeEmis: v7 has no stringified getter for it
//   and the colour input falls back to its "#ffffff" default.
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ED_HAVE_TRIGGERS_PANEL
// WP4 (editor_triggers.c) defines ED_HAVE_TRIGGERS_PANEL and ed_triggers_panel(): the Trigger
// Event / Trigger Actions folders. Until it lands, the v3-shaped block below stands in for it.
static void ed_triggers_panel_fallback(EdBuild *b, EditorObject *o);
#endif

// config.js constants the generated data does not carry (CFG lines)
static const char *const k_team_names[6] = {"Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"}; // CFG:352
enum { TEAM_LIMIT = 5 };                                                                          // CFG:351
static const char *const k_tone_types[5] = {"NoToneMapping", "LinearToneMapping", "ReinhardToneMapping",
                                            "CineonToneMapping", "ACESFilmicToneMapping"}; // CFG:959
static const char *const k_billboards[7] = {"Random", "Krunkie Loans", "Krunker Japan", "SoonTM", "Be Like Joe", "Moomoo.io",
                                            "Report Hackers"}; // CFG:925
#define BOOST_RANGE 100.0  // CFG:2296
#define SIGN_FONT_MIN 1.0  // CFG:930
#define SIGN_FONT_MAX 240.0 // CFG:924
#define EMITTER_VOL_MIN 0.05
#define EMITTER_VOL_MAX 2.0
#define EMITTER_RATE_MIN 0.1
#define EMITTER_RATE_MAX 4.0
#define EMITTER_DIST_MIN 0.0
#define EMITTER_DIST_MAX 5000.0
#define EMITTER_ROLL_MIN 0.0
#define EMITTER_ROLL_MAX 1.0 // CFG:2313-2320

// ObjectInstance.prototype setters (the group "Key" select of v7.1.4, in prototype order)
static const char *const k_oi_setters[] = {
    "pos", "rot", "size", "sprayTexture", "assetTexture", "texture", "collidable", "visible", "intensity", "angle", "penumbra",
    "decay", "distance", "color", "emissive", "opacity", "forceTrans", "direction", "border", "highlighted", "renderSide",
    "fog", "texOffX", "texOffY", "texRot", "prefab", "texturePrefab", "texStretch", "part", "partC", "partS", "partSP",
    "partG", "partSC", "partD", "partB", "boost", "bounce", "edgeNoise", "team", "single", "weaponId", "weaponReplace",
    "randWep", "poster", "channel", "node", "momentum", "ambient", "hidden", "closeable", "startClosed", "event", "action",
    "constant", "fsize", "fcolor", "bcolor", "text", "backVisible", "tAlign", "cmpSpwn", "classIndx", "respawnT", "respawnR",
    "turnSpd", "speed", "interfaceId", "interfaceT", "withdraw", "deposit", "objectiveId", "faces", "wSpeed", "wHeight",
    "tilesX", "tilesZ", "fullcone", "snd", "vol", "dist", "roll", "rate", "emitfrom", "mSize", "spin", "texSmooth",
    "constantTxt", "forceDir", "constE", "dynamic", "gravity", "itemID", "openEnded", "sides", "leaderOrder", "leaderType",
    "canShoot", "assetID", "aiName", "assetSound", "visDis", "deathSound", "idleSound", "shootSound", "agroSound", "canHit",
    "xrayVis", "frate", "projSpd", "rewardType", "behaviorType", "canDest", "health", "crouch", "canTrigger", "startDest",
    "canWallJump", "chance", "singleP", "actionCost", "spinAxis", "actionMethod", "selfDestruct", "score", "method",
    "penetrable", "movingSpeed", "movingAxis", "interfaceIdT", "teamOnly", "frameCount", "frameSpeed", "trigActions",
    "telCheckPoint", "cast", "receive", "animS", "delay", "projA", "projG", "projSC", "projS", "hbHeight", "hbWidth", "mYOff",
    "projDmg", "roamTime", "roamRad", "mROff", "meldmg", "hrate", "melrng", "enclose", "targAI", "aiChasd", "aicur", "targPlr",
    "shoBrk", "projGL", "distM", "nodeId", "pathInput", "pathType", "vdrag", "vangDrag", "vturnSpd", "vturnLean", "vmaxPow",
    "vpowerF", "vmaxRev", "vrevF", "vRange", "lightTarget", "interactM", "stype", "startAnim", "forceRender", "minimum",
    "texScale", "static", "scriptId", "texEncode", "camType", "forceCam", "followPath", "canGrapple", "leaderTypeCust",
    "radiusTop", "radiusBottom", "thetaLength", "texVar", "alphaTest", "ocolor", "othick", "mimicSprite", "spread", "shoArY",
    "shoDel", "melDel", "projMS", "aiLvl", "aiTagScale", "melSound", "melHitSound", "shootHitSound", "friendly", "melShake",
    "shoShake", "critDmg", "playerCollidable", "stepSrc", "skyDome", "skyDomeCol0", "skyDomeCol1", "skyDomeCol2", "skyDomeEmis",
    "skyDomeEmisTex", "skyDomeTex", "skyDomeTexA", "skyDomeMovD", "skyDomeMovT", "shadowR", "shadowD", "ambientC", "ambientI",
    "light", "lightD", "lightI", "sunAngX", "sunAngY", "sky", "fogC", "fogD", "correctLights", "toneMapping",
    "toneMappingExposure", "outputEncoding", "spawnChance", "explode", "complexColl", "objLimit", "label", "eth", "canImpulse",
    "thetaStart", "bulletColl", "lightC", "hitEff", "shotEff", "noSCF", "leaderResults", "darken", "aiColl", "perk",
    "ignoreOnStuck", "bobS", "bobD", "pMov", "nodeTag", "projT", "driftOnStuck", "showPickupIcon", "showMinimap", "noTexture",
    "botName", "botClass", "botForceClass", "botPrimSkin", "botDye", "botFace", "botHat", "botHead", "botBack", "botBody",
    "botWaist", "botWrist", "botShoes", "botPet", "botLives", "botMelee", "botTargetBots", "botTargetTeam", "botForceCrouch",
    "botAttackTarget", "botFollowTarget", "botWatchTarget", "botMimicTarget", "botSlideHop", "botCanRespawn",
    "botUseSpawnDelay", "botRemoveOnDeath", "botNodYes", "botNodNo", "botJoinMessage", "botLeaveMessage", "botVoiceChat",
    "botGodMode", "botRoam", "botRandomSpawn", "botTeam", "healthMlt", "dmgMlt", "spreadMlt", "botRecoil", "randWepGroup",
    "objectiveLink", "rangeMlt", "speedMlt", "invincibleOnStuck", "rewardMethod", "shotOffH", "rewardReq", "rewardNode",
    "anchorType", "botNodeGroup", "botNode", "botNodeNext", "botNodeRev",
};

// ---------------------------------------------------------------------------
// option lists (Editor.generateLists, E:1712)

static const cJSON *data_list(const char *key) { return editor_data_get(key); }

// textureList: Default first, then formatConstName(alias || name) -> texture name
static const EdOption *texture_list(int *count) {
    static EdOption opts[EDITOR_TEXTURE_COUNT + 1];
    static char names[EDITOR_TEXTURE_COUNT + 1][48];
    static int n = -1;
    if (n < 0) {
        n = 0;
        opts[n++] = (EdOption) {"Default", 0, "DEFAULT"};
        const cJSON *t;
        cJSON_ArrayForEach(t, data_list("textures")) {
            const char *name = ed_json_str(t, "name", "");
            if (!strcmp(name, "DEFAULT") || n > EDITOR_TEXTURE_COUNT) continue;
            edp_const_name(ed_json_str(t, "alias", name), names[n], sizeof(names[n]));
            opts[n] = (EdOption) {names[n], 0, name};
            n++;
        }
    }
    *count = n;
    return opts;
}

static const cJSON *texture_entry(const char *name) {
    const cJSON *t;
    cJSON_ArrayForEach(t, data_list("textures")) if (!strcmp(ed_json_str(t, "name", ""), name)) return t;
    return NULL;
}

static int texture_index_of(const char *name) {
    for (int i = 0; i < EDITOR_TEXTURE_COUNT; i++) if (!strcmp(editor_textures[i], name)) return i;
    return -1;
}

// options from a JSON list of names (or of {name} objects), value = index; formatConstName when asked
static EdOption *names_options(const cJSON *list, const int const_name, int *count) {
    const int n = cJSON_GetArraySize(list);
    EdOption *o = edp_opts_alloc(n);
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(list, i);
        const char *s = cJSON_IsString(it) ? it->valuestring : ed_json_str(it, "name", "");
        if (const_name) {
            const int slot = (int) (&o[i] - g_opt_pool);
            s = edp_const_name(s, g_opt_names[slot], sizeof(g_opt_names[slot]));
        }
        o[i] = (EdOption) {s, i, NULL};
    }
    *count = n;
    return o;
}

// store.skins (assets/skins.json, index order)
static cJSON *g_skins;
static int g_skins_loaded;

static const cJSON *skins(void) {
    if (!g_skins_loaded) {
        g_skins_loaded = 1;
        char *path = concat(assets_path(), "skins.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) g_skins = cJSON_ParseWithLength((const char *) buf, len);
        free(buf);
        free(path);
    }
    return g_skins;
}

enum { IT_SPRAY = 4, IT_DYE = 5, IT_COLLECTIBLE = 10, IT_WRIST = 11, IT_TICKET = 13, IT_PLAYER_CARD = 16 }; // data/itemTypes.js

static int skin_listed(const int type) {
    return !type || (type != IT_SPRAY && type != IT_DYE && type != IT_COLLECTIBLE && type != IT_WRIST && type != IT_TICKET && type != IT_PLAYER_CARD);
}

// Editor.getItemDesc: the i18n market label without " Item", else the weapon's name
static const char *item_desc(const cJSON *skin) {
    static const char *const k_types[17] = {NULL, "Hat", "Body", "Melee", "Spray", "Dye", "Waist", "Face", "Shoe", "Pet",
                                            "Collectible", "Wrist", "Charm", "Spin Ticket", "Back", "Head", "Player Card"};
    const int type = (int) ed_json_num(skin, "type", 0);
    if (type > 0 && type < 17) return k_types[type];
    return ed_json_str(cJSON_GetArrayItem(data_list("weapons"), (int) ed_json_num(skin, "weapon", 1) - 1), "name", "");
}

// skinList: an object keyed by "<name> [<desc>]" (a repeated label keeps its first position and
// takes the later index); sprayList: every spray as "<name> [<index>]" -> index
static EdOption *g_skin_opts, *g_spray_opts;
static char **g_skin_labels;
static int g_skin_count = -1, g_spray_count, g_skin_label_count;

static void build_skin_lists(void) {
    if (g_skin_count >= 0) return;
    g_skin_count = g_spray_count = 0;
    const cJSON *s = skins();
    const int n = cJSON_GetArraySize(s);
    if (n <= 0) return;
    g_skin_opts = calloc((size_t) n, sizeof(EdOption));
    g_spray_opts = calloc((size_t) n, sizeof(EdOption));
    g_skin_labels = calloc((size_t) n * 2, sizeof(char *));
    if (!g_skin_opts || !g_spray_opts || !g_skin_labels) return;
    for (int i = 0; i < n; i++) {
        const cJSON *t = cJSON_GetArrayItem(s, i);
        const int type = (int) ed_json_num(t, "type", 0);
        const char *name = ed_json_str(t, "name", "");
        char label[256];
        if (type == IT_SPRAY) {
            const int index = (int) ed_json_num(t, "index", 0);
            snprintf(label, sizeof(label), "%s [%d]", name, index);
            char *l = strdup(label);
            if (!l) continue;
            g_skin_labels[g_skin_label_count++] = l;
            g_spray_opts[g_spray_count++] = (EdOption) {l, index, NULL};
        }
        if (!skin_listed(type)) continue;
        snprintf(label, sizeof(label), "%s [%s]", name, item_desc(t));
        int dup = -1;
        for (int k = g_skin_count - 1; k >= 0 && dup < 0; k--) if (!strcmp(g_skin_opts[k].name, label)) dup = k;
        if (dup >= 0) {
            g_skin_opts[dup].value = i;
            continue;
        }
        char *l = strdup(label);
        if (!l) continue;
        g_skin_labels[g_skin_label_count++] = l;
        g_skin_opts[g_skin_count++] = (EdOption) {l, i, NULL};
    }
}

// ---------------------------------------------------------------------------
// the selection

static int is_group_owner(const EditorObject *o) {
    // Object.keys(objGroups).includes(owner.boundingMesh.uuid)
    for (size_t i = 0; o && i < g_ed.proj->group_count; i++) if (g_ed.proj->groups[i].owner == o->uuid) return 1;
    return 0;
}

static double prop_num(const EditorObject *o, const char *prop) {
    cJSON *v = ed_prop_get(o, prop);
    const double r = cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : cJSON_IsString(v) ? atof(v->valuestring) : 0;
    cJSON_Delete(v);
    return r;
}

static int prop_truthy(const EditorObject *o, const char *prop) {
    cJSON *v = ed_prop_get(o, prop);
    const int r = ed_json_truthy(v) && !(cJSON_IsString(v) && !strcmp(v->valuestring, "0"));
    cJSON_Delete(v);
    return r;
}

// triggers.events[o.event].name == name
static int event_is(const EditorObject *o, const char *name) {
    const cJSON *ev = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(data_list("triggers"), "events"), (int) prop_num(o, "event"));
    return ev && !strcmp(ed_json_str(ev, "name", ""), name);
}

// ---------------------------------------------------------------------------
// onChange handlers

static void refresh_panel(EdCtl *c, const cJSON *v) { (void) c; (void) v; g_ed.dirty_panel = 1; } // updateObjConfigGUI

static void regen_refresh(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    if (g_ed.proj->selected) ed_obj_rebuild(g_ed.proj->selected);
    g_ed.dirty_panel = 1;
}

static void on_faces(EdCtl *c, const cJSON *v) {
    // object: objConfig.faces, key: index -> A.faces = objConfig.faces; A.regen()
    EditorObject *o = g_ed.proj->selected;
    if (!o) return;
    cJSON *faces = ed_prop_get(o, "faces");
    cJSON_ReplaceItemInArray(faces, c->arg, cJSON_CreateBool(ed_json_truthy(v)));
    ed_prop_set(o, "faces", faces);
    cJSON_Delete(faces);
    ed_obj_rebuild(o);
    ed_mark_changed();
}

static void on_size(EdCtl *c, const cJSON *v) {
    // noScaleX: size[0] = prefab.defaultSize[0] (noScaleY: no v7.1.4 prefab sets it)
    (void) c; (void) v;
    EditorObject *o = g_ed.proj->selected;
    if (!o || !editor_prefab_has(o->prefab, EP_NOSCALEX)) return;
    cJSON *s = ed_prop_get(o, "size");
    cJSON_ReplaceItemInArray(s, 0, cJSON_CreateNumber(editor_prefabs[o->prefab].default_size[0]));
    ed_prop_set(o, "size", s);
    cJSON_Delete(s);
    ed_obj_rebuild(o);
    g_ed.dirty_panel = 1;
}

static void on_texture_type(EdCtl *c, const cJSON *v) {
    // assetTexture = 0; texture = value (ed_prop_set "texture" resets "at" and "tv"); updateObjConfigGUI
    regen_refresh(c, v);
}

static void on_interface(EdCtl *c, const cJSON *v) {
    // warningCheck("interface") + checkConnections: the counts / lines are rebuilt from the objects
    (void) c; (void) v;
    g_ed.dirty_counts = 1;
}

static void on_env_sky(EdCtl *c, const cJSON *v) {
    // ENVIRONMENT_ZONE Sky select: ensky_<value> shown, the other hidden; skyDome = !!parseInt(value)
    (void) c;
    const int dome = (int) cJSON_GetNumberValue(v) != 0;
    html_dom_set_display(html_dom_by_id("ensky_0"), dome ? "none" : "");
    html_dom_set_display(html_dom_by_id("ensky_1"), dome ? "" : "none");
}

static void on_env_cloud(EdCtl *c, const cJSON *v) {
    (void) c;
    html_dom_set_display(html_dom_by_id("encloud"), ed_json_truthy(v) ? "" : "none");
}

// advancedOptions buttons of the panel (the select* asset browsers are editor_net.c's;
// c->arg: ed_net_select's codes; editor_net.c opens the Assets / Textures / Sounds window)
enum {
    SEL_MODEL = 0, SEL_TEXTURE = 1, SEL_AUDIO = 2, SEL_DEATH = 3, SEL_IDLE = 4, SEL_ATTACK = 5, SEL_AGRO = 6,
    SEL_SHOOT_HIT = 8, SEL_MELEE, SEL_MELEE_HIT, SEL_PROJ_MODEL, SEL_CLOUD_EN, SEL_EMISSIVE_CLOUD_EN, SEL_AMB_EN,
};

static void select_button(EdBuild *b, const char *label, const int code) { ed_gui_button(b, label, ed_net_select)->arg = code; }

static void group_color_in(void *user, const char *value) {
    (void) user;
    if (!value || !*value) return;
    cJSON *v = cJSON_CreateString(value);
    ed_group_edit("color", v);
    cJSON_Delete(v);
}

static void act_group_color(EdCtl *c) { (void) c; ed_prompt("Input color", "", group_color_in, NULL); } // advancedOptions.editColor

static void group_custom_in(void *user, const char *value) {
    (void) user;
    if (!value || !*value) return;
    // eval(prompt) when it parses, else the raw string (editCustomExec)
    cJSON *v = cJSON_Parse(value);
    if (!v) v = cJSON_CreateString(value);
    const char *key = ed_json_str(g_ed.advanced, "editCustomKey", "");
    if (*key) ed_group_edit(key, v);
    cJSON_Delete(v);
}

static void act_group_custom(EdCtl *c) { (void) c; ed_prompt("Input value", "", group_custom_in, NULL); }
static void act_group_reflect(EdCtl *c) { (void) c; ed_group_reflect(); } // advancedOptions.groupReflect
static void on_group_texture(EdCtl *c, const cJSON *v) { (void) c; ed_group_edit("texture", v); } // editGroup("texture", value)
static void act_texture_list(EdCtl *c) { (void) c; editor_texture_list(); }                        // viewTextureList
static void act_model_list(EdCtl *c);                                                              // viewModelList

// ---------------------------------------------------------------------------
// control helpers (object-bound unless noted)

static EdBuild *B;
static EditorObject *O;

static EdCtl *sw(const char *name, const char *prop, const int sub, const EdChangeFn fn) { return edp_obj_switch(B, name, prop, sub, fn); }
static EdCtl *sl(const char *name, const char *prop, const double mn, const double mx, const double st, const int sub, const EdChangeFn fn) {
    return edp_obj_slider(B, name, prop, mn, mx, st, sub, fn);
}
static EdCtl *sel(const char *name, const char *prop, const EdOption *opts, const int n, const int sub, const EdChangeFn fn) {
    return edp_obj_select(B, name, prop, opts, n, sub, fn);
}
static EdCtl *col(const char *name, const char *prop, const int sub, const EdChangeFn fn) {
    EdCtl *c = edp_obj_color(B, name, prop, sub);
    c->on_change = fn;
    return c;
}

// a text field: v7 shows "" for an unset value (undefined), the value otherwise
static EdCtl *txt(const char *name, const char *prop, const char *placeholder, const int sub, const EdChangeFn fn, const int zero_blank) {
    EdCtl *c = ed_gui_text(B, name, placeholder, sub);
    cJSON *v = ed_prop_get(O, prop);
    cJSON *tmp = cJSON_CreateObject();
    if (zero_blank && ((cJSON_IsNumber(v) && v->valuedouble == 0) || cJSON_IsNull(v))) cJSON_AddStringToObject(tmp, "v", "");
    else cJSON_AddItemToObject(tmp, "v", cJSON_Duplicate(v, 1));
    ed_ctl_bind_json(c, tmp, "v");
    edp_finish(B, c, fn);
    ed_ctl_bind_obj(c, prop); // the markup is written; edits go to the object
    c->json = NULL;
    cJSON_Delete(tmp);
    cJSON_Delete(v);
    return c;
}

static EdCtl *num(const char *name, const char *prop, const char *placeholder, const int sub, const EdChangeFn fn) {
    EdCtl *c = ed_gui_number(B, name, placeholder, 0, sub);
    ed_ctl_bind_obj(c, prop);
    return edp_finish(B, c, fn);
}

static EdCtl *v3(const char *name, const char *prop, const EdChangeFn fn) { return edp_obj_vector3(B, name, prop, fn); }

// a select over a JSON blob (advancedOptions); string options when the first one has svalue
static EdCtl *json_sel(const char *name, cJSON *obj, const char *key, const EdOption *opts, const int n, const int sub, const EdChangeFn fn) {
    return edp_json_select(B, name, obj, key, opts, n, sub, fn);
}

static void fold(const char *name, const int size, const int closed, const int sub, const char *key) {
    char k[96];
    snprintf(k, sizeof(k), "object.%s", key);
    ed_gui_folder_begin(B, name, size, closed, sub, k);
}
static void fend(const int size) { ed_gui_folder_end(B, size); }
static void rule(void) { ed_gui_rule(B); }

// a block that is only kept when it emitted something (v7: `if (Object.keys(x).length)`)
static EdBuf block_begin(void) {
    EdBuf saved = B->html;
    edb_init(&B->html);
    return saved;
}
static EdBuf block_end(const EdBuf saved) {
    EdBuf inner = B->html;
    B->html = saved;
    return inner;
}
static void block_put(EdBuf *inner) {
    edb_add(&B->html, inner->buf ? inner->buf : "");
    edb_free(inner);
}

static EdOption g_axes[2] = {{"X", 0, NULL}, {"Y", 1, NULL}};

// ---------------------------------------------------------------------------
// header (UI:1288-1972)

static void build_header(const EditorPrefab *pf) {
#define HAS(f) ((pf->flags & (f)) != 0)
#define IS(n) (!strcmp(pf->name, n))
    if (HAS(EP_AI)) txt("AI Name", "aiName", "BOT", 0, NULL, 0);
    if (HAS(EP_BOT)) txt("Bot Name", "botName", "Gnomie", 0, NULL, 0);
    if (!HAS(EP_TOOL) || HAS(EP_BOUNCE)) sw("Visible", "visible", 0, NULL);
    if (!HAS(EP_TOOL) && !HAS(EP_FORCECOLLISION) && !HAS(EP_NOCOLLISION)) sw("Collidable", "collidable", 0, NULL);
    if (HAS(EP_CANCOMPLEXCOLL)) sw("Complex Collisions", "complexColl", 0, NULL);
    if (IS("TRIGGER")) sw("Player Collidable", "playerCollidable", 0, NULL);
    if (HAS(EP_EDITBULLETCOL)) sw("Bullet Collisions", "bulletColl", 0, NULL);
    if (HAS(EP_EDITAICOL)) sw("AI Collisions", "aiColl", 0, NULL);
    if (HAS(EP_EDITPEN)) sl("Penetrable", "penetrable", 0, 1, 0.01, 0, NULL);
    if (HAS(EP_CANWALLJUMP)) sw("Wall Jumpable", "canWallJump", 0, NULL);
    if (HAS(EP_CANGRAPPLE)) sw("Can Grapple", "canGrapple", 0, NULL);
    if (HAS(EP_CANIMPULSE)) sw("Can Impulse", "canImpulse", 0, NULL);
    if (HAS(EP_BOOSTABLE)) sl("Boost", "boost", -BOOST_RANGE, BOOST_RANGE, 0.01, 0, NULL);
    if (HAS(EP_BOUNCE)) {
        sl("Bounce", "bounce", -3, 3, 0.1, 0, NULL);
        sw("Crouch Reduces", "crouch", 0, NULL);
    }
    if (HAS(EP_EDGENOISE)) sl("Edge Noise", "edgeNoise", -5, 5, 0.1, 0, NULL);
    if (HAS(EP_HASBORDER)) sw("Border", "border", 0, NULL);
    if (HAS(EP_SHOWMINIMAP)) sw("Show On Minimap", "showMinimap", 0, NULL);
    if (IS("MAP_ANCHOR")) {
        static const EdOption a[] = {{"Corner", 0, NULL}, {"Center", 1, NULL}};
        sel("Anchor Type", "anchorType", a, 2, 0, NULL);
    }
    if (IS("KR_ZONE")) sl("Minimum Amount", "minimum", 0, 10000000, 100, 0, NULL);
    if (HAS(EP_GEN) || HAS(EP_HASSPIN)) {
        sl("Spin", "spin", -20, 20, 0.1, 0, NULL);
        static const EdOption a[] = {{"Y", 0, NULL}, {"X", 1, NULL}};
        if (HAS(EP_GEN) || HAS(EP_HASSPINDIR)) sel("Spin Axis", "spinAxis", a, 2, 0, NULL);
    }
    if (IS("CYLINDER")) {
        sw("Open Ended", "openEnded", 0, NULL);
        sl("Radius Top", "radiusTop", 0, 1, 0.01, 0, NULL);
        sl("Radius Bottom", "radiusBottom", 0, 1, 0.01, 0, NULL);
    }
    if (IS("LIGHT_CONE") || IS("CYLINDER")) sl("Sides", "sides", 3, 64, 1, 0, NULL);
    if (IS("CYLINDER")) sl("Theta Start", "thetaStart", 0, 360, 0.1, 0, NULL);
    if (IS("LIGHT_CONE") || IS("CYLINDER")) sl("Theta Length", "thetaLength", 0.1, 360, 0.1, 0, NULL);
    if (IS("LEADERBOARD")) {
        static const EdOption t[] = {{"Score", 0, NULL}, {"Time", 1, NULL}};
        static const EdOption o[] = {{"ASC", 0, NULL}, {"DESC", 1, NULL}};
        sel("Leader Type", "leaderType", t, 2, 0, NULL);
        txt("Leader Type Custom", "leaderTypeCust", "", 0, NULL, 0);
        sel("Leader Order", "leaderOrder", o, 2, 0, NULL);
        sl("Leader Results", "leaderResults", 1, 50, 1, 0, NULL);
    }
    if (HAS(EP_CUSTOMDIRECTION)) sl("Direction", "direction", 0, HAS(EP_EXTRADIRECTION) ? 7 : HAS(EP_ALTDIRECTION) ? 5 : 3, 1, 0, NULL);
    if (HAS(EP_OPTIONALDIRECTION)) sw("Force Direction", "forceDir", 0, NULL);
    if (HAS(EP_TEAMABLE)) {
        // (noDefault ? 1 : 0) + teamLimit teams: Default, Alpha.. or Alpha..Foxtrot; canNeutral adds Neutral -1
        const int n = (HAS(EP_NODEFAULT) ? 1 : 0) + TEAM_LIMIT;
        EdOption *t = edp_opts_alloc(n + 1);
        for (int i = 0; i < n; i++) t[i] = (EdOption) {HAS(EP_NODEFAULT) ? k_team_names[i] : i == 0 ? "Default" : k_team_names[i - 1], i, NULL};
        int m = n;
        if (HAS(EP_CANNEUTRAL)) t[m++] = (EdOption) {"Neutral", -1, NULL};
        sel("Team", "team", t, m, 0, NULL);
    }
    if (IS("SERVER_NODE")) {
        txt("Tag", "nodeTag", "", 0, NULL, 0);
        sl("Max Spread", "spread", 0, 50, 0.1, 0, NULL);
    }
    if (IS("RAMP")) sw("Enclose", "enclose", 0, NULL);
    if (IS("CAMERA_POSITION")) sw("Static Cam", "static", 0, NULL);
    if (IS("PATH_NODE")) sl("Node Id", "nodeId", 0, 1000, 1, 0, NULL);
    if (IS("PRIZE_WHEEL")) {
        static const EdOption s[] = {{"Starter", 0, NULL}, {"Elite", 1, NULL}, {"Heroic", 2, NULL}};
        sel("Spin Type", "stype", s, 3, 0, NULL);
    }
    if (HAS(EP_GEN)) {
        sl("Model Scale", "mSize", 0.01, 100, 0.01, 0, NULL);
        if (HAS(EP_AI)) {
            sl("Model Y Offset", "mYOff", -100, 100, 0.1, 0, NULL);
            sl("Model Direction", "mROff", 0, 360, 1, 0, NULL);
        }
    }
    build_skin_lists();
    if (HAS(EP_SPRAYTEXTURE)) {
        EdCtl *c = ed_gui_select_click(B, "Spray", g_spray_opts, g_spray_count, 1, "dashboard_customize", act_texture_list);
        ed_ctl_bind_obj(c, "sprayTexture");
        edp_finish(B, c, regen_refresh);
    }
    if (IS("SHOWCASE")) {
        EdCtl *c = ed_gui_select_click(B, "Item", g_skin_opts, g_skin_count, 0, "dashboard_customize", act_texture_list);
        ed_ctl_bind_obj(c, "itemID");
        edp_finish(B, c, regen_refresh);
    }
    if (HAS(EP_CANSETOBJECTIVE)) sl("Objective Id", "objectiveId", 0, 10, 1, 0, NULL);
    if (IS("CAMERA")) {
        int n;
        const EdOption *t = names_options(data_list("cameraTypes"), 0, &n);
        sel("Camera Type", "camType", t, n, 0, refresh_panel);
        const int type = (int) prop_num(O, "camType");
        if (type == 0) sl("Objective Link", "objectiveLink", -1, 10, 1, 0, NULL);
        else if (type == 1) {
            sw("Force Player Cam", "forceCam", 0, NULL);
            txt("Follow Path", "followPath", "id,id,id", 0, NULL, 0);
        }
    }
    if (HAS(EP_STEPSRC)) {
        int n;
        const EdOption *s = names_options(data_list("stepSounds"), 0, &n);
        sel("Step Sound", "stepSrc", s, n, 0, NULL);
    }
#undef HAS
#undef IS
}

// ---------------------------------------------------------------------------
// Transform / Style (UI:1974-2554)

static void build_transform(const EditorPrefab *pf, const int group) {
    fold("Transform", 0, !group, 0, "transform");
    v3("Position", "pos", NULL);
    if (!(pf->flags & EP_NOROTATION)) v3("Rotation", "rot", NULL);
    if ((pf->flags & EP_SCALABLE) && !prop_truthy(O, "assetID")) v3("Size", "size", on_size);
    if (group) {
        static const EdOption dirs[] = {{"X", 0, NULL}, {"Y", 1, NULL}, {"Z", 2, NULL}};
        fold("Reflect", 1, 0, 0, "transform.reflect");
        json_sel("Direction", g_ed.advanced, "groupReflectDir", dirs, 3, 1, NULL);
        ed_gui_button(B, "Execute", act_group_reflect);
        fend(1);
    }
    fend(0);
}

static void build_style(const EditorPrefab *pf, const int group) {
#define HAS(f) ((pf->flags & (f)) != 0)
#define IS(n) (!strcmp(pf->name, n))
    if (HAS(EP_NOTSTYLEABLE)) return;
    const int spray = HAS(EP_SPRAYTEXTURE);
    const EdBuf saved = block_begin();
    int tn;
    const EdOption *tex = texture_list(&tn);
    int color_done = 0;
    if (group) {
        EdCtl *t = json_sel("Texture", g_ed.advanced, "texture", tex, tn, 0, on_group_texture);
        t->value_is_string = 1;
        // vO233.color: the group's Open button, replaced in place by the colour control below
        if (HAS(EP_EDITCOLOR)) col("Color", "color", 0, NULL);
        else ed_gui_button_line(B, "Color", "Open", act_group_color, 0);
        color_done = 1;
    }
    if ((HAS(EP_TEXTURABLE) || HAS(EP_MOVINGTEXTURE) || HAS(EP_FRAMETEXTURE)) && !spray) {
        fold("Texture", 1, 1, 0, "style.texture");
        if (HAS(EP_TEXTURABLE)) {
            EdCtl *c = ed_gui_select_click(B, "Type", tex, tn, 1, "dashboard_customize", act_texture_list);
            ed_ctl_bind_obj(c, "texture");
            c->value_is_string = 1;
            edp_finish(B, c, on_texture_type);
            // texturePrefab.variants
            cJSON *tname = ed_prop_get(O, "texture");
            const cJSON *variants = cJSON_GetObjectItemCaseSensitive(texture_entry(cJSON_IsString(tname) ? tname->valuestring : ""), "variants");
            cJSON_Delete(tname);
            if (cJSON_GetArraySize(variants) > 0) {
                int n;
                const EdOption *v = names_options(variants, 0, &n);
                sel("Variant", "texVar", v, n, 1, regen_refresh);
            }
            if (HAS(EP_ASSETTEXTURES)) {
                txt("Asset ID", "assetTexture", "Texture Asset ID", 1, regen_refresh, 0);
                select_button(B, "Select Custom", SEL_TEXTURE);
            }
            if (HAS(EP_CANSTRETCH)) {
                sw("Stretch", "texStretch", 1, NULL);
                sl("Scale", "texScale", 0, 10, 0.01, 1, NULL);
            }
        }
        if ((HAS(EP_TEXTURABLE) || IS("AI_SPAWNER")) && HAS(EP_CANTRANS)) sw("Force Transparency", "forceTrans", 1, NULL);
        if (HAS(EP_TEXTURABLE)) {
            sl("Rotation", "texRot", 0, 360, 1, 1, NULL);
            fold("Offset", 1, 1, 1, "style.texture.offset");
            sl("X", "texOffX", 0, 1, 0.01, 2, NULL);
            sl("Y", "texOffY", 0, 1, 0.01, 2, NULL);
            fend(1);
        }
        if (HAS(EP_MOVINGTEXTURE)) {
            fold("Movement", 1, 1, 1, "style.texture.movement");
            sl("Speed", "movingSpeed", -20, 20, 0.1, 2, NULL);
            sel("Axis", "movingAxis", g_axes, 2, 2, NULL);
            fend(1);
        }
        if (HAS(EP_FRAMETEXTURE)) {
            fold("Frame Animation", 1, 1, 1, "style.texture.frame");
            sl("Frame Count", "frameCount", 0, 100, 1, 2, NULL);
            sl("Frame Speed", "frameSpeed", 0, 10, 0.1, 2, NULL);
            fend(1);
        }
        rule();
        fend(1);
    }
    if (HAS(EP_AI) || IS("CUSTOM_ASSET") || IS("TRIGGER")) {
        txt("Asset ID", "assetID", "ID", 1, regen_refresh, 1);
        select_button(B, "Select Custom", SEL_MODEL);
    }
    if (pf->variants > 0) {
        // model variants: selectClick, the T-key model list on the icon
        char id[16];
        snprintf(id, sizeof(id), "%d", O->prefab);
        int n = 0;
        const EdOption *v = names_options(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(data_list("prefabVariants"), id), "names"), 0, &n);
        EdCtl *c = ed_gui_select_click(B, "Variant", v, n, 0, "dashboard_customize", act_model_list);
        ed_ctl_bind_obj(c, "texVar");
        edp_finish(B, c, regen_refresh);
    }
    if (!spray) {
        int n;
        const EdOption *e = names_options(data_list("encodingTypes"), 0, &n);
        sel("Encoding", "texEncode", e, n, 0, NULL);
    }
    if (HAS(EP_EDITCOLOR) && !color_done) col("Color", "color", 0, NULL);
    if (HAS(EP_EDITEMISSIVE)) col("Emissive", "emissive", 0, NULL);
    if (HAS(EP_EDITOPAC)) sl("Opacity", "opacity", 0, 1, 0.1, 0, NULL);
    if (HAS(EP_CUSTOM) || IS("TRIGGER") || IS("AI_SPAWNER")) sw("Texture Smoothing", "texSmooth", 0, NULL);
    if (!HAS(EP_COMPLEX) && !HAS(EP_TOOL)) sw("Shading", "ambient", 0, NULL);
    if (HAS(EP_EDITFOG)) sw("Fog", "fog", 0, NULL);
    if (IS("CUSTOM_ASSET") || IS("TRIGGER")) {
        sw("Cast Shadows", "cast", 0, NULL);
        sw("Receive Shadows", "receive", 0, NULL);
        sw("Force Render", "forceRender", 0, NULL);
    }
    if (HAS(EP_EDITALPHATEST)) sl("AlphaTest", "alphaTest", 0, 1, 0.01, 0, NULL);
    if (HAS(EP_EDITRENDERSIDE)) {
        static const EdOption s[] = {{"Game Default", -1, NULL}, {"Front", 0, NULL}, {"Back", 1, NULL}, {"Double", 2, NULL}};
        sel("Render Side", "renderSide", s, 4, 0, NULL);
    }
    if (IS("AI_SPAWNER")) {
        sl("Bob Distance", "bobD", 0, 10, 0.01, 0, NULL);
        sl("Bob Speed", "bobS", 0, 10, 0.01, 0, NULL);
    }
    if (IS("LIGHT_CONE")) sw("Full Cone", "fullcone", 0, NULL);
    if (HAS(EP_CANSPRITE)) sw("Mimic Sprite", "mimicSprite", 0, NULL);
    EdBuf inner = block_end(saved);
    if (inner.len) {
        fold("Style", 0, !group, 0, "style");
        block_put(&inner);
        fend(0);
    } else edb_free(&inner);
#undef HAS
#undef IS
}

// ---------------------------------------------------------------------------
// Technical & Logic (UI:2555-4625)

static void build_ai(void) {
    // Behavior .. Nametag (UI:2863-3536)
    fold("Behavior", 1, 1, 0, "technical.behavior");
    sw("Ignore When Stuck", "ignoreOnStuck", 1, NULL);
    sw("Drift When Stuck", "driftOnStuck", 1, NULL);
    sw("Invincible When Stuck", "invincibleOnStuck", 1, NULL);
    sw("Friendly", "friendly", 1, NULL);
    sw("Target Players", "targPlr", 1, NULL);
    sw("Target other AI", "targAI", 1, NULL);
    sl("Behavior Type", "behaviorType", 0, 20, 1, 1, NULL);
    sl("Chase Distance", "aiChasd", 0, 2000, 1, 1, NULL);
    sl("Roam Radius", "roamRad", 0, 500, 1, 1, NULL);
    sl("Roam Time", "roamTime", 1000, 10000, 1, 1, NULL);
    int n;
    const EdOption *paths = names_options(data_list("pathTypes"), 0, &n);
    sel("Path Type", "pathType", paths, n, 1, NULL);
    txt("Path Input", "pathInput", "id,id,id", 1, NULL, 0);
    rule();
    fend(1);

    fold("Movement", 1, 1, 0, "technical.mov");
    sl("Speed", "speed", 0, 10, 0.1, 1, NULL);
    sl("Turn Speed", "turnSpd", 0, 30, 0.1, 1, NULL);
    sl("Gravity", "gravity", 0, 1, 0.1, 1, NULL);
    sw("Velocity Movement", "pMov", 1, NULL);
    rule();
    fend(1);

    fold("Vision", 1, 1, 0, "technical.vision");
    sl("Vision Distance", "visDis", 10, 1000, 10, 1, NULL);
    sw("Xray Vision", "xrayVis", 1, NULL);
    rule();
    fend(1);

    // "None" -1 + the effects (p10027)
    const cJSON *effects = data_list("effects");
    const int ne = cJSON_GetArraySize(effects);
    EdOption *eff = edp_opts_alloc(ne + 1);
    eff[0] = (EdOption) {"None", -1, NULL};
    for (int i = 0; i < ne; i++) eff[i + 1] = (EdOption) {ed_json_str(cJSON_GetArrayItem(effects, i), "name", ""), i, NULL};

    fold("Melee", 1, 1, 0, "technical.meleeing");
    sw("Can Melee", "canHit", 1, NULL);
    sl("Melee Dmg", "meldmg", 1, 200, 1, 1, NULL);
    sl("Melee Rate", "hrate", 200, 10000, 50, 1, NULL);
    sl("Melee Range", "melrng", 10, 100, 1, 1, NULL);
    sl("Melee Delay (ms)", "melDel", 0, 5000, 1, 1, NULL);
    sl("Melee Shake", "melShake", 0, 1, 0.1, 1, NULL);
    sel("Effect On Hit", "hitEff", eff, ne + 1, 1, NULL);
    rule();
    fend(1);

    fold("Shooting", 1, 1, 0, "technical.attacking");
    sw("Can Shoot", "canShoot", 1, NULL);
    sl("Firerate", "frate", 200, 10000, 50, 1, NULL);
    sl("Spread", "aicur", 0, 1, 0.001, 1, NULL);
    sl("Shot Break (ms)", "shoBrk", 0, 5000, 1, 1, NULL);
    sw("Offset Shot", "shoOffH", 1, NULL);
    sl("Shot Arch Y", "shoArY", -90, 90, 1, 1, NULL);
    sl("Shot Delay (ms)", "shoDel", 0, 5000, 1, 1, NULL);
    sl("Shot Shake", "shoShake", 0, 1, 0.1, 1, NULL);
    sel("Effect On Shot", "shotEff", eff, ne + 1, 1, NULL);
    fold("Projectile", 2, 1, 0, "technical.attacking.projFol");
    txt("Projectile Asset", "projA", "ID", 1, NULL, 0);
    select_button(B, "Select Custom", SEL_PROJ_MODEL);
    sl("Projectile Dmg", "projDmg", 0, 1000, 1, 1, NULL);
    sl("Projectile Speed", "projSpd", 0, 20, 0.01, 1, NULL);
    sl("Projectile Size", "projSC", 0.1, 10, 0.1, 1, NULL);
    sl("Projectile Mesh Scale", "projMS", 0.01, 10, 0.01, 1, NULL);
    sl("Projectile Spin", "projS", 0, 2, 0.01, 1, NULL);
    sl("Projectile Gravity", "projG", -2, 2, 0.01, 1, NULL);
    sw("Projectile Glow", "projGL", 1, NULL);
    sw("Projectile Transparency", "projT", 1, NULL);
    sw("Explode", "explode", 1, NULL);
    fend(2);
    rule();
    fend(1);

    fold("Rewards", 1, 1, 0, "technical.reward");
    txt("Skin Rewards", "rewardType", "id,id,id", 1, NULL, 0);
    static const EdOption method[] = {{"Auto", 0, NULL}, {"Drop From AI", 1, NULL}, {"Drop on Node", 2, NULL}};
    static const EdOption req[] = {{"Player Active", 0, NULL}, {"Player Active & Damage", 1, NULL}, {"Player Killed", 2, NULL}};
    sel("Reward Method", "rewardMethod", method, 3, 1, NULL);
    sel("Reward Requirements", "rewardReq", req, 3, 1, NULL);
    sl("Reward Node Id", "rewardNode", 0, 1000, 1, 1, NULL);
    rule();
    fend(1);

    fold("Nametag", 1, 1, 0, "technical.nameTag");
    sl("Level", "aiLvl", 0, 999, 1, 1, NULL);
    sl("Health Scale", "aiTagScale", 0, 10, 0.1, 1, NULL);
    rule();
    fend(1);
}

static void build_bot(const EditorPrefab *pf) {
    // BOT_SPAWNER (UI:3538-3995; localOnly)
    EdCtl *c = ed_gui_number(B, "Class", "id", 0, 0);
    ed_ctl_bind_obj(c, "botClass");
    edp_finish(B, c, NULL);
    sw("Force Class", "botForceClass", 0, NULL);
    sl("Lives", "botLives", 0, 10, 1, 0, NULL);
    const int nodef = (pf->flags & EP_NODEFAULT) != 0;
    const int n = (nodef ? 1 : 0) + TEAM_LIMIT;
    EdOption *t = edp_opts_alloc(n);
    for (int i = 0; i < n; i++) t[i] = (EdOption) {nodef ? k_team_names[i] : i == 0 ? "Default" : k_team_names[i - 1], i, NULL};
    sel("Team", "botTeam", t, n, 0, NULL);
    sl("Health Mlt", "healthMlt", 0, 10, 0.01, 0, NULL);
    sl("Damage Mlt", "dmgMlt", 0, 10, 0.01, 0, NULL);
    sl("Spread Mlt", "spreadMlt", 0, 10, 0.01, 0, NULL);
    sl("Range Mlt", "rangeMlt", 0, 10, 0.01, 0, NULL);
    sl("Speed Mlt", "speedMlt", 0, 10, 0.01, 0, NULL);
    sw("Recoil", "botRecoil", 0, NULL);
    static const char *const behave[][2] = {
        {"Choose Random Spawn", "botRandomSpawn"}, {"Roam When No Target", "botRoam"}, {"God Mode", "botGodMode"},
        {"Voicechat on Kill", "botVoiceChat"}, {"Message on Join", "botJoinMessage"}, {"Message on Leave", "botLeaveMessage"},
        {"Nod Yes", "botNodYes"}, {"Nod No", "botNodNo"}, {"Remove On Death", "botRemoveOnDeath"},
        {"Has Spawn Delay", "botUseSpawnDelay"}, {"Can Respawn", "botCanRespawn"}, {"Can Slide hop", "botSlideHop"},
        {"Mimic Target", "botMimicTarget"}, {"Watch Target", "botWatchTarget"}, {"Follow Target", "botFollowTarget"},
        {"Attack Target", "botAttackTarget"}, {"Force Crouch", "botForceCrouch"}, {"Can Target Team", "botTargetTeam"},
        {"Can Target Bots", "botTargetBots"},
    };
    fold("Behavior", 1, 1, 0, "technical.botBehavior");
    for (size_t i = 0; i < sizeof(behave) / sizeof(behave[0]); i++) sw(behave[i][0], behave[i][1], 1, NULL);
    rule();
    fend(1);
    // botWrist is listed twice in v7's object literal: one control
    static const char *const cosm[][2] = {
        {"Primary Skin", "botPrimSkin"}, {"Melee Skin", "botMelee"}, {"Hat", "botHat"}, {"Head", "botHead"}, {"Face", "botFace"},
        {"Body", "botBody"}, {"Back", "botBack"}, {"Waist", "botWaist"}, {"Shoes", "botShoes"}, {"Dye", "botDye"},
        {"Wrist", "botWrist"}, {"Pet", "botPet"},
    };
    fold("Cosmetics", 1, 1, 0, "technical.botCosmetics");
    for (size_t i = 0; i < sizeof(cosm) / sizeof(cosm[0]); i++) txt(cosm[i][0], cosm[i][1], "id", 1, NULL, 0);
    rule();
    fend(1);
}

static void build_technical(const EditorPrefab *pf, const int group) {
#define HAS(f) ((pf->flags & (f)) != 0)
#define IS(n) (!strcmp(pf->name, n))
    if (HAS(EP_NOTTECHNICAL)) return;
    const EdBuf saved = block_begin();
    if (group && !IS("DEPOSIT_BOX")) { // a DEPOSIT_BOX's withdraw / deposit replace the whole set
        static EdOption keys[sizeof(k_oi_setters) / sizeof(k_oi_setters[0])];
        for (size_t i = 0; i < sizeof(k_oi_setters) / sizeof(k_oi_setters[0]); i++) keys[i] = (EdOption) {k_oi_setters[i], 0, k_oi_setters[i]};
        EdCtl *k = json_sel("Key", g_ed.advanced, "editCustomKey", keys, (int) (sizeof(keys) / sizeof(keys[0])), 0, NULL);
        k->value_is_string = 1;
        ed_gui_button(B, "Execute", act_group_custom);
    }
    if (IS("DEPOSIT_BOX")) {
        sl("Withdraw Amount", "withdraw", 0, 100000, 1, 0, NULL);
        sl("Deposit Amount", "deposit", 0, 100000, 1, 0, NULL);
    }
    if (HAS(EP_CUSTOMSCORE)) {
        const int machine = HAS(EP_MYSTERYBOX) || HAS(EP_PERKMACHINE) || HAS(EP_PACKAPUNCH);
        sl(machine ? "Cost" : HAS(EP_INTERACT) ? "Amount" : "Score", "score", HAS(EP_INTERACT) || HAS(EP_AI) ? 0 : 1, machine ? 50000 : 10000, 1, 0, NULL);
    }
    if (HAS(EP_PERKMACHINE)) {
        int n;
        const EdOption *p = names_options(data_list("perks"), 0, &n);
        sel("Perk Type", "perk", p, n, 0, NULL);
    }
    if (IS("CHECK_POINT") || IS("TRIGGER") || HAS(EP_AI)) sw("Single Use", "single", 0, NULL);
    if (IS("TRIGGER")) sw("Once Per Player", "singleP", 0, NULL);
    if (HAS(EP_INTERACT)) {
        static const EdOption m[] = {{"Cost Score", 0, NULL}, {"Require Score", 1, NULL}, {"Require Kills", 2, NULL},
                                     {"Requires Account", 3, NULL}, {"Requires Verified", 4, NULL}, {"Requires Premium", 5, NULL},
                                     {"Requires Partner", 6, NULL}, {"Requires Level", 7, NULL}};
        sel("Method", "method", m, 8, 0, NULL);
        if (HAS(EP_CANUNDO)) {
            sw("Closeable", "closeable", 0, NULL);
            sw("Starts Hidden", "startClosed", 0, NULL);
        }
        sw("No Popup", "hidden", 0, NULL);
    }
    if (HAS(EP_INTERACTMSG) || (IS("TRIGGER") && (event_is(O, "onInteractKey") || event_is(O, "onHoldMouse"))))
        txt("Custom Message", "interactM", "", 0, NULL, 0);
    if (HAS(EP_HASHEALTH)) {
        sw("Destructible", "canDest", 0, NULL);
        if (!IS("GATE")) {
            sw("Start Destroyed", "startDest", 0, NULL);
            sl("Spawn Chance", "spawnChance", 0, 100, 0.01, 0, NULL);
        }
        if (IS("TRIGGER")) sw("Destroy On Use", "selfDestruct", 0, NULL);
        sl("Health", "health", 0, 50000, 10, 0, NULL);
        rule();
        fold("Respawn", 1, 1, 0, "technical.respawn");
        sl("Timer (s)", "respawnT", 0, 3600, 0.1, 1, NULL);
        sw("Random", "respawnR", 1, NULL);
        rule();
        fend(1);
    }
    const int hitbox = HAS(EP_ALTERHITBOX);
    if (hitbox) {
        fold("Hitbox", 1, 1, 0, "technical.hitbox");
        sl("Width Mlt", "hbWidth", 0.1, 10, 0.1, 1, NULL);
        sl("Height Mlt", "hbHeight", 0.1, 20, 0.1, 1, NULL);
        if (HAS(EP_AI)) sl("Crit Damage Mlt", "critDmg", 0, 5, 0.01, 1, NULL);
        rule();
        fend(1);
    }
    if (HAS(EP_AI)) build_ai();
    if (HAS(EP_BOT)) build_bot(pf);
    if (IS("WEAPON_PICKUP")) {
        // the weapons with a model (p10025.map(src ? {name, value: index} : null).filter)
        const cJSON *weapons = data_list("weapons");
        EdOption *w = edp_opts_alloc(cJSON_GetArraySize(weapons));
        int n = 0, i = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, weapons) {
            if (ed_json_str(it, "src", NULL)) w[n++] = (EdOption) {ed_json_str(it, "name", ""), i, NULL};
            i++;
        }
        fold("Weapon", 1, 0, 0, "technical.weapon");
        sel("Type", "weaponId", w, n, 1, regen_refresh);
        sw("Replaceable", "weaponReplace", 1, NULL);
        sw("Random", "randWep", 1, NULL);
        sl("Grouping Id", "randWepGroup", 0, 10000, 1, 1, NULL);
        sw("Show Icon", "showPickupIcon", 1, NULL);
        sw("White Texture", "noTexture", 1, NULL);
        fend(1);
    }
    if (IS("TRIGGER") || HAS(EP_AI)) {
#ifdef ED_HAVE_TRIGGERS_PANEL
        ed_triggers_panel(B, O);
#else
        ed_triggers_panel_fallback(B, O);
#endif
    }
    const int iface = !IS("WEAPON_PICKUP") && HAS(EP_CANINTERFACE);
    if (iface || HAS(EP_CANTARGETINTERFACE)) {
        const double mx = ed_json_num(editor_data_json(), "interfaceIDMax", 2000);
        fold("Interface", 1, 1, 0, "technical.interface");
        if (iface) sl("ID", "interfaceId", 0, mx, 1, 1, on_interface);
        if (HAS(EP_CANTARGETINTERFACE)) sl("Target", "interfaceIdT", 0, mx, 1, 1, on_interface);
        fend(1);
    }
    if (IS("TELEPORTER")) {
        static const EdOption nodes[] = {{"Sending", 0, NULL}, {"Receiving", 1, NULL}, {"Both", 2, NULL}};
        sel("Node", "node", nodes, 3, 0, NULL);
        sl("Channel", "channel", 0, 100, 1, 0, NULL);
        sw("Stop Momentum", "momentum", 0, NULL);
        sw("Use Active Checkpoint", "telCheckPoint", 0, NULL);
    }
    if (HAS(EP_CANSCRIPT)) txt("Script #", "scriptId", "", 0, NULL, 1);
    EdBuf inner = block_end(saved);
    if (inner.len) {
        fold("Technical & Logic", 0, !group, 0, "technical");
        block_put(&inner);
        fend(0);
    } else edb_free(&inner);
#undef HAS
#undef IS
}

// ---------------------------------------------------------------------------
// type folders (UI:4626-5826)

static void build_faces(void) {
    static const char *const faces[6] = {"Right", "Left", "Top", "Bottom", "Back", "Front"};
    fold("Render Faces", 0, 1, 0, "renderFaces");
    cJSON *fv = ed_prop_get(O, "faces");
    for (int i = 0; i < 6; i++) {
        EdCtl *c = ed_gui_switch(B, faces[i], 0);
        cJSON *tmp = cJSON_CreateObject();
        cJSON_AddItemToObject(tmp, "v", cJSON_Duplicate(cJSON_GetArrayItem(fv, i), 1));
        ed_ctl_bind_json(c, tmp, "v");
        edp_finish(B, c, on_faces);
        c->arg = i; // after the markup: arg is the line's sub level until then
        c->bind = EB_NONE;
        c->json = NULL;
        cJSON_Delete(tmp);
    }
    cJSON_Delete(fv);
    fend(0);
}

static void build_environment(void) {
    // ENVIRONMENT_ZONE: the Environment panel's fields, stored on the object (UI:4646-5069)
    fold("Environment", 0, 0, 0, "environments");
    static const EdOption sky[] = {{"Solid", 0, NULL}, {"Gradient", 1, NULL}};
    const int dome = prop_truthy(O, "skyDome");
    sel("Sky", "skyDome", sky, 2, 0, on_env_sky);
    edp_div_begin(B, "ensky_0", !dome);
    col("Color", "sky", 1, NULL);
    edb_add(&B->html, "</div>\n");
    edp_div_begin(B, "ensky_1", dome);
    fold("Color", 1, 0, 1, "environments.sky1.color");
    col("Top", "skyDomeCol0", 2, NULL);
    col("Middle", "skyDomeCol1", 2, NULL);
    col("Bottom", "skyDomeCol2", 2, NULL);
    fend(1);
    sw("Cloud Texture", "skyDomeTex", 1, on_env_cloud);
    edb_add(&B->html, "</div>\n");
    edp_div_begin(B, "encloud", prop_truthy(O, "skyDomeTex"));
    col("Emissive", "skyDomeEmis", 2, NULL);
    txt("Emissive Asset ID", "skyDomeEmisTex", "Emissive Asset ID", 2, NULL, 0);
    select_button(B, "Select Emissive", SEL_EMISSIVE_CLOUD_EN);
    sl("Speed", "skyDomeMovT", -20, 20, 0.1, 2, NULL);
    sel("Axis", "skyDomeMovD", g_axes, 2, 2, NULL);
    txt("Asset ID", "skyDomeTexA", "Texture Asset ID", 2, NULL, 0);
    select_button(B, "Select Custom", SEL_CLOUD_EN);
    edb_add(&B->html, "</div>\n");
    rule();

    fold("Ambient", 0, 1, 0, "environments.ambient");
    // soundScapes: an object (name -> id) in key order
    const cJSON *scapes = data_list("soundScapes");
    EdOption *s = edp_opts_alloc(cJSON_GetArraySize(scapes));
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, scapes) s[n++] = (EdOption) {it->string, cJSON_GetNumberValue(it), NULL};
    sel("Sound", "ambInd", s, n, 0, NULL);
    num("Custom Sound ID", "ambIndC", "Sound Asset ID", 0, NULL);
    select_button(B, "Select Sound", SEL_AMB_EN);
    sl("Intensity", "ambientI", 0.1, 3, 0.1, 0, NULL);
    col("Color", "ambientC", 0, NULL);
    fend(0);

    fold("Light", 0, 1, 0, "environments.light");
    col("Color", "lightC", 0, NULL);
    sl("Distance", "lightD", 10, 5000, 1, 0, NULL);
    sl("Intensity", "lightI", 0.1, 3, 0.1, 0, NULL);
    fold("Angle", 1, 0, 1, "environments.light.angle");
    sl("X", "sunAngX", 0, 360, 1, 0, NULL);
    sl("Y", "sunAngY", 0, 360, 1, 0, NULL);
    fend(1);
    fend(0);

    fold("Shadows", 0, 1, 0, "environments.shadows");
    sl("Resolution", "shadowR", 128, 4096, 1, 0, NULL);
    sl("Distance", "shadowD", 128, 4096, 1, 0, NULL);
    fend(0);

    fold("Weather", 0, 1, 0, "environments.weather");
    fold("Fog", 1, 0, 1, "environments.weather.fog");
    col("Color", "fogC", 0, NULL);
    sl("Distance", "fogD", 0, 15000, 1, 0, NULL);
    fend(1);
    fend(0);

    fold("Render", 0, 1, 0, "environments.render");
    EdOption *t = edp_opts_alloc(5);
    for (int i = 0; i < 5; i++) t[i] = (EdOption) {k_tone_types[i], i, NULL};
    sel("ToneMapping", "toneMapping", t, 5, 0, NULL);
    sl("ToneMapping Exposure", "toneMappingExposure", 0, 10, 0.1, 0, NULL);
    const EdOption *e = names_options(data_list("encodingTypes"), 0, &n);
    sel("Output Encoding", "outputEncoding", e, n, 0, NULL);
    sw("Physically Correct Lights", "correctLights", 0, NULL);
    fend(0);
    fend(0);
}

static void build_type_folders(const EditorPrefab *pf) {
#define HAS(f) ((pf->flags & (f)) != 0)
#define IS(n) (!strcmp(pf->name, n))
    if (HAS(EP_EDITFACES)) build_faces();
    if (IS("ENVIRONMENT_ZONE")) build_environment();
    if (HAS(EP_LIGHT) || IS("DIRECTIONAL_LIGHT") || IS("RECTAREA_LIGHT") || IS("SPOT_LIGHT")) {
        fold("Lighting", 0, 0, 0, "lighting");
        sl("Intensity", "intensity", 0, 15, 0.1, 0, NULL);
        if (HAS(EP_LIGHT) || IS("SPOT_LIGHT")) {
            sl("Distance", "distance", 0, 1000, 0.1, 0, NULL);
            sl("Decay", "decay", 0, 2, 0.1, 0, NULL);
        }
        if (IS("DIRECTIONAL_LIGHT") || IS("SPOT_LIGHT")) v3("Target", "lightTarget", NULL);
        if (IS("SPOT_LIGHT")) {
            sl("Angle", "angle", 1, 90, 1, 0, NULL);
            sl("Penumbra", "penumbra", 0, 1, 0.1, 0, NULL);
        }
        rule();
        fend(0);
    }
    int n;
    if (IS("BILLBOARD")) {
        fold("Billboard", 0, 0, 0, "billboard");
        EdOption *b = edp_opts_alloc(7);
        for (int i = 0; i < 7; i++) b[i] = (EdOption) {k_billboards[i], i, NULL};
        sel("Poster", "poster", b, 7, 0, NULL);
        rule();
        fend(0);
    }
    if (IS("PARTICLES")) {
        fold("Particles", 0, 0, 0, "particles");
        const EdOption *t = names_options(data_list("particleIDS"), 1, &n);
        sel("Texture", "part", t, n, 0, NULL);
        const EdOption *bl = names_options(data_list("particleBlending"), 1, &n);
        sel("Blending", "partB", bl, n, 0, NULL);
        sl("Count", "partC", 0, 300, 1, 0, NULL);
        sl("Size", "partS", 0, 30, 0.1, 0, NULL);
        sl("Scale", "partSC", -10, 10, 0.1, 0, NULL);
        fold("Movement", 1, 0, 0, "particles.movement");
        sl("Speed", "partSP", -10, 10, 0.1, 1, NULL);
        sl("Deceleration", "partD", -1, 1, 0.1, 1, NULL);
        sl("Gravity", "partG", -10, 10, 0.1, 1, NULL);
        rule();
        fend(1);
        rule();
        fend(0);
    }
    if (HAS(EP_HASTEXT)) {
        fold("Text", 0, 0, 0, "text");
        if (!IS("LEADERBOARD")) {
            static const EdOption align[] = {{"Left", 0, NULL}, {"Center", 1, NULL}, {"Right", 2, NULL}};
            txt("Content", "text", "Content", 0, regen_refresh, 0);
            sel("Align", "tAlign", align, 3, 0, NULL);
        }
        fold("Font", 1, 0, 0, "text.font");
        sl("Size", "fsize", SIGN_FONT_MIN, SIGN_FONT_MAX, 1, 1, NULL);
        sw("Dont Scale Font", "noSCF", 1, NULL);
        col("Color", "fcolor", 1, NULL);
        col("Outline Color", "ocolor", 1, NULL);
        sl("Outline Thickness", "othick", 1, 50, 1, 1, NULL);
        fend(1);
        fold("Background", 1, 0, 0, "text.background");
        col("Color", "bcolor", 1, NULL);
        sw("Visible", "backVisible", 1, NULL);
        fend(1);
        rule();
        fend(0);
    }
    if (O->prefab == ED_PREFAB_SPAWN_POINT) {
        fold("Spawn Point", 0, 0, 0, "spawnpoint");
        sw("Starting Area", "cmpSpwn", 0, NULL);
        sw("Team Only", "teamOnly", 0, NULL);
        sl("Spawn Spread", "spread", 0, 50, 0.1, 0, NULL);
        rule();
        fend(0);
    }
    if (IS("LIQUID")) {
        fold("Wave", 0, 0, 0, "liquid");
        fold("Liquid", 1, 0, 0, "liquid.wave");
        sl("Speed", "wSpeed", 0, 25, 0.1, 1, NULL);
        sl("Height", "wHeight", 0, 5, 0.1, 1, NULL);
        rule();
        fend(1);
        fold("Depth", 1, 0, 0, "liquid.depth");
        sl("X", "tilesX", 2, 100, 1, 1, NULL);
        sl("Z", "tilesZ", 2, 100, 1, 1, NULL);
        fend(1);
        rule();
        fend(0);
    }
    if (HAS(EP_AI)) {
        // the AI's sound folder replaces the emitter one (same key)
        static const char *const s[][3] = {{"Death Sound ID", "deathSound", "3"}, {"Idle Sound ID", "idleSound", "4"},
                                           {"Shoot Sound ID", "shootSound", "5"}, {"Shoot-Hit Sound ID", "shootHitSound", "8"},
                                           {"Melee Sound ID", "melSound", "9"}, {"Melee-Hit Sound ID", "melHitSound", "10"},
                                           {"Agro Sound ID", "agroSound", "6"}};
        fold("Sound", 0, 1, 0, "sound");
        for (size_t i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
            num(s[i][0], s[i][1], "Sound Asset ID", 0, NULL);
            select_button(B, "Select Sound", atoi(s[i][2]));
        }
        fend(0);
    } else if (IS("SOUND_EMITTER") || HAS(EP_HASSOUND)) {
        fold("Sound", 0, 1, 0, "sound");
        const EdOption *snd = names_options(data_list("emitterSounds"), 0, &n);
        sel("Type", "snd", snd, n, 0, NULL);
        num("Asset ID", "assetSound", "Sound Asset ID", 0, NULL);
        select_button(B, "Select Custom", SEL_AUDIO);
        sl("Volume", "vol", EMITTER_VOL_MIN, EMITTER_VOL_MAX, 0.01, 0, NULL);
        sl("Rate", "rate", EMITTER_RATE_MIN, EMITTER_RATE_MAX, 0.01, 0, NULL);
        if (IS("SOUND_EMITTER")) {
            static const EdOption dm[] = {{"Inverse", 0, NULL}, {"Linear", 1, NULL}, {"Exponential", 2, NULL}};
            sl("Reference Distance", "dist", EMITTER_DIST_MIN, EMITTER_DIST_MAX, 1, 0, NULL);
            sel("Distance Model", "distM", dm, 3, 0, NULL);
            sl("Roll-off", "roll", EMITTER_ROLL_MIN, EMITTER_ROLL_MAX, 0.01, 0, NULL);
        } else {
            static const EdOption from[] = {{"Trigger (3D)", 0, NULL}, {"Player (3D)", 1, NULL}, {"Global (2D)", 2, NULL}};
            sel("Emit From", "emitfrom", from, 3, 0, NULL);
        }
        fend(0);
    }
    if (IS("CUSTOM_ASSET") || HAS(EP_AI) || IS("TRIGGER")) {
        fold("Animations", 0, 1, 0, "animations");
        sl("Animation Speed", "animS", 0.01, 10, 0.01, 0, NULL);
        txt("Start Animation", "startAnim", "", 0, NULL, 0);
        fend(0);
    }
#undef HAS
#undef IS
}

void ed_panel_right_object(void) {
    if (ed_project_background()) { // working on another project: rebuild once it is shown again
        g_ed.ui_stale = 1;
        return;
    }
    if (ed_panel_locked(1) && strcmp(ed_panel_right_active(), "object")) return;
    EditorObject *o = g_ed.proj->selected;
    if (!o) {
        ed_panel_right_blank();
        return;
    }
    const EditorPrefab *pf = &editor_prefabs[o->prefab];
    const int group = is_group_owner(o);
    EdBuild b;
    edp_right_begin(&b, "object");
    B = &b;
    O = o;
    ed_gui_panel_buttons(&b, 1, group ? "Group" : pf->label, 0);
    build_header(pf);
    rule();
    build_transform(pf, group);
    build_style(pf, group);
    build_technical(pf, group);
    build_type_folders(pf);
    ed_gui_end(&b, g_ed.el_right_list);
    B = NULL;
    O = NULL;
}

#ifndef ED_HAVE_TRIGGERS_PANEL
// ---------------------------------------------------------------------------
// WP4 owns this: editor_triggers.c's ed_triggers_panel() replaces it (doc/v7/editor.md WP4).
// The pre-WP4 controls: event, its value, action 0, its value and the action chance.

static void ed_triggers_panel_fallback(EdBuild *b, EditorObject *o) {
    const EditorPrefab *pf = &editor_prefabs[o->prefab];
    const int ai = (pf->flags & EP_AI) != 0;
    const cJSON *trig = data_list("triggers");
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(trig, "events");
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(trig, "actions");
    ed_gui_folder_begin(b, "Trigger Event", 1, 1, 0, "object.technical.events");
    if (ai) edp_obj_switch(b, "Can Trigger", "canTrigger", 0, NULL);
    EdOption *ev = edp_opts_alloc(cJSON_GetArraySize(events));
    int n = 0, i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, events) {
        const char *name = cJSON_IsString(it) ? it->valuestring : ed_json_str(it, "name", "");
        const int ai_event = strstr(name, "onAI") != NULL;
        if ((ai && ai_event) || (!strcmp(pf->name, "TRIGGER") && !ai_event)) ev[n++] = (EdOption) {name, i, NULL};
        i++;
    }
    edp_obj_select(b, "Event", "event", ev, n, 0, refresh_panel);
    edp_obj_slider(b, "Event Value", "constantEvent", 0, 10000, 1, 0, NULL);
    ed_gui_folder_end(b, 1);
    ed_gui_folder_begin(b, "Trigger Actions", 1, 1, 0, "object.technical.actions");
    edp_obj_slider(b, "Action Chance (%)", "chance", 0, 100, 1, 0, NULL);
    EdOption *ac = edp_opts_alloc(cJSON_GetArraySize(actions));
    n = 0;
    i = 0;
    cJSON_ArrayForEach(it, actions) ac[n++] = (EdOption) {ed_json_str(it, "name", ""), i++, NULL};
    edp_obj_select(b, "Action", "action", ac, n, 1, refresh_panel);
    const cJSON *cur = cJSON_GetArrayItem(actions, (int) prop_num(o, "action"));
    if (cur && !strcmp(ed_json_str(cur, "type", ""), "Text")) edp_obj_text(b, "Value", "constantTxt", "Text", 0, NULL);
    else edp_obj_slider(b, "Action Value", "constant", -10000, 10000, 1, 0, NULL);
    ed_gui_rule(b);
    ed_gui_folder_end(b, 1);
}
#endif

// ---------------------------------------------------------------------------
// T-key list pickers (E:6813-6957): genericPopup(html, "listPop") with KE.select* on each item

static void act_model_list(EdCtl *c) {
    (void) c;
    editor_texture_list();
}

static void list_item_img(EdBuf *b, const char *index, const char *onclick, const char *src, const char *name, const char *variant, const int model) {
    edb_add(b, "<div class='textureListItem' data-index='");
    edb_add_escaped(b, index);
    edb_addf(b, "' onmouseenter='playTick(0.1)' onclick='playSelect();%s;clearPops();'>\n\t\t\t\t\t<div class='textureListImgH'>\n"
                "\t\t\t\t\t\t<img class='textureListImg' loading='lazy' draggable='false' src='", onclick);
    edb_add_escaped(b, src);
    edb_add(b, model ? "' style=\"image-rendering: unset!important;\"/>\n\t\t\t\t\t</div>\n" : "'/>\n\t\t\t\t\t</div>\n");
    if (name) {
        edb_add(b, "\t\t\t\t\t<div class='textureListName'>");
        edb_add(b, name); // as v7: store names are markup ("Soon&trade;")
        edb_add(b, "</div>\n");
    }
    if (variant) {
        edb_add(b, "\t\t\t\t\t<div class='textureListVariant'>");
        edb_add_escaped(b, variant);
        edb_add(b, "</div>\n");
    }
    edb_add(b, "\t\t\t\t</div>");
}

// UTILS.getPreview (utils.js:698) with store.types: the skin's thumbnail under textures/
static void skin_preview(const cJSON *t, char *out, const size_t n) {
    static const char *const types[17] = {"weapons/weapon_", "hats/hat_", "body/body_", "melee/melee_", "sprays/", "dyes/",
                                          "waist/waist_", "faces/face_", "shoes/shoe_", "pets/pet_", "collectibles/collect_",
                                          "wrist/wrist_", "charms/charm_", "tickets/ticket_", "back/back_", "head/head_", "playercards/card_"};
    const int type = (int) ed_json_num(t, "type", 0), id = (int) ed_json_num(t, "id", 0);
    const int asset = (int) ed_json_num(t, "assetType", 0) ? (int) ed_json_num(t, "assetType", 0) : type;
    const char *ext = ed_json_str(t, "texExt", "png");
    const cJSON *tex = cJSON_GetObjectItemCaseSensitive(t, "tex"), *pat = cJSON_GetObjectItemCaseSensitive(t, "pat");
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(t, "mid"), *midt = cJSON_GetObjectItemCaseSensitive(t, "midT");
    char texs[32] = "";
    if (tex && !cJSON_IsNull(tex)) snprintf(texs, sizeof(texs), "%s", cJSON_IsString(tex) ? tex->valuestring : "");
    if (cJSON_IsNumber(tex)) snprintf(texs, sizeof(texs), "%d", (int) tex->valuedouble);
    const int tex_truthy = texs[0] && strcmp(texs, "0");
    if (type == IT_SPRAY || type == IT_PLAYER_CARD) snprintf(out, n, "textures/%s%d.%s", types[type], id, ext);
    else if (type && type != 3) snprintf(out, n, "textures/previews/cosmetics/%d_%d%s%s.%s", asset, id, tex_truthy ? "_" : "", tex_truthy ? texs : "", ext);
    else if (type == 3) {
        if (pat && !cJSON_IsNull(pat)) snprintf(out, n, "textures/previews/%s%d_c%d.%s", types[asset > 0 && asset < 17 ? asset : 3], id, (int) cJSON_GetNumberValue(pat), ext);
        else snprintf(out, n, "textures/previews/%s%d%s%s.%s", types[asset > 0 && asset < 17 ? asset : 3], id, tex && !cJSON_IsNull(tex) ? "_" : "", texs, ext);
    } else {
        char tail[48];
        if (mid && !cJSON_IsNull(mid)) {
            if (midt && !cJSON_IsNull(midt)) snprintf(tail, sizeof(tail), "m%d_%d", (int) cJSON_GetNumberValue(mid), (int) cJSON_GetNumberValue(midt));
            else snprintf(tail, sizeof(tail), "m%d", (int) cJSON_GetNumberValue(mid));
        } else if (pat && !cJSON_IsNull(pat)) snprintf(tail, sizeof(tail), "c%d", (int) cJSON_GetNumberValue(pat));
        else if (tex_truthy) snprintf(tail, sizeof(tail), "%s", texs);
        else snprintf(tail, sizeof(tail), "%d", id);
        snprintf(out, n, "textures/previews/%s%d_%s.%s", types[asset > 0 && asset < 17 ? asset : 0], (int) ed_json_num(t, "weapon", 0), tail, ext);
    }
}

// viewModelList: the selected gen prefab's model variants (img/editor-models/<src>[_<id>].png)
static void view_model_list(const EditorObject *o) {
    char id[16];
    snprintf(id, sizeof(id), "%d", o->prefab);
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(data_list("prefabVariants"), id);
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(pv, "names");
    if (cJSON_GetArraySize(names) <= 0) {
        editor_message("Select a object with variants");
        return;
    }
    EdBuf b;
    edb_init(&b);
    edb_add(&b, "<div class=\"popHeader\">Select a Variant</div>");
    for (int i = 0; i < cJSON_GetArraySize(names); i++) {
        char sfx[16] = "", src[160], onclick[64];
        if (i) snprintf(sfx, sizeof(sfx), "_%d", i);
        snprintf(src, sizeof(src), "img/editor-models/%s%s.png", ed_json_str(pv, "src", ""), sfx);
        snprintf(onclick, sizeof(onclick), "KE.selectModelVariant(%d)", i);
        const cJSON *nm = cJSON_GetArrayItem(names, i);
        list_item_img(&b, sfx, onclick, src, NULL, cJSON_IsString(nm) ? nm->valuestring : "", 1);
    }
    ed_generic_popup(b.buf ? b.buf : "", "listPop");
    edb_free(&b);
}

// viewTextureList: textures x variants, or the sprays (spray prefabs) / skins (SHOWCASE)
static void view_texture_list(const EditorObject *o) {
    const EditorPrefab *pf = o ? &editor_prefabs[o->prefab] : NULL;
    const int spray = pf && (pf->flags & EP_SPRAYTEXTURE);
    const int skin = pf && !strcmp(pf->name, "SHOWCASE");
    if (!o || ((pf->flags & EP_NOTSTYLEABLE) && !spray) || (!(pf->flags & EP_TEXTURABLE) && !skin)) {
        editor_message("Select a texturable object first");
        return;
    }
    EdBuf b;
    edb_init(&b);
    edb_addf(&b, "<div class=\"popHeader\">Select a %s<div id=\"skinSearchH\"><input id=\"skinSearch\" type=\"text\" placeholder=\"Search Item\" "
                 "oninput=\"KE.searchItems(this,%s)\"></div></div>", spray ? "Spray" : skin ? "Skin" : "Texture", !spray && !skin ? "true" : "false");
    if (spray || skin) {
        const cJSON *s = skins();
        for (int i = 0; i < cJSON_GetArraySize(s); i++) {
            const cJSON *t = cJSON_GetArrayItem(s, i);
            const int type = (int) ed_json_num(t, "type", 0);
            if ((skin && !skin_listed(type)) || (spray && type != IT_SPRAY)) continue;
            char index[16], onclick[64], src[256];
            snprintf(index, sizeof(index), "%d", (int) ed_json_num(t, "index", i));
            // intentional difference from v7.1.4: selectSpray gets the spray's index (the value the
            // Spray select and "spt" use); v7 passes the store position, which names another item
            snprintf(onclick, sizeof(onclick), "KE.%s(%d)", spray ? "selectSpray" : "selectSkin", spray ? (int) ed_json_num(t, "index", i) : i);
            char path[160];
            skin_preview(t, path, sizeof(path));
            html_dom_asset_url(path, src, sizeof(src)); // UTILS.getPreview: an assetsUrl
            list_item_img(&b, index, onclick, src, ed_json_str(t, "name", ""), NULL, 0);
        }
    } else {
        edb_add(&b, "<div class='textureListItem' onmouseenter='playTick(0.1)' onclick='playSelect();KE.selectTex(5, 0);clearPops();'>\n"
                    "\t\t<div class='textureListImgH'>\n\t\t\t<div class='textureNone'>X</div>\n\t\t</div>\n\t\t<div class='textureListName'>None</div>\n\t</div>");
        int i = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, data_list("textures")) {
            const char *name = ed_json_str(t, "name", "");
            if (!strcmp(name, "DEFAULT")) {
                i++;
                continue;
            }
            const cJSON *variants = cJSON_GetObjectItemCaseSensitive(t, "variants");
            const int nv = cJSON_GetArraySize(variants) > 0 ? cJSON_GetArraySize(variants) : 1;
            char label[48];
            edp_const_name(ed_json_str(t, "alias", name), label, sizeof(label));
            for (int v = 0; v < nv; v++) {
                char sfx[16] = "", index[24], onclick[64], src[256];
                if (v) snprintf(sfx, sizeof(sfx), "_%d", v);
                snprintf(index, sizeof(index), "%d%s", i, sfx);
                snprintf(onclick, sizeof(onclick), "KE.selectTex(%d,%d)", i, v);
                char path[128];
                snprintf(path, sizeof(path), "textures/%s%s.png", ed_json_str(t, "src", ""), sfx);
                html_dom_asset_url(path, src, sizeof(src)); // UTILS.assetsUrl("/textures/<src>[_<id>].png")
                const cJSON *vn = cJSON_GetArrayItem(variants, v);
                list_item_img(&b, index, onclick, src, label, v && cJSON_IsString(vn) ? vn->valuestring : NULL, 0);
            }
            i++;
        }
    }
    ed_generic_popup(b.buf ? b.buf : "", "listPop");
    edb_free(&b);
}

// textureKey (T) and the selectClick icons: viewModelList for gen prefabs with variants, else viewTextureList
void editor_texture_list(void) {
    const EditorObject *o = g_ed.proj ? g_ed.proj->selected : NULL;
    if (o && editor_prefab_has(o->prefab, EP_GEN) && editor_prefabs[o->prefab].variants) view_model_list(o);
    else view_texture_list(o);
}

static void set_num(EditorObject *o, const char *prop, const double v) {
    cJSON *n = cJSON_CreateNumber(v);
    ed_prop_set(o, prop, n);
    cJSON_Delete(n);
}

// KE.selectTex(t, v) / selectModelVariant(v) / selectSpray(i) / selectSkin(i)
static int select_item(const char *what, const HtmlArg *a, const int n) {
    EditorObject *o = g_ed.proj ? g_ed.proj->selected : NULL;
    if (!o || n < 1) return 1;
    const EditorPrefab *pf = &editor_prefabs[o->prefab];
    const int arg0 = (int) a[0].num;
    if (!strcmp(what, "tex")) {
        if ((pf->flags & EP_NOTSTYLEABLE) || !(pf->flags & EP_TEXTURABLE) || arg0 < 0 || arg0 >= EDITOR_TEXTURE_COUNT) return 1;
        // assetTexture = 0; texVar = v; texture = prefabs.textures[t].name
        cJSON *name = cJSON_CreateString(editor_textures[arg0]);
        ed_prop_set(o, "texture", name);
        cJSON_Delete(name);
        set_num(o, "texVar", n >= 2 ? (int) a[1].num : 0);
    } else if (!strcmp(what, "model")) {
        if (!pf->variants) return 1;
        set_num(o, "texVar", arg0);
    } else if (!strcmp(what, "spray")) {
        if (!(pf->flags & EP_TEXTURABLE)) return 1;
        set_num(o, "sprayTexture", arg0); // the spray's index (view_texture_list)
    } else if (!strcmp(what, "skin")) {
        if (strcmp(pf->name, "SHOWCASE")) return 1;
        set_num(o, "itemID", arg0);
    }
    ed_obj_rebuild(o);
    g_ed.dirty_panel = 1;
    ed_mark_changed();
    return 1;
}

// KE.searchItems(input, textures): filter the list items by name / alias / variant (textures) or
// by keyword / name / weapon / creators (skins)
static int h_search(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) a; (void) n;
    char q[128] = "";
    html_dom_get_value(ev->target, q, sizeof(q));
    for (char *p = q; *p; p++) *p = (char) tolower((unsigned char) *p);
    const int textures = n >= 2 && a[1].kind == HTML_ARG_BOOL && a[1].num != 0;
    enum { MAX_ITEMS = 8192 }; // the skin list (store.skins): ~6000
    HtmlElem *items = malloc(MAX_ITEMS * sizeof(*items));
    if (!items) return 1;
    int count = html_dom_query_all(html_dom_by_id("genericPop"), ".textureListItem", items, MAX_ITEMS);
    if (count > MAX_ITEMS) count = MAX_ITEMS;
    for (int i = 0; i < count; i++) {
        char index[32] = "";
        int show = 1;
        if (q[0] && html_dom_get_attr(items[i], "data-index", index, sizeof(index)) >= 0) {
            show = 0;
            char hay[512] = "";
            if (textures) {
                const cJSON *t = cJSON_GetArrayItem(data_list("textures"), atoi(index));
                if (!t) show = 1;
                else {
                    snprintf(hay, sizeof(hay), "%s|%s", ed_json_str(t, "name", ""), ed_json_str(t, "alias", ""));
                    const cJSON *v;
                    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(t, "variants")) {
                        const size_t l = strlen(hay);
                        if (cJSON_IsString(v) && l + 1 < sizeof(hay)) snprintf(hay + l, sizeof(hay) - l, "|%s", v->valuestring);
                    }
                }
            } else {
                html_dom_get_text(items[i], hay, sizeof(hay));
            }
            for (char *p = hay; *p; p++) *p = (char) tolower((unsigned char) *p);
            if (!show) show = strstr(hay, q) != NULL;
        }
        html_dom_set_style(items[i], "display", show ? "inline-block" : "none");
    }
    free(items);
    return 1;
}

static int h_select_tex(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { (void) u; (void) ev; return select_item("tex", a, n); }
static int h_select_model(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { (void) u; (void) ev; return select_item("model", a, n); }
static int h_select_spray(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { (void) u; (void) ev; return select_item("spray", a, n); }
static int h_select_skin(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) { (void) u; (void) ev; return select_item("skin", a, n); }

void ed_inspector_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.selectTex", h_select_tex},
        {"KE.selectModelVariant", h_select_model},
        {"KE.selectSpray", h_select_spray},
        {"KE.selectSkin", h_select_skin},
        {"KE.searchItems", h_search},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
