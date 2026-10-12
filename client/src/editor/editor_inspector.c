// Map editor object panel: Krunker ui/editor.js `panel.right.object` (the object inspector).
// Split from editor_panels.c (doc/v7/editor.md WP1); doc/v7/editor.md WP3 owns it.
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void refresh_panel(EdCtl *c, const cJSON *v) { (void) c; (void) v; g_ed.dirty_panel = 1; }
static void rebuild_sel(EdCtl *c, const cJSON *v) { (void) c; (void) v; if (g_ed.proj->selected) ed_obj_rebuild(g_ed.proj->selected); }

static void on_faces(EdCtl *c, const cJSON *v) {
    // object: e.objConfig.faces, key: index -> A.faces = objConfig.faces; A.regen()
    EditorObject *o = g_ed.proj->selected;
    if (!o) return;
    cJSON *faces = ed_prop_get(o, "faces");
    const int index = c->arg;
    cJSON_ReplaceItemInArray(faces, index, cJSON_CreateBool(ed_json_truthy(v)));
    ed_prop_set(o, "faces", faces);
    cJSON_Delete(faces);
    ed_obj_rebuild(o);
}

// The object panel's controls bind ObjectInstance properties by name (ed_prop_get / ed_prop_set,
// editor_model.c's table); the few v3.1.1 controls v7 has no field for (projType, showAd,
// dynamic) fall back to EditorObject.scratch, which is never exported.

// SHOWCASE "Item": Editor.skinList = every STORE.skins entry but types 4/5, named
// "<name> [<getItemDesc>]" (assets/skins.json is the store's skin table in index order).
static EdOption *g_skin_opts;
static int g_skin_count = -1;

static const EdOption *skin_options(int *count) {
    if (g_skin_count < 0) {
        g_skin_count = 0;
        char *path = concat(assets_path(), "skins.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        cJSON *skins = NULL;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) skins = cJSON_ParseWithLength((const char *) buf, len);
        free(buf);
        free(path);
        const cJSON *weapons = editor_data_get("weapons");
        const int n = cJSON_GetArraySize(skins);
        g_skin_opts = n > 0 ? calloc((size_t) n, sizeof(EdOption)) : NULL;
        for (int i = 0; g_skin_opts && i < n; i++) {
            const cJSON *t = cJSON_GetArrayItem(skins, i);
            const int type = (int) ed_json_num(t, "type", 0);
            if (type == 4 || type == 5) continue;
            const char *desc = type == 1 ? "Head" : type == 2 ? "Back" : type == 3 ? "Melee" : type == 6 ? "Waist" : type == 7 ? "Face"
                             : ed_json_str(cJSON_GetArrayItem(weapons, (int) ed_json_num(t, "weapon", 1) - 1), "name", "");
            const char *name = ed_json_str(t, "name", "");
            const size_t sz = strlen(name) + strlen(desc) + 4;
            char *label = malloc(sz);
            if (!label) continue;
            snprintf(label, sz, "%s [%s]", name, desc);
            // skinList is an object keyed by the label: a repeated label keeps its first
            // position and takes the later index
            int dup = -1;
            for (int k = 0; k < g_skin_count && dup < 0; k++) if (!strcmp(g_skin_opts[k].name, label)) dup = k;
            if (dup >= 0) {
                g_skin_opts[dup].value = i;
                free(label);
                continue;
            }
            g_skin_opts[g_skin_count++] = (EdOption) {label, i, NULL};
        }
        cJSON_Delete(skins);
    }
    *count = g_skin_count;
    return g_skin_opts;
}

static void regen_sel_panel(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    if (g_ed.proj->selected) ed_obj_rebuild(g_ed.proj->selected);
    g_ed.dirty_panel = 1;
}

static int g_panel_action; // the selected TRIGGER's action when the panel was built

static int action_is_text(const int a) {
    const cJSON *act = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(editor_data_get("triggers"), "actions"), a);
    return act && (!strcmp(ed_json_str(act, "type", ""), "Text") || ed_json_truthy(cJSON_GetObjectItemCaseSensitive(act, "custom")));
}

