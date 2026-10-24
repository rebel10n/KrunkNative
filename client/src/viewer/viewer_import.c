// The viewer's file inputs and outputs (src/viewer.js J / Q / re / ee / te, the screenshot and
// render downloads) and the alert() / prompt() it calls.
//
// Q(): a picked or dropped file is
//   *.zip  a mod pack: every textures/<key>.png goes to RENDER.updateTexture(key), every
//          models/<key>.obj / .gltf / .glb to RENDER.updateMesh(key), then Z() (zip.js; here
//          client/src/zip_read.c over stb_image's inflate),
//   *.obj / *.gltf / *.glb  the model of the selected cosmetic (Load Custom <slot>: key
//          <type dir><id>[_<tex>]) or of the current weapon ("weapons/<src>"); a GLTF / GLB
//          replaces the animated model of a hasAnim item ("<key>.<fileExt>"); FBX is not
//          supported (v7's FBXLoader),
//   else   an image: the texture of that cosmetic, of the melee skin, or of the weapon skin.
// Load buttons open a file dialog (native: zenity / kdialog when installed; web: <input
// type=file>); files dropped on the window take the same path (container "drop" listener).
//
// Downloads land in $XDG_DOWNLOAD_DIR, else ~/Downloads, else the working directory natively
// (like ui_settings_export), and as a browser download on the web.
#include "viewer_internal.h"
#include "klog.h"
#include <cosmetics.h>
#include <html_ui.h>
#include <png_write.h>
#include <zip_read.h>
#include <stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define IMPORT_MAX_FILES 8
#define IMPORT_REBUILD_DELAY_MS 1000.0 // ne.close(): setTimeout(Z, 1000) once every entry is read

static struct {
    Client *client;
    int rebuild_timer;
    void (*prompt_done)(const char *text);
#ifndef __EMSCRIPTEN__
    GLFWdropfun previous_drop;
#endif
} g_import;

// ---------------------------------------------------------------- dialogs (alert / prompt)

static void dialog_show(const char *title, const char *message, const int with_input) {
    HtmlElem dialog = html_dom_by_id("knViewerDialog");
    if (!dialog) {
        html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END,
                             "<div id=\"knViewerDialog\" class=\"hidden\"><div class=\"knDialogBox\">"
                             "<div id=\"knViewerDialogText\"></div>"
                             "<input type=\"text\" id=\"knViewerDialogInput\" spellcheck=\"false\" autocomplete=\"off\">"
                             "<div class=\"knDialogButtons\"><div class=\"knDialogButton\" onclick=\"knViewerDialogOk()\">OK</div>"
                             "<div id=\"knViewerDialogCancel\" class=\"knDialogButton\" onclick=\"window.knViewerDialogCancel()\">Cancel</div></div>"
                             "</div></div>");
        dialog = html_dom_by_id("knViewerDialog");
    }
    if (!dialog) return;

    char text[512];
    snprintf(text, sizeof(text), "%s%s%s", title ? title : "", title && message ? "\n" : "", message ? message : "");
    html_dom_set_text(html_dom_by_id("knViewerDialogText"), text);
    const HtmlElem input = html_dom_by_id("knViewerDialogInput");
    html_dom_set_value(input, "");
    html_dom_set_display(input, with_input ? "" : "none");
    html_dom_set_display(html_dom_by_id("knViewerDialogCancel"), with_input ? "" : "none");
    html_dom_set_class(dialog, "hidden", 0);
    if (with_input) html_dom_focus(input);
}

static void dialog_hide(void) {
    const HtmlElem dialog = html_dom_by_id("knViewerDialog");
    if (dialog) html_dom_set_class(dialog, "hidden", 1);
}

void viewer_alert(const char *message) {
    KN_LOG_VERBOSE2("[viewer] alert: %s\n", message);
    g_import.prompt_done = NULL;
    dialog_show(NULL, message, 0);
}

void viewer_prompt(const char *title, void (*done)(const char *text)) {
    g_import.prompt_done = done;
    dialog_show(title, NULL, 1);
}

static int h_dialog_ok(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    (void) args;
    (void) argc;
    void (*done)(const char *) = g_import.prompt_done;
    g_import.prompt_done = NULL;

    const HtmlElem input = html_dom_by_id("knViewerDialogInput");
    const int length = input ? html_dom_get_value(input, NULL, 0) : 0;
    char *text = calloc((size_t) (length > 0 ? length : 0) + 1, 1);
    if (text && input && length > 0) html_dom_get_value(input, text, (size_t) length + 1);
    dialog_hide();
    if (done) done(text ? text : "");
    free(text);
    return 1;
}

