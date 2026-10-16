// Profile pictures on the account page (client/include/avatar_ui.h). The picture never passes
// through here decoded: the file's bytes go to the brain as base64 JSON, and the brain's answer
// (202 / an error name, GET status) decides what the page says. The size and type checks here
// are only there to answer early; the brain enforces the same limits.
#include <avatar_ui.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include <backend.h>
#include <backend_ui.h> // account_session_token()
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>   // glfwGetTime(), as account_ui.c uses it

#define AVATAR_MAX_BYTES (512u * 1024u)
#define AVATAR_POLL_MS 200.0
#define AVATAR_PICK_WAIT_S 300.0
#define AVATAR_RECHECK_MS 4000
#define AVATAR_RECHECKS_MAX 30 // a pending review is asked about this many times (about 2 minutes)

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// An <input type=file> for pictures (png_only: PNG only). The file is read as a data URL (base64) in
// the browser; Module.knAvatarPick then holds the data URL, or "!size" / "!type" / "!read" / "!cancel".
EM_JS(void, kn_avatar_pick_open, (int png_only, double max_bytes), {
    var input = document.createElement("input");
    input.type = "file";
    input.accept = png_only ? "image/png" : "image/png,image/jpeg,image/webp";
    input.onchange = function () {
        var f = input.files && input.files[0];
        if (!f) { Module.knAvatarPick = "!cancel"; return; }
        if (f.size > max_bytes) { Module.knAvatarPick = "!size"; return; }
        if (png_only && f.type !== "image/png") { Module.knAvatarPick = "!type"; return; }
        var r = new FileReader();
        r.onload = function () { Module.knAvatarPick = String(r.result || "!read"); };
        r.onerror = function () { Module.knAvatarPick = "!read"; };
        r.readAsDataURL(f);
    };
    input.click();
});

EM_JS(char *, kn_avatar_pick_take, (void), {
    var p = Module.knAvatarPick;
    if (!p) return 0;
    Module.knAvatarPick = null;
    return stringToNewUTF8(p);
});
#endif

static double g_pick_until;
static int g_rechecks;   // checks of the picture under review since the last upload
static struct {          // the dialog in progress (one at a time)
    int png_only;
    size_t max_bytes;
    AvatarPickFn fn;
    void *user;
} g_pick;

// ---- base64 (the upload is JSON; no shared encoder in the client)

