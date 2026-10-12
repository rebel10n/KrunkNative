// Map editor logic UI: the "Trigger Event" and "Trigger Actions" folders of a TRIGGER or
// AI_SPAWNER object (Krunker v7.1.4 ui/editor.js UI:4080-4500) and ObjectInstance.addAction /
// removeAction / selectActionAudio (OI:1968-2008). doc/v7/editor.md WP4 (sections 1.7 / 3.7).
// editor_inspector.c calls ed_triggers_panel inside "Technical & Logic"; the runtime meaning of
// events, conditions and actions is shared/src/triggers_v7.c's (doc/v7/map-logic.md).
//
// The controls bind ObjectInstance properties through ed_prop_get / ed_prop_set: the trigger's
// own ("event", "constantEvent", "chance", "actionMethod", "actionCost", "canTrigger") and
// action N's as "<prop>@N" ("action@2", "constant@2", "conditionVal0@2", "snd@2", ...), which
// editor_model.c stores in tas[N] with serialize()'s rules. Objects are linked only by typed ids
// (interface ID / Target, in the inspector); there is no click-to-link, as in v7.
//
// Intentional differences from v7.1.4:
// - Condition values picked from a select are stored as option index strings, as v7's
//   GUI.action does ("2"); the runtime parses both (triggers_conditions.c). Same bytes as v7.
// - An action that is devOnly (hidden on krunker.io, config.isProd) keeps its value when the
//   object is loaded; its select then shows no option, as v7's.
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include "editor_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cJSON *table(const char *key) { return cJSON_GetObjectItemCaseSensitive(editor_data_get("triggers"), key); }

static int prop_int(const EditorObject *o, const char *prop) {
    cJSON *v = ed_prop_get(o, prop);
    const int r = cJSON_IsNumber(v) ? (int) v->valuedouble : cJSON_IsString(v) ? atoi(v->valuestring) : cJSON_IsTrue(v);
    cJSON_Delete(v);
    return r;
}

// "<prop>@<index>" (a few live at once in one control row: a small ring)
static const char *at(const char *prop, const int index) {
    static char ring[8][64];
    static int next;
    char *out = ring[next++ & 7];
    snprintf(out, 64, "%s@%d", prop, index);
    return out;
}

static int is_trigger(const EditorObject *o) { return o && (o->prefab == ED_PREFAB_TRIGGER || editor_prefab_has(o->prefab, EP_AI)); }

int ed_triggers_event_is(const EditorObject *o, const char *event_name) {
    if (!o || o->prefab != ED_PREFAB_TRIGGER) return 0;
    const cJSON *ev = cJSON_GetArrayItem(table("events"), prop_int(o, "event"));
    return ev && !strcmp(ed_json_str(ev, "name", ""), event_name);
}

static void refresh_panel(EdCtl *c, const cJSON *v) { (void) c; (void) v; g_ed.dirty_panel = 1; }

// v7's panel build: `if (!action || !action.hasCust) trigActions[i].constantType = 0`. It runs
// on the refresh right after the Action select changes and whenever the panel shows the object.
static void normalize_value_type(EditorObject *o, const int i) {
    const cJSON *ad = cJSON_GetArrayItem(table("actions"), prop_int(o, at("action", i)));
    if ((ad && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(ad, "hasCust"))) || !prop_int(o, at("constantType", i))) return;
    cJSON *zero = cJSON_CreateNumber(0);
    ed_prop_set(o, at("constantType", i), zero);
    cJSON_Delete(zero);
}

static void on_action_change(EdCtl *c, const cJSON *v) {
    const char *suffix = strchr(c->key, '@');
    if (suffix && g_ed.proj->selected) normalize_value_type(g_ed.proj->selected, atoi(suffix + 1));
    refresh_panel(c, v);
}

// the actions of the object as v7's trigActions: at least one (the constructor always adds one)
static int action_count(const EditorObject *o) {
    const int n = ed_model_action_count(o->data);
    return n > 0 ? n : 1;
}

static int action_limit(void) { return (int) ed_json_num(editor_data_get("limits"), "triggerActionLimit", 5); }