static int h_dialog_cancel(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    (void) args;
    (void) argc;
    void (*done)(const char *) = g_import.prompt_done;
    g_import.prompt_done = NULL;
    dialog_hide();
    if (done) done(NULL); // prompt() returned null
    return 1;
}

void viewer_dialog_register_handlers(HtmlHandlerEntry *table, int *count) {
    table[(*count)++] = (HtmlHandlerEntry) {"knViewerDialogOk", h_dialog_ok};
    table[(*count)++] = (HtmlHandlerEntry) {"knViewerDialogCancel", h_dialog_cancel};
}

// ---------------------------------------------------------------- downloads

void viewer_download_png(const char *name, const unsigned char *rgba, const int w, const int h) {
#ifdef __EMSCRIPTEN__
    const char *temp = "/tmp/kn_viewer_download.png";
    if (!png_write_rgba(temp, rgba, w, h)) return;
    EM_ASM({
        var data = FS.readFile(UTF8ToString($0));
        var a = document.createElement('a');
        a.href = URL.createObjectURL(new Blob([data], {type: 'image/png'}));
        a.download = UTF8ToString($1);
        a.click();
    }, temp, name);
    KN_LOG_VERBOSE2("[viewer] download %s (%dx%d)\n", name, w, h);
#else
    char path[1024];
    const char *dir = getenv("XDG_DOWNLOAD_DIR");
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    int written = 0;
    if (dir && *dir) {
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        written = png_write_rgba(path, rgba, w, h);
    }
    if (!written && home && *home) {
        snprintf(path, sizeof(path), "%s/Downloads/%s", home, name);
        written = png_write_rgba(path, rgba, w, h);
    }
    if (!written) {
        snprintf(path, sizeof(path), "%s", name);
        written = png_write_rgba(path, rgba, w, h);
    }
    KN_LOG_VERBOSE2("[viewer] download %s: %s (%dx%d)\n", written ? "wrote" : "could not write", path, w, h);
#endif
}

// ---------------------------------------------------------------- import targets