static void on_trigger_action(EdCtl *c, const cJSON *v) {
    // Action onChange: refresh the panel when the old or the new action is Text / custom
    (void) c;
    const int now = (int) cJSON_GetNumberValue(v);
    if (action_is_text(g_panel_action) || action_is_text(now)) g_ed.dirty_panel = 1;
    g_panel_action = now;
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
    const int p = o->prefab;
    const EditorPrefab *pf = &editor_prefabs[p];
#define HAS(f) ((pf->flags & (f)) != 0)
    EdBuild b;
    edp_right_begin(&b, "object");
    ed_gui_panel_buttons(&b, 1, pf->label, 0);

    if (HAS(EP_AI)) edp_obj_text(&b, "AI Name", "aiName", "BOT", 0, NULL);
    if (!HAS(EP_TOOL) || HAS(EP_BOUNCE)) edp_obj_switch(&b, "Visible", "visible", 0, NULL);
    if (!HAS(EP_TOOL) && !HAS(EP_FORCECOLLISION) && !HAS(EP_NOCOLLISION)) edp_obj_switch(&b, "Collidable", "collidable", 0, NULL);
    if (HAS(EP_EDITPEN)) edp_obj_switch(&b, "Penetrable", "penetrable", 0, NULL);
    if (HAS(EP_CANWALLJUMP)) edp_obj_switch(&b, "Wall Jumpable", "canWallJump", 0, NULL);
    if (HAS(EP_BOOSTABLE)) edp_obj_slider(&b, "Boost", "boost", -10, 10, 0.1, 0, NULL);
    if (HAS(EP_BOUNCE)) {
        edp_obj_slider(&b, "Bounce", "bounce", 0.1, 3, 0.1, 0, NULL);
        edp_obj_switch(&b, "Crouch Reduces", "crouch", 0, NULL);
    }
    if (HAS(EP_EDGENOISE)) edp_obj_slider(&b, "Edge Noise", "edgeNoise", -5, 5, 0.1, 0, NULL);
    if (HAS(EP_HASBORDER)) edp_obj_switch(&b, "Border", "border", 0, NULL);
    if (HAS(EP_HASSPIN)) edp_obj_slider(&b, "Spin", "spin", -20, 20, 0, 0, NULL);
    if (!strcmp(pf->name, "CYLINDER")) edp_obj_switch(&b, "Open Ended", "openEnded", 0, NULL);
    if (HAS(EP_CUSTOMDIRECTION)) edp_obj_slider(&b, "Direction", "direction", 0, 3, 1, 0, NULL);
    if (HAS(EP_OPTIONALDIRECTION)) edp_obj_switch(&b, "Force Direction", "forceDir", 0, NULL);
    if (HAS(EP_TEAMABLE)) {
        static const EdOption two[] = {{"Team 1", 0, NULL}, {"Team 2", 1, NULL}};
        static const EdOption three[] = {{"Default", 0, NULL}, {"Team 1", 1, NULL}, {"Team 2", 2, NULL}};
        if (HAS(EP_NODEFAULT)) edp_obj_select(&b, "Team", "team", two, 2, 0, NULL);
        else edp_obj_select(&b, "Team", "team", three, 3, 0, NULL);
    }
    if (HAS(EP_GEN)) edp_obj_slider(&b, "Model Scale", "mSize", 0.1, 100, 0.1, 0, NULL);
    if (!strcmp(pf->name, "SHOWCASE")) {
        int n;
        const EdOption *skins = skin_options(&n);
        edp_obj_select(&b, "Item", "itemID", skins, n, 0, rebuild_sel);
    }
    if (HAS(EP_OBJECTIVELINK)) edp_obj_slider(&b, "Objective Id", "objectiveLink", -1, 10, 1, 0, NULL); // v7: "obl"
    ed_gui_rule(&b);

    ed_gui_folder_begin(&b, "Transform", 0, 0, 0, "object.transform");
    edp_obj_vector3(&b, "Position", "pos", NULL);
    edp_obj_vector3(&b, "Rotation", "rot", NULL);
    if (HAS(EP_SCALABLE)) edp_obj_vector3(&b, "Size", "size", NULL);
    ed_gui_folder_end(&b, 0);

    if (!HAS(EP_NOTSTYLEABLE)) {
        ed_gui_folder_begin(&b, "Style", 0, 0, 0, "object.style");
        if (HAS(EP_TEXTURABLE) || HAS(EP_MOVINGTEXTURE)) {
            ed_gui_folder_begin(&b, "Texture", 1, 1, 0, "object.style.texture");
            if (HAS(EP_TEXTURABLE)) {
                // textureList: Default first, then every other texture by formatted name
                EdOption *opts = edp_opts_alloc(EDITOR_TEXTURE_COUNT);
                int n = 0;
                opts[n++] = (EdOption) {"Default", 0, "DEFAULT"};
                for (int i = 0; i < EDITOR_TEXTURE_COUNT; i++) {
                    if (!strcmp(editor_textures[i], "DEFAULT")) continue;
                    const int slot = (int) (&opts[n] - g_opt_pool);
                    opts[n++] = (EdOption) {edp_const_name(editor_textures[i], g_opt_names[slot], sizeof(g_opt_names[slot])), 0, editor_textures[i]};
                }
                edp_obj_select(&b, "Type", "texture", opts, n, 1, refresh_panel);
                if (HAS(EP_ASSETTEXTURES)) {
                    edp_obj_number(&b, "Asset ID", "assetTexture", "Texture Asset ID", 1);
                    ed_gui_button(&b, "Select Custom", ed_net_select)->arg = 1; // selectTexture
                }
                if (HAS(EP_CANSTRETCH)) edp_obj_switch(&b, "Stretch", "texStretch", 1, NULL);
                ed_gui_folder_begin(&b, "Offset", 1, 0, 1, "object.style.texture.offset");
                edp_obj_slider(&b, "X", "texOffX", 0, 1, 0.1, 2, NULL);
                edp_obj_slider(&b, "Y", "texOffY", 0, 1, 0.1, 2, NULL);
                ed_gui_folder_end(&b, 1);
            }
            if (HAS(EP_MOVINGTEXTURE)) {
                static const EdOption axes[] = {{"X", 0, NULL}, {"Y", 1, NULL}};
                ed_gui_folder_begin(&b, "Movement", 1, 0, 1, "object.style.texture.movement");
                edp_obj_slider(&b, "Speed", "movingSpeed", -20, 20, 0.1, 2, NULL);
                edp_obj_select(&b, "Axis", "movingAxis", axes, 2, 2, NULL);
                ed_gui_folder_end(&b, 1);
            }
            ed_gui_folder_end(&b, 1);
        }
        if (HAS(EP_AI)) {
            edp_obj_text(&b, "Asset ID", "assetID", "ID", 1, regen_sel_panel);
            ed_gui_button(&b, "Select Custom", ed_net_select)->arg = 0; // selectModel
        }
        if (HAS(EP_EDITCOLOR)) edp_obj_color(&b, "Color", "color", 0);
        if (HAS(EP_EDITEMISSIVE)) edp_obj_color(&b, "Emissive", "emissive", 0);
        if (HAS(EP_EDITOPAC)) edp_obj_slider(&b, "Opacity", "opacity", 0, 1, 0.1, 0, NULL);
        if (HAS(EP_CUSTOM)) edp_obj_switch(&b, "Texture Smoothing", "texSmooth", 0, NULL);
        if (!HAS(EP_COMPLEX) && !HAS(EP_TOOL)) edp_obj_switch(&b, "Shading", "ambient", 0, NULL);
        if (HAS(EP_EDITFOG)) edp_obj_switch(&b, "Fog", "fog", 0, NULL);
        if (!strcmp(pf->name, "LIGHT_CONE")) edp_obj_switch(&b, "Full Cone", "fullcone", 0, NULL);
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_EDITFACES)) {
        static const char *faces[6] = {"Right", "Left", "Top", "Bottom", "Back", "Front"};
        ed_gui_folder_begin(&b, "Render Faces", 0, 0, 0, "object.faces");
        cJSON *fv = ed_prop_get(o, "faces");
        for (int i = 0; i < 6; i++) {
            EdCtl *c = ed_gui_switch(&b, faces[i], 0);
            char key[4];
            snprintf(key, sizeof(key), "%d", i);
            cJSON *tmp = cJSON_CreateObject();
            cJSON_AddItemToObject(tmp, key, cJSON_Duplicate(cJSON_GetArrayItem(fv, i), 1));
            ed_ctl_bind_json(c, tmp, key);
            edp_finish(&b, c, on_faces);
            c->arg = i; // after the markup: arg is the line's sub level until then
            c->bind = EB_NONE; // value handled by on_faces
            c->json = NULL;
            cJSON_Delete(tmp);
        }
        cJSON_Delete(fv);
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_LIGHT)) {
        ed_gui_folder_begin(&b, "Lighting", 0, 0, 0, "object.lighting");
        edp_obj_slider(&b, "Intensity", "intensity", 0, 2, 0.1, 0, NULL);
        edp_obj_slider(&b, "Distance", "distance", 0, 1000, 0.1, 0, NULL);
        edp_obj_slider(&b, "Decay", "decay", 0, 2, 0.1, 0, NULL);
        ed_gui_folder_end(&b, 0);
    }

    if (!HAS(EP_NOTTECHNICAL)) {
        // The Technical folder holds the optional controls below; Krunker only adds it when
        // any applies. Build it into a side buffer and keep it when non-empty.
        EdBuild t = b;
        EdBuf saved = b.html;
        edb_init(&t.html);
        b.html = t.html;
        const size_t ctl_marker = 0;
        (void) ctl_marker;
        int any = 0;
        if (!strcmp(pf->name, "DEPOSIT_BOX")) {
            edp_obj_slider(&b, "Withdraw Amount", "withdraw", 0, 100000, 1, 0, NULL);
            edp_obj_slider(&b, "Deposit Amount", "deposit", 0, 100000, 1, 0, NULL);
            any = 1;
        }
        int custom_score = 0;
        if (HAS(EP_CUSTOMSCORE)) custom_score = 1;
        if (!strcmp(pf->name, "CHECK_POINT") || !strcmp(pf->name, "TRIGGER") || HAS(EP_AI)) custom_score = 2;
        if (custom_score == 1) edp_obj_slider(&b, HAS(EP_INTERACT) ? "Amount" : "Score", "score", HAS(EP_INTERACT) || HAS(EP_AI) ? 0 : 1, 10000, 1, 0, NULL);
        if (custom_score == 2) edp_obj_switch(&b, "Single Use", "single", 0, NULL);
        any |= custom_score != 0;
        if (HAS(EP_INTERACT) || HAS(EP_INTERACT2)) {
            if (HAS(EP_INTERACT)) {
                static const EdOption methods[] = {{"Cost Score", 0, NULL}, {"Require Score", 1, NULL}, {"Require Kills", 2, NULL}};
                edp_obj_select(&b, "Method", "method", methods, 3, 0, NULL);
                if (HAS(EP_CANUNDO)) {
                    edp_obj_switch(&b, "Closeable", "closeable", 0, NULL);
                    edp_obj_switch(&b, "Starts Hidden", "startClosed", 0, NULL);
                }
            }
            edp_obj_switch(&b, "No Popup", "hidden", 0, NULL);
            any = 1;
        }
        if (HAS(EP_HASHEALTH)) {
            edp_obj_switch(&b, "Destructible", "canDest", 0, NULL);
            if (strcmp(pf->name, "GATE")) edp_obj_switch(&b, "Start Destroyed", "startDest", 0, NULL);
            if (!strcmp(pf->name, "TRIGGER")) edp_obj_switch(&b, "Destroy On Use", "selfDestruct", 0, NULL);
            edp_obj_slider(&b, "Health", "health", 0, 20000, 10, 0, NULL);
            ed_gui_folder_begin(&b, "Respawn", 1, 0, 0, "object.tech.respawn");
            edp_obj_slider(&b, "Timer (s)", "respawnT", 0, 300, 0.1, 1, NULL);
            edp_obj_switch(&b, "Random", "respawnR", 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (!strcmp(pf->name, "WEAPON_PICKUP")) {
            EdOption *opts = edp_opts_alloc(32);
            int n = 0, wi = 0;
            const cJSON *w;
            cJSON_ArrayForEach(w, editor_data_get("weapons")) {
                if (ed_json_str(w, "src", NULL)) opts[n++] = (EdOption) {ed_json_str(w, "name", ""), wi, NULL};
                wi++;
            }
            ed_gui_folder_begin(&b, "Weapon", 1, 0, 0, "object.tech.weapon");
            edp_obj_select(&b, "Type", "weaponId", opts, n, 1, rebuild_sel);
            edp_obj_switch(&b, "Replaceable", "weaponReplace", 1, NULL);
            edp_obj_switch(&b, "Random", "randWep", 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (!strcmp(pf->name, "TRIGGER") || HAS(EP_AI)) {
            const cJSON *trig = editor_data_get("triggers");
            const cJSON *events = cJSON_GetObjectItemCaseSensitive(trig, "events");
            const cJSON *actions = cJSON_GetObjectItemCaseSensitive(trig, "actions");
            if (HAS(EP_AI)) edp_obj_switch(&b, "Can Trigger", "canTrigger", 0, NULL);
            EdOption *ev = edp_opts_alloc(cJSON_GetArraySize(events));
            int n = 0, i = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, events) {
                const char *name = cJSON_IsString(it) ? it->valuestring : "";
                const int ai_event = strstr(name, "onAI") != NULL;
                if ((HAS(EP_AI) && ai_event) || (!strcmp(pf->name, "TRIGGER") && !ai_event)) ev[n++] = (EdOption) {name, i, NULL};
                i++;
            }
            edp_obj_select(&b, "Event", "event", ev, n, 0, NULL);
            edp_obj_slider(&b, "Event Value", "constantEvent", 0, 10000, 1, 0, NULL);
            EdOption *ac = edp_opts_alloc(cJSON_GetArraySize(actions));
            n = 0;
            i = 0;
            cJSON_ArrayForEach(it, actions) ac[n++] = (EdOption) {ed_json_str(it, "name", ""), i++, NULL};
            edp_obj_select(&b, "Action", "action", ac, n, 0, on_trigger_action);
            cJSON *action = ed_prop_get(o, "action");
            g_panel_action = (int) cJSON_GetNumberValue(action);
            const cJSON *cur = cJSON_GetArrayItem(actions, (int) cJSON_GetNumberValue(action));
            cJSON_Delete(action);
            if (cur && !strcmp(ed_json_str(cur, "type", ""), "Text")) edp_obj_text(&b, "Value", "constantTxt", "Text", 0, NULL);
            else edp_obj_slider(&b, "Value", "constant", -10000, 10000, 1, 0, NULL);
            edp_obj_slider(&b, "Action Chance (%)", "chance", 0, 100, 1, 0, NULL);
            any = 1;
        }
        const int iface = strcmp(pf->name, "WEAPON_PICKUP") && HAS(EP_CANINTERFACE);
        if (iface || HAS(EP_CANTARGETINTERFACE)) {
            ed_gui_folder_begin(&b, "Interface", 1, 0, 0, "object.tech.interface");
            if (iface) edp_obj_slider(&b, "ID", "interfaceId", 0, ed_json_num(editor_data_json(), "interfaceIDMax", 1000), 1, 1, NULL);
            if (HAS(EP_CANTARGETINTERFACE)) edp_obj_slider(&b, "Target", "interfaceIdT", 0, 1000, 1, 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (!strcmp(pf->name, "TELEPORTER")) {
            static const EdOption nodes[] = {{"Sending", 0, NULL}, {"Receiving", 1, NULL}, {"Both", 2, NULL}};
            edp_obj_select(&b, "Node", "node", nodes, 3, 0, NULL);
            edp_obj_slider(&b, "Channel", "channel", 0, 100, 1, 0, NULL);
            edp_obj_switch(&b, "Stop Momentum", "momentum", 0, NULL);
            any = 1;
        }
        if (HAS(EP_CANBEDYNAMIC)) {
            ed_gui_folder_begin(&b, "Dynamic", 1, 0, 0, "object.tech.dynamic");
            edp_obj_switch(&b, "Enabled", "dynamic", 1, NULL);
            edp_obj_slider(&b, "Gravity", "gravity", 0, 5, 0.1, 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        if (HAS(EP_AI)) {
            ed_gui_folder_begin(&b, "Behavior", 1, 0, 0, "object.tech.behavior");
            edp_obj_slider(&b, "Behavior Type", "behaviorType", 0, 20, 1, 1, NULL);
            edp_obj_slider(&b, "Speed", "speed", 0, 5, 0.1, 1, NULL);
            edp_obj_slider(&b, "Turn Speed", "turnSpd", 0, 30, 1, 1, NULL);
            edp_obj_slider(&b, "Gravity", "gravity", 0, 1, 0.1, 1, NULL);
            edp_obj_slider(&b, "Firerate", "frate", 200, 10000, 50, 1, NULL);
            edp_obj_slider(&b, "Vision Distance", "visDis", 10, 1000, 10, 1, NULL);
            edp_obj_switch(&b, "Xray Vision", "xrayVis", 1, NULL);
            edp_obj_switch(&b, "Can Shoot", "canShoot", 1, NULL);
            edp_obj_switch(&b, "Can Hit", "canHit", 1, NULL);
            edp_obj_slider(&b, "Projectile Type", "projType", 0, 5, 1, 1, NULL);
            edp_obj_text(&b, "Reward Type", "rewardType", "id,id,id", 1, NULL);
            ed_gui_folder_end(&b, 1);
            any = 1;
        }
        EdBuf inner = b.html;
        b.html = saved;
        if (any) {
            ed_gui_folder_begin(&b, "Technical", 0, 0, 0, "object.tech");
            edb_add(&b.html, inner.buf ? inner.buf : "");
            ed_gui_folder_end(&b, 0);
        }
        edb_free(&inner);
    }

    if (!strcmp(pf->name, "BILLBOARD")) {
        ed_gui_folder_begin(&b, "Billboard", 0, 0, 0, "object.billboard");
        edp_obj_slider(&b, "Poster", "poster", 1, 6, 1, 0, NULL);
        edp_obj_switch(&b, "Show AD", "showAd", 0, NULL);
        ed_gui_folder_end(&b, 0);
    }
    if (!strcmp(pf->name, "PARTICLES")) {
        int n;
        ed_gui_folder_begin(&b, "Particles", 0, 0, 0, "object.particles");
        EdOption *ids = edp_opts_from_names(editor_data_get("particleIDS"), &n);
        for (int i = 0; i < n; i++) {
            const int slot = (int) (&ids[i] - g_opt_pool);
            ids[i].name = edp_const_name(ids[i].name, g_opt_names[slot], sizeof(g_opt_names[slot]));
        }
        edp_obj_select(&b, "Texture", "part", ids, n, 0, NULL);
        EdOption *blend = edp_opts_from_names(editor_data_get("particleBlending"), &n);
        for (int i = 0; i < n; i++) {
            const int slot = (int) (&blend[i] - g_opt_pool);
            blend[i].name = edp_const_name(blend[i].name, g_opt_names[slot], sizeof(g_opt_names[slot]));
        }
        edp_obj_select(&b, "Blending", "partB", blend, n, 0, NULL);
        edp_obj_slider(&b, "Count", "partC", 0, 300, 1, 0, NULL);
        edp_obj_slider(&b, "Size", "partS", 0, 30, 0.1, 0, NULL);
        edp_obj_slider(&b, "Scale", "partSC", -10, 10, 0.1, 0, NULL);
        ed_gui_folder_begin(&b, "Movement", 1, 0, 0, "object.particles.movement");
        edp_obj_slider(&b, "Speed", "partSP", -10, 10, 0.1, 1, NULL);
        edp_obj_slider(&b, "Deceleration", "partD", -1, 1, 0.1, 1, NULL);
        edp_obj_slider(&b, "Gravity", "partG", -10, 10, 0.1, 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_HASTEXT)) {
        static const EdOption align[] = {{"Left", 0, NULL}, {"Center", 1, NULL}, {"Right", 2, NULL}};
        ed_gui_folder_begin(&b, "Text", 0, 0, 0, "object.text");
        edp_obj_text(&b, "Content", "text", "Content", 0, rebuild_sel);
        edp_obj_select(&b, "Align", "tAlign", align, 3, 0, NULL);
        ed_gui_folder_begin(&b, "Font", 1, 0, 0, "object.text.font");
        edp_obj_slider(&b, "Size", "fsize", 10, 120, 1, 1, NULL);
        edp_obj_color(&b, "Color", "fcolor", 1);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_begin(&b, "Background", 1, 0, 0, "object.text.background");
        edp_obj_color(&b, "Color", "bcolor", 1);
        edp_obj_switch(&b, "Visible", "backVisible", 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_end(&b, 0);
    }
    if (p == ED_PREFAB_SPAWN_POINT) {
        ed_gui_folder_begin(&b, "Spawn Point", 0, 0, 0, "object.spawn");
        edp_obj_switch(&b, "Starting Area", "cmpSpwn", 0, rebuild_sel);
        ed_gui_folder_end(&b, 0);
    }
    if (!strcmp(pf->name, "LIQUID")) {
        ed_gui_folder_begin(&b, "Wave", 0, 0, 0, "object.wave");
        ed_gui_folder_begin(&b, "Liquid", 1, 0, 0, "object.wave.liquid");
        edp_obj_slider(&b, "Speed", "wSpeed", 0, 25, 0.1, 1, NULL);
        edp_obj_slider(&b, "Height", "wHeight", 0, 5, 0.1, 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_begin(&b, "Depth", 1, 0, 0, "object.wave.depth");
        edp_obj_slider(&b, "X", "tilesX", 2, 100, 1, 1, NULL);
        edp_obj_slider(&b, "Z", "tilesZ", 2, 100, 1, 1, NULL);
        ed_gui_folder_end(&b, 1);
        ed_gui_folder_end(&b, 0);
    }
    cJSON *act = ed_prop_get(o, "action");
    const int action14 = HAS(EP_HASSOUND) && (int) cJSON_GetNumberValue(act) == 14;
    cJSON_Delete(act);
    if (!strcmp(pf->name, "SOUND_EMITTER") || action14) {
        int n;
        ed_gui_folder_begin(&b, "Sound", 0, 0, 0, "object.sound");
        EdOption *snd = edp_opts_alloc(cJSON_GetArraySize(editor_data_get("emitterSounds")));
        n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, editor_data_get("emitterSounds")) {
            snd[n] = (EdOption) {cJSON_IsString(it) ? it->valuestring : "", n, NULL};
            n++;
        }
        edp_obj_select(&b, "Type", "snd", snd, n, 0, NULL);
        edp_obj_number(&b, "Asset ID", "assetSound", "Sound Asset ID", 0);
        ed_gui_button(&b, "Select Custom", ed_net_select)->arg = 2; // selectAudio
        edp_obj_slider(&b, "Volume", "vol", 0.05, 2, 0.01, 0, NULL);
        edp_obj_slider(&b, "Rate", "rate", 0.5, 4, 0.1, 0, NULL);
        if (!strcmp(pf->name, "SOUND_EMITTER")) {
            edp_obj_slider(&b, "Reference Distance", "dist", 0, 5000, 1, 0, NULL);
            edp_obj_slider(&b, "Roll-off", "roll", 0, 1, 0.01, 0, NULL);
        } else {
            static const EdOption from[] = {{"Trigger", 0, NULL}, {"Player", 1, NULL}};
            edp_obj_select(&b, "Emit From", "emitfrom", from, 2, 0, NULL);
        }
        ed_gui_folder_end(&b, 0);
    }
    if (HAS(EP_AI)) {
        static const char *ids[4][3] = {{"Death Sound ID", "deathSound", "3"}, {"Idle Sound ID", "idleSound", "4"},
                                        {"Attack Sound ID", "shootSound", "5"}, {"Agro Sound ID", "agroSound", "6"}};
        ed_gui_folder_begin(&b, "Sound", 0, 0, 0, "object.sound");
        for (int i = 0; i < 4; i++) {
            edp_obj_number(&b, ids[i][0], ids[i][1], "Sound Asset ID", 0);
            ed_gui_button(&b, "Select Sound", ed_net_select)->arg = atoi(ids[i][2]);
        }
        ed_gui_folder_end(&b, 0);
    }
#undef HAS
    ed_gui_end(&b, g_ed.el_right_list);
}

void ed_select_texture(EdCtl *c) {
    (void) c;
    editor_show_window(7); // advancedOptions.selectTexture: showWindow(7) (editor_net.c)
}