static int add_action(EditorObject *o) {
    // addAction(): a new default action while below triggerActionLimit
    if (!is_trigger(o)) return 0;
    if (!ed_model_action_count(o->data)) {
        cJSON *zero = cJSON_CreateNumber(0); // tas[0] exists before the second action is added
        ed_prop_set(o, "action@0", zero);
        cJSON_Delete(zero);
    }
    if (!ed_model_action_add(&o->data)) return 0;
    ed_mark_changed();
    g_ed.dirty_panel = 1; // updateObjConfigGUI
    return 1;
}

static int remove_action(EditorObject *o, const int index) {
    // removeAction(id): any action but the first (the panel never offers action 0)
    if (!is_trigger(o) || !ed_model_action_remove(&o->data, index)) return 0;
    ed_mark_changed();
    g_ed.dirty_panel = 1;
    return 1;
}

static void on_add(EdCtl *c) { (void) c; add_action(g_ed.proj->selected); }
static void on_remove(EdCtl *c) { remove_action(g_ed.proj->selected, c->arg); }
static void on_select_audio(EdCtl *c) { ed_asset_target_open(ED_ASSET_ACTION_AUDIO, c->arg); } // KE.activeAction = id; viewSounds

// KE.soundsList: {name: index} over config.emitterSounds (a repeated name keeps its first position
// and takes the later index)
static const EdOption *sound_options(int *count) {
    static EdOption *opts;
    static int n = -1;
    if (n < 0) {
        const cJSON *names = editor_data_get("emitterSounds");
        const int total = cJSON_GetArraySize(names);
        opts = calloc((size_t) (total > 0 ? total : 1), sizeof(EdOption));
        n = 0;
        for (int i = 0; opts && i < total; i++) {
            const cJSON *it = cJSON_GetArrayItem(names, i);
            const char *name = cJSON_IsString(it) ? it->valuestring : "";
            int dup = -1;
            for (int k = 0; k < n && dup < 0; k++) if (!strcmp(opts[k].name, name)) dup = k;
            if (dup >= 0) opts[dup].value = i;
            else opts[n++] = (EdOption) {name, i, NULL};
        }
    }
    *count = n;
    return opts;
}

// actionOptions / actionOptions2 (index -> name) as select options
static EdOption *index_options(const cJSON *names, int *count) {
    int n;
    EdOption *o = edp_opts_from_names(names, &n);
    *count = n;
    return o;
}

// condition select options with string values ("0", "1", ...): GUI.action stores a select's
// value as the option's string
static EdOption *string_index_options(const cJSON *names, int *count) {
    static char values[64][4];
    const int n = cJSON_GetArraySize(names);
    EdOption *o = edp_opts_alloc(n);
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(names, i);
        snprintf(values[i & 63], sizeof(values[0]), "%d", i);
        o[i] = (EdOption) {cJSON_IsString(it) ? it->valuestring : "", i, values[i & 63]};
    }
    *count = n;
    return o;
}