#ifndef __EMSCRIPTEN__ // the native dialog's upload (the web build hands over the data URL)
static char *b64_encode(const unsigned char *in, const size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc(4 * ((n + 2) / 3) + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = in[i], b = i + 1 < n ? in[i + 1] : 0, c = i + 2 < n ? in[i + 2] : 0;
        const uint32_t v = a << 16 | b << 8 | c;
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}
#endif

// ---- messages

static void set_status(const char *text) {
    HtmlElem el = html_dom_by_id("pfpStatus");
    if (!el) return;
    html_dom_set_text(el, text ? text : "");
}

// The brain's error name -> what the player is told (kn_brain: avatar.c).
static const char *error_message(const int status, const char *name) {
    if (status == 0) return "The server could not be reached. Try again later.";
    if (name && !strcmp(name, "UnsupportedType")) return "Pick a PNG, JPEG or WebP picture.";
    if (name && (!strcmp(name, "FileTooLarge") || !strcmp(name, "ImageTooLarge"))) return "That picture is too large (512 KB, 2048 px at most).";
    if (name && !strcmp(name, "ImageTooSmall")) return "That picture is too small (at least 32 x 32 px).";
    if (name && !strcmp(name, "BadImage")) return "That file could not be read as a picture.";
    if (name && !strcmp(name, "TooManyRequests")) return "You can upload 5 pictures an hour. Try again later.";
    if (name && (!strcmp(name, "LoginRequired") || status == 401)) return "Sign in to set a profile picture.";
    if (name && (!strcmp(name, "AvatarsUnavailable") || !strcmp(name, "ModerationUnavailable"))) return "Profile pictures are not available right now.";
    return "The picture could not be uploaded. Try again later.";
}

// ---- status (GET /api/account/avatar?token=...)

static void recheck(void *user);

static void on_status(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    if (status != 200 || !body || !length) return; // keep the last line; a failed refresh says nothing
    cJSON *o = cJSON_ParseWithLength(body, length);
    const cJSON *data = o ? cJSON_GetObjectItemCaseSensitive(o, "data") : NULL;
    const cJSON *st = data ? cJSON_GetObjectItemCaseSensitive(data, "status") : NULL;
    const cJSON *why = data ? cJSON_GetObjectItemCaseSensitive(data, "reason") : NULL;
    const char *s = cJSON_IsString(st) && st->valuestring ? st->valuestring : "none";
    char line[320];
    if (!strcmp(s, "pending")) snprintf(line, sizeof(line), "Your picture is being checked.");
    else if (!strcmp(s, "approved")) snprintf(line, sizeof(line), "Your picture is shown.");
    else if (!strcmp(s, "rejected") || !strcmp(s, "error")) {
        const char *r = cJSON_IsString(why) && why->valuestring ? why->valuestring : "";
        snprintf(line, sizeof(line), "Not shown: %s", r[0] ? r : "it did not pass review.");
    } else line[0] = 0;
    set_status(line);
    // still under review: look again (bounded, so a stuck review does not poll forever)
    if (!strcmp(s, "pending") && g_rechecks++ < AVATAR_RECHECKS_MAX) html_timer_add(AVATAR_RECHECK_MS, recheck, NULL);
    cJSON_Delete(o);
}

void avatar_ui_refresh(void) {
    if (!backend_enabled() || !account_session_token() || !account_session_token()[0]) return;
    char enc[512], path[640];
    backend_url_encode(enc, sizeof(enc), account_session_token());
    snprintf(path, sizeof(path), "/api/account/avatar?token=%s", enc);
    backend_http_get(path, on_status, NULL);
}

static void recheck(void *user) {
    (void) user;
    avatar_ui_refresh();
}

// ---- upload (POST /api/account/avatar {image: base64, token})

static void on_upload(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    cJSON *o = body && length ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *name = o ? cJSON_GetObjectItemCaseSensitive(o, "error") : NULL;
    if (status == 202) {
        g_rechecks = 0;
        set_status("Picture received. Checking it now.");
        html_timer_add(AVATAR_RECHECK_MS, recheck, NULL);
    } else {
        set_status(error_message(status, cJSON_IsString(name) ? name->valuestring : NULL));
    }
    cJSON_Delete(o);
}

// b64: the picture's bytes, base64, not owned (copied into the request body)
static void upload_base64(const char *b64) {
    const char *token = account_session_token();
    if (!backend_enabled() || !token || !token[0]) {
        set_status("Sign in to set a profile picture.");
        return;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "image", b64);
    cJSON_AddStringToObject(req, "token", token);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!json) return;
    const size_t n = strlen(backend_url()) + 32;
    char *url = malloc(n);
    if (url) {
        snprintf(url, n, "%s/api/account/avatar", backend_url());
        set_status("Uploading picture...");
        backend_http_request("POST", url, json, on_upload, NULL);
        free(url);
    }
    free(json);
}

// ---- the dialog: the picked file's checks (type, size), then its base64 to the caller

// the answer of a picked file: the caller's callback, once (the error name, or NULL = ok)
static void picked(const char *b64, const size_t bytes, const char *error) {
    AvatarPickFn fn = g_pick.fn;
    void *user = g_pick.user;
    g_pick.fn = NULL;
    if (fn) fn(user, b64, bytes, error);
}

#ifndef __EMSCRIPTEN__
// the picture's type by its first bytes: PNG only, or PNG / JPEG / WebP
static int picture_type_ok(const unsigned char *data, const size_t n, const int png_only) {
    const int png = n >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8);
    if (png_only) return png;
    const int jpeg = n >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
    const int webp = n >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4);
    return png || jpeg || webp;
}