static int ends_with(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

// vO204: the store item of a Load Custom target (store type: hat 1, head 15, body 2, back 14, waist 6,
// face 7, shoes 8, pet 9, dye 5, wrist) and its key: store.types[assetType || type] + id + (tex ? "_" + tex : "")
static int cosmetic_key(const int target, char *key, const size_t n) {
    const ViewerState *e = &g_viewer.e;
    const VgValue *value = target == ITEM_HAT ? &e->hat : target == ITEM_HEAD ? &e->head : target == ITEM_BODY ? &e->body :
                           target == ITEM_BACK ? &e->back : target == ITEM_WAIST ? &e->waist : target == ITEM_FACE ? &e->face :
                           target == ITEM_SHOE ? &e->shoe : target == ITEM_PET ? &e->pet : target == ITEM_DYE ? &e->dye :
                           target == ITEM_WRIST ? &e->wrist : NULL;
    const ViewerSkin *skin = value ? viewer_skin(value->num) : NULL;
    if (!skin) return 0;
    const int dir = skin->asset_type ? skin->asset_type : skin->type;
    if (dir < 0 || dir >= g_viewer_type_count) return 0;
    if (viewer_skin_tex_truthy(skin)) snprintf(key, n, "%s%d_%s", g_viewer_types[dir], skin->id, skin->tex);
    else snprintf(key, n, "%s%d", g_viewer_types[dir], skin->id);
    return 1;
}

// the texture key of the weapon in hand (Q's image branch, no cosmetic target)
static int weapon_texture_key(char *key, const size_t n) {
    const Weapon *weapon = vscene_player_weapon();
    if (!weapon) return 0;

    if (weapon->melee) {
        const ViewerSkin *skin = viewer_skin(g_viewer.e.melee.num);
        const int id = skin && skin->id > 0 ? skin->id : 0;
        if (skin && viewer_skin_tex_truthy(skin)) snprintf(key, n, "melee/melee_%d_%s", id, skin->tex);
        else snprintf(key, n, "melee/melee_%d", id);
        return 1;
    }

    const int slot = vscene_player_weapon_slot();
    const ViewerSkin *skin = viewer_skin(slot == 0 ? g_viewer.e.skin_prim.num : slot == 1 ? g_viewer.e.skin_sec.num : -1);
    const ViewerAltSkin *alt = skin ? NULL : viewer_weapon_alt_skin(vscene_player_weapon_id(), 0);
    if (alt) { // no skin: weapon.altSkin, a remodel: getReModel's texture (v7 viewer.js:1950-1956), the one the model wears
        snprintf(key, n, "%s%d_%d", g_viewer_types[0], alt->skin.weapon, alt->skin.mid_t > 0 ? alt->skin.mid_t : alt->skin.mid);
        return 1;
    }
    if (!skin) snprintf(key, n, "weapons/%s", weapon->src);
    else if (skin->mid >= 0) snprintf(key, n, "weapons/%s_%d", weapon->src, skin->mid);       // n.midT || ...: midT
    else if (viewer_skin_tex_truthy(skin)) snprintf(key, n, "%s", skin->tex);
    else if (skin->id >= 0) snprintf(key, n, "weapons/skins/%s_%d", weapon->src, skin->id);
    else snprintf(key, n, "weapons/%s", weapon->src);
    if (skin && skin->mid >= 0 && skin->mid_t >= 0) snprintf(key, n, "%d", skin->mid_t); // n.midT (a bare id in v3.1.1)
    return 1;
}

static void rebuild_now(void *user) {
    (void) user;
    g_import.rebuild_timer = 0;
    vscene_rebuild_player();
}

// ---- zip (client/src/zip_read.c)

static int write_temp(const char *name, const unsigned char *data, const size_t length, char *out, const size_t n) {
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
#ifdef _WIN32
    const char *dir = getenv("TEMP") ? getenv("TEMP") : ".";
#else
    const char *dir = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
#endif
    static int serial;
    snprintf(out, n, "%s/kn_viewer_%d_%s", dir, ++serial, base);
    FILE *file = fopen(out, "wb");
    if (!file) return 0;
    const int ok = fwrite(data, 1, length, file) == length;
    fclose(file);
    return ok;
}

static void import_zip_entry(const char *name, const unsigned char *data, const size_t length) {
    char key[256];
    const char *slash = strchr(name, '/');
    if (!slash) return;
    const size_t top = (size_t) (slash - name);

    if (top == 8 && !strncmp(name, "textures", 8) && ends_with(name, ".png")) {
        // ee: updateTexture(filename.replace("textures/", "").replace(".png", ""))
        snprintf(key, sizeof(key), "%.*s", (int) (strlen(slash + 1) - 4), slash + 1);
        int w, h, channels;
        unsigned char *rgba = stbi_load_from_memory(data, (int) length, &w, &h, &channels, 4);
        if (rgba) {
            vscene_update_texture_rgba(key, rgba, w, h);
            stbi_image_free(rgba);
        }
    } else if (top == 6 && !strncmp(name, "models", 6)) {
        // te: updateMesh(filename.replace(/models\/|\.obj|\.gltf|\.glb|\.fbx/gim, ""), data, ext)
        snprintf(key, sizeof(key), "%s", slash + 1);
        const char *dot = strrchr(slash + 1, '.');
        const char *ext = dot ? dot + 1 : "";
        static const char *const strip[] = {".obj", ".gltf", ".glb", ".fbx"};
        for (size_t i = 0; i < sizeof(strip) / sizeof(strip[0]); i++) {
            char *at;
            while ((at = strstr(key, strip[i]))) memmove(at, at + strlen(strip[i]), strlen(at + strlen(strip[i])) + 1);
        }
        char temp[512];
        if (write_temp(name, data, length, temp, sizeof(temp))) {
            if (!strcmp(ext, "gltf") || !strcmp(ext, "glb")) vscene_update_mesh_gltf(key, temp, ext);
            else if (!strcmp(ext, "obj")) vscene_update_mesh_obj(key, temp);
            else fprintf(stderr, "[viewer] %s: FBX models are not supported\n", name); // v7 FBXLoader
            remove(temp);
        }
    }
}

static int import_zip_cb(const char *name, const unsigned char *data, const size_t length, void *user) {
    (void) user;
    import_zip_entry(name, data, length);
    return 1;
}

static int import_zip(const unsigned char *zip, const size_t length) {
    return zip_read_entries(zip, length, import_zip_cb, NULL);
}

// ---- Q(): one file

static int read_whole(const char *path, unsigned char **data, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    *data = size > 0 ? malloc((size_t) size) : NULL;
    const int ok = *data && fread(*data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);
    if (!ok) {
        free(*data);
        *data = NULL;
        return 0;
    }
    *length = (size_t) size;
    return 1;
}

void viewer_import_path(const char *path, const int target) {
    unsigned char *data = NULL;
    size_t length = 0;
    if (!read_whole(path, &data, &length)) {
        fprintf(stderr, "[viewer] cannot read %s\n", path);
        return;
    }
    KN_LOG_VERBOSE2("[viewer] import %s (target %d)\n", path, target);

    char key[256];
    if (ends_with(path, ".zip")) {
        if (!import_zip(data, length)) viewer_alert("Issues loading mode zip");
        if (g_import.rebuild_timer) html_timer_cancel(g_import.rebuild_timer);
        g_import.rebuild_timer = html_timer_add(IMPORT_REBUILD_DELAY_MS, rebuild_now, NULL);
    } else if (ends_with(path, ".obj") || ends_with(path, ".gltf") || ends_with(path, ".glb") || ends_with(path, ".fbx")) {
        // updateMesh(<cosmetic key> or "weapons/<src>", data, ext), then f458()
        const char *ext = strrchr(path, '.') + 1;
        int have = 0;
        if (target) {
            have = cosmetic_key(target, key, sizeof(key));
        } else if (vscene_player_weapon()) {
            snprintf(key, sizeof(key), "weapons/%s", vscene_player_weapon()->src ? vscene_player_weapon()->src : "undefined");
            have = 1;
        }
        if (have) {
            if (!strcmp(ext, "obj")) vscene_update_mesh_obj(key, path);
            else if (!strcmp(ext, "fbx")) fprintf(stderr, "[viewer] %s: FBX models are not supported\n", path); // v7 FBXLoader
            else vscene_update_mesh_gltf(key, path, ext);
            vscene_rebuild_player();
        }
    } else {
        int w, h, channels;
        unsigned char *rgba = stbi_load_from_memory(data, (int) length, &w, &h, &channels, 4);
        if (rgba) {
            const int have = target ? cosmetic_key(target, key, sizeof(key)) : weapon_texture_key(key, sizeof(key));
            if (have) vscene_update_texture_rgba(key, rgba, w, h);
            stbi_image_free(rgba);
        }
    }
    free(data);
}

static void import_paths(const char **paths, const int count, const int max_files, const int target) {
    if (count <= 0 || count > max_files) {
        viewer_alert("Please select 1 file");
        return;
    }
    for (int i = 0; i < count; i++) viewer_import_path(paths[i], target);
}

// ---------------------------------------------------------------- file picking / dropping

#ifdef __EMSCRIPTEN__
// <input type=file> and window drop listeners write the files into MEMFS and queue
// "target|max|path\n" lines that viewer_import_tick() drains.
EM_JS(void, viewer_web_listen, (void), {
    if (Module.knViewerListening) return;
    Module.knViewerListening = true;
    Module.knViewerQueue = [];
    Module.knViewerStore = function(files, max, target) {
        if (!files || !files.length || files.length > max) { Module.knViewerQueue.push(target + '|' + max + '|'); return; }
        var left = files.length, paths = [];
        Array.prototype.forEach.call(files, function(file, i) {
            var reader = new FileReader();
            reader.onload = function() {
                try { FS.mkdir('/tmp'); } catch (e) {}
                var path = '/tmp/kn_viewer_' + Date.now() + '_' + i + '_' + file.name.replace(/[^A-Za-z0-9._-]/g, '_');
                FS.writeFile(path, new Uint8Array(reader.result));
                paths[i] = path;
                if (--left === 0) Module.knViewerQueue.push(target + '|' + max + '|' + paths.join('\t'));
            };
            reader.readAsArrayBuffer(file);
        });
    };
    var container = function() { return document.getElementById('container'); };
    window.addEventListener('dragover', function(e) { e.preventDefault(); var c = container(); if (c) c.style.boxShadow = '#666666 0px 0px 100px inset'; });
    window.addEventListener('dragleave', function(e) { e.preventDefault(); var c = container(); if (c) c.style.boxShadow = 'none'; });
    window.addEventListener('drop', function(e) {
        e.preventDefault();
        var c = container(); if (c) c.style.boxShadow = 'none';
        if (Module.knViewerActive) Module.knViewerStore(e.dataTransfer && e.dataTransfer.files, 1, 0);
    });
});

EM_JS(void, viewer_web_pick, (int max, int target), {
    var input = document.createElement('input');
    input.type = 'file';
    input.id = 'jsonInput';
    if (max > 1) input.multiple = true;
    input.addEventListener('change', function() { Module.knViewerStore(input.files, max, target); }, false);
    input.click();
});

EM_JS(void, viewer_web_set_active, (int active), { Module.knViewerActive = !!active; });

EM_JS(int, viewer_web_next, (char *buf, int n), {
    if (!Module.knViewerQueue || !Module.knViewerQueue.length) return 0;
    stringToUTF8(Module.knViewerQueue.shift(), buf, n);
    return 1;
});
#endif

void viewer_import_pick(const int max_files, const int target) {
#ifdef __EMSCRIPTEN__
    viewer_web_pick(max_files, target);
#else
    // a native file dialog when one is installed (zenity / kdialog); else drop files on the window
    const char *commands[] = {
        max_files > 1 ? "zenity --file-selection --multiple --separator='\n' 2>/dev/null" : "zenity --file-selection 2>/dev/null",
        max_files > 1 ? "kdialog --getopenfilename --multiple --separate-output . 2>/dev/null" : "kdialog --getopenfilename . 2>/dev/null",
    };
    for (size_t c = 0; c < sizeof(commands) / sizeof(commands[0]); c++) {
        FILE *pipe = popen(commands[c], "r");
        if (!pipe) continue;
        char line[1024];
        char *paths[IMPORT_MAX_FILES];
        int count = 0;
        while (fgets(line, sizeof(line), pipe) && count < IMPORT_MAX_FILES) {
            line[strcspn(line, "\r\n")] = 0;
            if (*line) paths[count++] = strdup(line);
        }
        const int status = pclose(pipe);
        if (count) {
            import_paths((const char **) paths, count, max_files, target);
            for (int i = 0; i < count; i++) free(paths[i]);
            return;
        }
        if (status == 0 || status == 256) return; // the dialog ran and was cancelled
    }
    fprintf(stderr, "[viewer] no file dialog (install zenity or kdialog); drop the file on the window instead\n");
#endif
}

#ifndef __EMSCRIPTEN__
static void drop_callback(GLFWwindow *window, const int count, const char **paths) {
    (void) window;
    import_paths(paths, count, 1, 0); // container "drop": Q(event)
}
#endif

void viewer_import_init(Client *client) {
    memset(&g_import, 0, sizeof(g_import));
    g_import.client = client;
#ifdef __EMSCRIPTEN__
    viewer_web_listen();
    viewer_web_set_active(1);
#else
    if (client && client->window) g_import.previous_drop = glfwSetDropCallback(client->window, drop_callback);
#endif
}

void viewer_import_fini(void) {
    if (g_import.rebuild_timer) html_timer_cancel(g_import.rebuild_timer);
#ifdef __EMSCRIPTEN__
    viewer_web_set_active(0);
#else
    if (g_import.client && g_import.client->window) glfwSetDropCallback(g_import.client->window, g_import.previous_drop);
#endif
    memset(&g_import, 0, sizeof(g_import));
}

void viewer_import_tick(void) {
#ifdef __EMSCRIPTEN__
    char line[4096];
    while (viewer_web_next(line, sizeof(line))) {
        char *first = strchr(line, '|');
        char *second = first ? strchr(first + 1, '|') : NULL;
        if (!second) continue;
        *first = 0;
        *second = 0;
        const int target = atoi(line), max = atoi(first + 1);
        const char *paths[IMPORT_MAX_FILES];
        int count = 0;
        for (char *p = second + 1; *p && count < IMPORT_MAX_FILES;) {
            char *tab = strchr(p, '\t');
            if (tab) *tab = 0;
            paths[count++] = p;
            if (!tab) break;
            p = tab + 1;
        }
        import_paths(paths, count, max, target);
        for (int i = 0; i < count; i++) remove(paths[i]);
    }
#endif
}