static void action_folder(EdBuild *b, EditorObject *o, const int i, const int event_valid) {
    const cJSON *actions = table("actions");
    char title[32], state[64];
    snprintf(title, sizeof(title), "Action %d", i);
    snprintf(state, sizeof(state), "object.tech.actions.a%d", i);
    ed_gui_folder_begin(b, title, 1, 1, 1, state);
    if (i > 0) {
        char label[48];
        snprintf(label, sizeof(label), "Remove Action %d", i);
        ed_gui_button(b, label, on_remove)->arg = i;
    }
    // Action: every action but the devOnly ones (the page is production, as krunker.io)
    EdOption *ac = edp_opts_alloc(cJSON_GetArraySize(actions));
    int n = 0, k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, actions) {
        if (!ed_json_truthy(cJSON_GetObjectItemCaseSensitive(it, "devOnly"))) ac[n++] = (EdOption) {ed_json_str(it, "name", ""), k, NULL};
        k++;
    }
    edp_obj_select(b, "Action", at("action", i), ac, n, 1, on_action_change);

    const int a = prop_int(o, at("action", i));
    const cJSON *ad = cJSON_GetArrayItem(actions, a);
    const int has_cust = ad && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(ad, "hasCust"));
    normalize_value_type(o, i);
    const int ct = has_cust ? prop_int(o, at("constantType", i)) : 0;
    if (has_cust) {
        int m;
        EdOption *types = edp_opts_from_names(editor_data_get("inputTypes"), &m);
        edp_obj_select(b, "Value Type", at("constantType", i), types, m, 0, refresh_panel);
        if (ct) edp_obj_text(b, ed_json_str(ad, "actionValueCust", "Key"), at("constantCust", i), "Text", 0, NULL);
    }
    const char *type = ed_json_str(ad, "type", "");
    const int bypass = ad && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(ad, "bypassType"));
    if (ad && (!strcmp(type, "Both") || !strcmp(type, "Text")) && (bypass || !ct))
        edp_obj_text(b, ed_json_str(ad, "actionValueTxt", "Value"), at("constantTxt", i), "Text", 0, NULL);
    if (ad && (!strcmp(type, "Both") || strcmp(type, "Text")) && !ct) {
        const char *name = ed_json_str(ad, "actionValue", "Action Value");
        const cJSON *opts = cJSON_GetObjectItemCaseSensitive(ad, "actionOptions");
        if (opts) {
            int m;
            EdOption *o2 = index_options(opts, &m);
            edp_obj_select(b, name, at("constant", i), o2, m, 0, NULL);
        } else {
            edp_obj_slider(b, name, at("constant", i), -10000, 10000, 1, 0, NULL);
        }
    }
    if (ad && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(ad, "hasExtra")) && (bypass || !ct)) {
        const char *name = ed_json_str(ad, "actionValue2", "Action Value");
        const cJSON *opts = cJSON_GetObjectItemCaseSensitive(ad, "actionOptions2");
        if (opts) {
            int m;
            EdOption *o2 = index_options(opts, &m);
            edp_obj_select(b, name, at("constant2", i), o2, m, 0, NULL);
        } else {
            edp_obj_slider(b, name, at("constant2", i), -10000, 10000, 1, 0, NULL);
        }
    }
    if (ad) edp_obj_slider(b, "Delay", at("delay", i), 0, 60, 0.1, 0, NULL);

    if (event_valid) {
        // Condition: the select, then one control per vals[] entry (text / select / slider)
        const cJSON *conds = table("conditions");
        snprintf(state, sizeof(state), "object.tech.actions.a%d.condition", i);
        ed_gui_folder_begin(b, "Condition", 1, 1, 1, state);
        EdOption *co = edp_opts_alloc(cJSON_GetArraySize(conds));
        n = 0;
        cJSON_ArrayForEach(it, conds) {
            co[n] = (EdOption) {ed_json_str(it, "name", ""), n, NULL};
            n++;
        }
        edp_obj_select(b, "Condition", at("condition", i), co, n, 0, refresh_panel);
        const cJSON *cd = cJSON_GetArrayItem(conds, prop_int(o, at("condition", i)));
        int j = 0;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(cd, "vals")) {
            char key[24];
            snprintf(key, sizeof(key), "conditionVal%d", j++);
            const char *name = ed_json_str(it, "name", "");
            const cJSON *select = cJSON_GetObjectItemCaseSensitive(it, "select");
            if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(it, "input"))) edp_obj_text(b, name, at(key, i), name, 0, NULL);
            else if (select) {
                int m;
                EdOption *so = string_index_options(select, &m);
                edp_obj_select(b, name, at(key, i), so, m, 0, NULL);
            } else {
                edp_obj_slider(b, name, at(key, i), ed_json_num(it, "min", 0), ed_json_num(it, "max", 1), ed_json_num(it, "step", 0.1), 0, NULL);
            }
        }
        ed_gui_folder_end(b, 1);
    }
    if (a == 14 || a == 34 || a == 54) {
        // Sound: Broadcast Sound / Send Sound / Stop Sound
        snprintf(state, sizeof(state), "object.tech.actions.a%d.sound", i);
        ed_gui_folder_begin(b, "Sound", 1, 1, 1, state);
        int m;
        const EdOption *snd = sound_options(&m);
        edp_obj_select(b, "Type", at("snd", i), snd, m, 0, NULL);
        EdCtl *num = ed_gui_number(b, "Asset ID", "Sound Asset ID", 0, 0);
        ed_ctl_bind_obj(num, at("assetSound", i));
        edp_finish(b, num, NULL);
        ed_gui_button(b, "Select Custom", on_select_audio)->arg = i;
        edp_obj_slider(b, "Volume", at("vol", i), 0.05, 2, 0.01, 0, NULL); // emitterVolMin / Max
        edp_obj_slider(b, "Rate", at("rate", i), 0.1, 4, 0.1, 0, NULL);    // emitterRateMin / Max
        static const EdOption from[] = {{"Trigger (3D)", 0, NULL}, {"Player (3D)", 1, NULL}, {"Global (2D)", 2, NULL}};
        edp_obj_select(b, "Emit From", at("emitfrom", i), from, 3, 0, NULL);
        ed_gui_folder_end(b, 1);
    }
    ed_gui_folder_end(b, 1);
    ed_gui_rule(b); // "rul" + i
}