static int read_picture(const char *path, const size_t max_bytes, unsigned char **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *buf = malloc(max_bytes + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    *n = fread(buf, 1, max_bytes + 1, f);
    fclose(f);
    *out = buf;
    return 1;
}

// zenity, then kdialog (the editor's dialogs); the chosen path, or NULL when cancelled / none
static int native_pick(const int png_only, char *out, const size_t cap) {
    static const char *commands[] = {
        "zenity --file-selection --title='Choose a picture' --file-filter='Pictures | *.png *.jpg *.jpeg *.webp' 2>/dev/null",
        "kdialog --getopenfilename . 'Pictures (*.png *.jpg *.jpeg *.webp)' 2>/dev/null",
    };
    static const char *png_commands[] = {
        "zenity --file-selection --title='Choose a PNG picture' --file-filter='PNG pictures | *.png' 2>/dev/null",
        "kdialog --getopenfilename . 'PNG pictures (*.png)' 2>/dev/null",
    };
    const char *const *list = png_only ? png_commands : commands;
    for (size_t c = 0; c < sizeof(commands) / sizeof(commands[0]); c++) {
        FILE *pipe = popen(list[c], "r");
        if (!pipe) continue;
        char line[1024] = "";
        const int got = fgets(line, sizeof(line), pipe) != NULL;
        const int status = pclose(pipe);
        line[strcspn(line, "\r\n")] = 0;
        if (got && line[0]) {
            snprintf(out, cap, "%s", line);
            return 1;
        }
        if (status == 0 || status == 256) return 0; // the dialog ran and was cancelled
    }
    return 0;
}

static void pick_native(void) {
    char path[1024];
    if (!native_pick(g_pick.png_only, path, sizeof(path))) {
        g_pick.fn = NULL; // cancelled: the callback is not called
        return;
    }
    unsigned char *data = NULL;
    size_t n = 0;
    if (!read_picture(path, g_pick.max_bytes, &data, &n)) {
        picked(NULL, 0, "BadImage");
        return;
    }
    if (n > g_pick.max_bytes) picked(NULL, 0, "FileTooLarge");
    else if (!picture_type_ok(data, n, g_pick.png_only)) picked(NULL, 0, "UnsupportedType");
    else {
        char *b64 = b64_encode(data, n);
        if (b64) picked(b64, n, NULL);
        else picked(NULL, 0, "BadImage");
        free(b64);
    }
    free(data);
}
#else
static void pick_poll(void *user) {
    (void) user;
    char *p = kn_avatar_pick_take();
    if (p) {
        if (!strcmp(p, "!size")) picked(NULL, 0, "FileTooLarge");
        else if (!strcmp(p, "!type")) picked(NULL, 0, "UnsupportedType");
        else if (!strcmp(p, "!read")) picked(NULL, 0, "BadImage");
        else if (strcmp(p, "!cancel") != 0) {
            // "data:<type>;base64,<...>": the browser's data URL, the base64 part goes up as is
            const char *comma = strchr(p, ',');
            if (comma && strstr(p, ";base64,")) {
                const char *b = comma + 1;
                const size_t len = strlen(b);
                size_t pad = 0;
                if (len >= 1 && b[len - 1] == '=') pad++;
                if (len >= 2 && b[len - 2] == '=') pad++;
                picked(b, len / 4 * 3 - pad, NULL);
            } else picked(NULL, 0, "BadImage");
        }
        free(p);
        return;
    }
    if (glfwGetTime() < g_pick_until) html_timer_add(AVATAR_POLL_MS, pick_poll, NULL);
}
#endif

void avatar_ui_pick_file(const int png_only, const size_t max_bytes, const AvatarPickFn fn, void *user) {
    g_pick.png_only = png_only;
    g_pick.max_bytes = max_bytes;
    g_pick.fn = fn;
    g_pick.user = user;
#ifdef __EMSCRIPTEN__
    kn_avatar_pick_open(png_only, (double) max_bytes);
    g_pick_until = glfwGetTime() + AVATAR_PICK_WAIT_S;
    html_timer_add(AVATAR_POLL_MS, pick_poll, NULL);
#else
    pick_native();
#endif
}

// "Change Picture": the profile picture (PNG, JPEG or WebP, up to 512 KB), uploaded when chosen
static void on_profile_picked(void *user, const char *b64, const size_t bytes, const char *error) {
    (void) user;
    (void) bytes;
    if (error) {
        const int status = !strcmp(error, "FileTooLarge") ? 413 : !strcmp(error, "UnsupportedType") ? 415 : 422;
        set_status(error_message(status, error));
        return;
    }
    upload_base64(b64);
}

void avatar_ui_pick(void) { avatar_ui_pick_file(0, AVATAR_MAX_BYTES, on_profile_picked, NULL); }