void ed_triggers_panel(EdBuild *b, EditorObject *o) {
    if (!is_trigger(o)) return;
    const int ai = editor_prefab_has(o->prefab, EP_AI);
    const cJSON *events = table("events");

    // Trigger Event: AI_SPAWNER sees the "onAI" events, TRIGGER the others
    ed_gui_folder_begin(b, "Trigger Event", 1, 1, 0, "object.tech.events");
    if (ai) edp_obj_switch(b, "Can Trigger", "canTrigger", 0, NULL);
    EdOption *ev = edp_opts_alloc(cJSON_GetArraySize(events));
    int n = 0, k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, events) {
        const char *name = ed_json_str(it, "name", "");
        const int ai_event = strstr(name, "onAI") != NULL;
        if (ai ? ai_event : !ai_event) ev[n++] = (EdOption) {name, k, NULL};
        k++;
    }
    edp_obj_select(b, "Event", "event", ev, n, 0, refresh_panel);
    edp_obj_slider(b, "Event Value", "constantEvent", 0, 10000, 1, 0, NULL);
    ed_gui_folder_end(b, 1);

    // Trigger Actions
    const cJSON *event = cJSON_GetArrayItem(events, prop_int(o, "event"));
    ed_gui_folder_begin(b, "Trigger Actions", 1, 1, 0, "object.tech.actions");
    if (event && !strcmp(ed_json_str(event, "name", ""), "onInteractKey")) {
        static const EdOption methods[] = {
            {"Cost KR", 0, NULL}, {"Cost Score", 1, NULL}, {"Require Score", 2, NULL}, {"Require Kills", 3, NULL}, {"Requires Account", 4, NULL},
            {"Requires Verified", 5, NULL}, {"Requires Premium", 6, NULL}, {"Requires Partner", 7, NULL}, {"Requires Level", 8, NULL},
        };
        edp_obj_select(b, "Action Method", "actionMethod", methods, 9, 0, NULL);
        edp_obj_slider(b, "Action Cost", "actionCost", 0, 10000, 1, 0, NULL);
    }
    edp_obj_slider(b, "Action Chance (%)", "chance", 0, 100, 1, 0, NULL);
    const int count = action_count(o);
    if (count < action_limit()) ed_gui_button(b, "Add Action", on_add);
    for (int i = 0; i < count; i++) action_folder(b, o, i, event != NULL);
    ed_gui_folder_end(b, 1);
}

// KRUNKNATIVE_EDITOR_SCRIPT commands of the logic UI (editor.c run_script_text), on the selected
// object, for the parity tests: trig:add (Add Action), trig:remove:N (Remove Action N),
// trig:set:<prop>:<json> (a control's GUI.action: "trig:set:condition@1:5"). 1 = handled.
int ed_triggers_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "trig") || !arg) return 0;
    EditorObject *o = g_ed.proj->selected;
    if (!strcmp(arg, "add")) add_action(o);
    else if (!strncmp(arg, "remove:", 7)) remove_action(o, atoi(arg + 7));
    else if (!strncmp(arg, "set:", 4) && o) {
        char prop[64];
        const char *colon = strchr(arg + 4, ':');
        if (!colon || (size_t) (colon - arg - 4) >= sizeof(prop)) return 1;
        snprintf(prop, sizeof(prop), "%.*s", (int) (colon - arg - 4), arg + 4);
        cJSON *v = cJSON_Parse(colon + 1);
        if (v) {
            ed_prop_set(o, prop, v);
            if (!strncmp(prop, "action@", 7)) normalize_value_type(o, atoi(prop + 7)); // as on_action_change
            ed_mark_changed();
            g_ed.dirty_panel = 1;
            cJSON_Delete(v);
        }
    }
    return 1;
}
