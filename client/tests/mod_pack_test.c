// mod_pack_test: map resource packs (client/src/mod_pack.c over client/src/zip_read.c), GL-free.
//   - client/tests/data/mod_pack_test.zip (stored, deflated and repeated entries, every v7 entry
//     kind and the ones v7 does not apply, a "../" escape) loads from the asset tree's
//     user-assets/<path> for a user-assets.krunker.io URL;
//   - while its map is loaded the replaced files read as the pack's (file_redirect, read_file),
//     ahead of the resource swapper, and the loaded copies are refreshed (hooks);
//   - the same pack on the next map is kept as is (v7: v11759 != modURL);
//   - after a map change to a map without a pack the game's own files are back, the loaded
//     copies refreshed again and the extracted files deleted;
//   - URL resolution (old CDN names, Dropbox, "&", ".."), a corrupt pack, and the real
//     md23677/mod.zip of zombie_high (when assets/ has it).
//   mod_pack_test <repo root>        (POSIX: setenv, symlink)
#include <mod_pack.h>
#include <zip_read.h>
#include <shared.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(p) mkdir(p, 0755)

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- hooks: what the client's loaders would refresh
static char g_calls[4096];
static void note(const char *kind, const char *what) {
    const char *assets = assets_path();
    if (!strncmp(what, assets, strlen(assets))) what += strlen(assets);
    const size_t len = strlen(g_calls);
    snprintf(g_calls + len, sizeof(g_calls) - len, "%s:%s;", kind, what);
}
static int hook_texture(const char *path) { note("tex", path); return 1; }
static int hook_geometry(const char *path) { note("obj", path); return 1; }
static void hook_gltf(const char *path) { note("gltf", path); }
static void hook_sound(const char *name) { note("snd", name); }

// ---- the resource swapper stand-in: everything under assets/ to "swapped"
static int swap_all(const char *path, char *out, const size_t n) {
    if (strncmp(path, assets_path(), strlen(assets_path()))) return 0;
    snprintf(out, n, "swapped:%s", path + strlen(assets_path()));
    return 1;
}

static int write_file(const char *path, const void *data, const size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

static char *asset(const char *rel) {
    return concat(assets_path(), rel);
}

// the content read for an asset path (through the overlay), "" when unreadable
static void read_asset(const char *rel, char *out, const size_t n) {
    char *path = asset(rel);
    unsigned char *data = NULL;
    size_t size = 0;
    out[0] = 0;
    if (path && !read_file(path, &data, &size) && data) snprintf(out, n, "%.*s", (int) size, (const char *) data);
    free(data);
    free(path);
}

static int redirected(const char *rel, char *out, const size_t n) {
    char *path = asset(rel);
    const char *r = file_redirect(path, out, n);
    const int moved = r != path;
    if (!moved) snprintf(out, n, "%s", path);
    free(path);
    return moved;
}

static cJSON *map_with(const char *url) {
    cJSON *map = cJSON_CreateObject();
    cJSON_AddStringToObject(map, "name", "test");
    if (url) cJSON_AddStringToObject(map, "modURL", url);
    return map;
}

static void map_loaded(const char *url) {
    cJSON *map = map_with(url);
    mod_pack_map_loaded(map);
    cJSON_Delete(map);
}

static void check_urls(void) {
    char *p = mod_pack_resolve_url("https://user-assets.krunker.io/md23677/mod.zip");
    char *expect = asset("user-assets/md23677/mod.zip");
    CHECK(p && !strcmp(p, expect), "user-assets URL -> %s (got %s)", expect, p ? p : "NULL");
    free(p);
    p = mod_pack_resolve_url("http://krunker-user-assets.nyc3.cdn.digitaloceanspaces.com/md23677/mod.zip?v=2");
    CHECK(p && !strcmp(p, expect), "old CDN URL with a query -> %s (got %s)", expect, p ? p : "NULL");
    free(p);
    free(expect);
    CHECK(!mod_pack_resolve_url("https://www.dropbox.com/s/ejrn99ayvddiq45/Verb%20Mod.zip?dl=0"), "a Dropbox URL resolves");
    CHECK(!mod_pack_resolve_url("https://user-assets.krunker.io/md1/mod.zip&x=1"), "a URL with & resolves");
    CHECK(!mod_pack_resolve_url("https://user-assets.krunker.io/../../etc/passwd"), "a .. URL resolves");
    CHECK(!mod_pack_resolve_url("https://example.com/mod.zip"), "another host resolves");
    CHECK(!mod_pack_resolve_url(""), "an empty URL resolves");
}

static void check_test_pack(const char *dir) {
    static const char *const url = "https://user-assets.krunker.io/mdtest/mod.zip";
    char got[512], file_a[512];

    // the game's own files
    read_asset("textures/kntest/tex_a.png", got, sizeof(got));
    CHECK(!strcmp(got, "ORIG tex_a"), "the game's textures/kntest/tex_a.png (%s)", got);
    g_calls[0] = 0;
    map_loaded(url);
    CHECK(!strcmp(mod_pack_active_url(), url), "the pack is active (%s)", mod_pack_active_url());
    CHECK(mod_pack_file_count() == 6, "6 files replace the game's (%d)", mod_pack_file_count());

    // replaced files read as the pack's
    read_asset("textures/kntest/tex_a.png", got, sizeof(got));
    CHECK(!strcmp(got, "MOD tex_a second"), "textures/kntest/tex_a.png is the pack's last entry (%s)", got);
    read_asset("textures/kntest/tex_b.png", got, sizeof(got));
    CHECK(!strncmp(got, "MOD tex_b MOD tex_b", 19) && strlen(got) == 500, "a deflated entry inflates (%zu bytes)", strlen(got));
    read_asset("textures/kntest/tex_c.png", got, sizeof(got));
    CHECK(!strcmp(got, "MOD tex_c via img/"), "img/<key>.png replaces textures/<key>.png (%s)", got);
    read_asset("sound/kntest_snd.mp3", got, sizeof(got));
    CHECK(!strcmp(got, "MOD sound"), "sound/<name>.mp3 (%s)", got);
    read_asset("models/kntest/anim_a.glb", got, sizeof(got));
    CHECK(!strcmp(got, "glTF mod"), "models/<key>.glb (%s)", got);
    CHECK(redirected("models/kntest/model_a.obj", got, sizeof(got)), "models/<key>.obj");
    CHECK(redirected("textures/kntest/tex_a.png", file_a, sizeof(file_a)), "");
    // what v7 does not apply
    static const char *const kept[] = {"textures/logo.png", "textures/medals/medal_1.png", "img/medals/medal_1.png", "textures/kntest/readme.txt",
                                       "sound/sub/deeper.mp3", "models/kntest/model_a.mtl", "models/kntest/model_f.fbx", "models/kntest/event_0_x.obj",
                                       "weapons/weapon_1.obj", "models/weapons/weapon_1.obj", "scripts/settings.txt", "css/main.css", "evil.png",
                                       "../evil.png"};
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) CHECK(!redirected(kept[i], got, sizeof(got)), "%s is replaced (%s)", kept[i], got);
    char evil[512];
    snprintf(evil, sizeof(evil), "%s/../evil.png", dir);
    FILE *escaped = fopen(evil, "rb");
    CHECK(!escaped, "the ../ entry was written outside the pack");
    if (escaped) fclose(escaped);
    // only paths under assets_path() move
    const char *outside = "textures/kntest/tex_a.png";
    CHECK(file_redirect(outside, got, sizeof(got)) == outside, "a path outside the asset tree moves");
    // the loaded copies follow
    CHECK(strstr(g_calls, "tex:textures/kntest/tex_a.png;") && strstr(g_calls, "tex:textures/kntest/tex_b.png;") &&
              strstr(g_calls, "tex:textures/kntest/tex_c.png;") && strstr(g_calls, "obj:models/kntest/model_a.obj;") &&
              strstr(g_calls, "gltf:models/kntest/anim_a.glb;") && strstr(g_calls, "snd:kntest_snd;"),
          "apply refreshes every replaced file (%s)", g_calls);

    // the pack is ahead of the resource swapper; the swapper still has everything else
    file_set_redirect(swap_all);
    CHECK(redirected("textures/kntest/tex_a.png", got, sizeof(got)) && !strcmp(got, file_a), "the pack wins over the swapper (%s)", got);
    CHECK(redirected("textures/logo.png", got, sizeof(got)) && !strcmp(got, "swapped:textures/logo.png"), "the swapper keeps the rest (%s)", got);
    file_set_redirect(NULL);

    // the next map has the same pack: nothing reloads
    g_calls[0] = 0;
    map_loaded(url);
    CHECK(!g_calls[0] && !strcmp(mod_pack_active_url(), url), "the same pack again reloads (%s)", g_calls);

    // a map change: the game's own files are back
    g_calls[0] = 0;
    map_loaded(NULL);
    CHECK(!mod_pack_active_url()[0] && mod_pack_file_count() == 0, "the pack is gone after the map change");
    read_asset("textures/kntest/tex_a.png", got, sizeof(got));
    CHECK(!strcmp(got, "ORIG tex_a"), "textures/kntest/tex_a.png is the game's again (%s)", got);
    read_asset("textures/kntest/tex_c.png", got, sizeof(got));
    CHECK(!got[0], "textures/kntest/tex_c.png (not a game file) reads nothing again (%s)", got);
    CHECK(!redirected("textures/kntest/tex_a.png", got, sizeof(got)), "still redirected after the map change (%s)", got);
    CHECK(strstr(g_calls, "tex:textures/kntest/tex_a.png;") && strstr(g_calls, "obj:models/kntest/model_a.obj;") &&
              strstr(g_calls, "gltf:models/kntest/anim_a.glb;") && strstr(g_calls, "snd:kntest_snd;"),
          "the map change refreshes every replaced file (%s)", g_calls);
    FILE *left = fopen(file_a, "rb");
    CHECK(!left, "the extracted file %s is still there", file_a);
    if (left) fclose(left);

    // a map with a pack that cannot be used: nothing applies
    map_loaded(url);
    CHECK(mod_pack_file_count() == 6, "the pack loads again (%d)", mod_pack_file_count());
    map_loaded("https://www.dropbox.com/s/0h3mnvii1nua0h9/Logo.zip?dl=0");
    CHECK(!mod_pack_active_url()[0] && !redirected("textures/kntest/tex_a.png", got, sizeof(got)),
          "a map with a Dropbox pack (not loadable here) still has the previous map's pack");
    map_loaded("https://user-assets.krunker.io/mdbad/mod.zip");
    CHECK(!mod_pack_active_url()[0], "a corrupt pack is active");
}

static void check_real_pack(void) {
    char *zip = asset("user-assets/md23677/mod.zip");
    FILE *f = zip ? fopen(zip, "rb") : NULL;
    free(zip);
    if (!f) {
        printf("skip: user-assets/md23677/mod.zip (make assets-import-live)\n");
        return;
    }
    fclose(f);
    char got[512];
    map_loaded("https://user-assets.krunker.io/md23677/mod.zip");
    CHECK(mod_pack_file_count() == 27, "zombie_high's pack: 27 files (%d)", mod_pack_file_count());
    CHECK(redirected("textures/weapons/weapon_1.png", got, sizeof(got)) && redirected("models/melee/melee_0.obj", got, sizeof(got)) &&
              redirected("sound/weapon_2.mp3", got, sizeof(got)) && redirected("textures/sky_6.png", got, sizeof(got)),
          "zombie_high's pack replaces weapon_1.png, melee_0.obj, weapon_2.mp3, sky_6.png");
    CHECK(!redirected("textures/logo.png", got, sizeof(got)) && !redirected("weapons/weapon_1.obj", got, sizeof(got)),
          "zombie_high's pack: logo.png and weapons/*.obj are not applied");
    map_loaded("");
    CHECK(!redirected("textures/weapons/weapon_1.png", got, sizeof(got)), "zombie_high's pack is gone after the map change");
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    // a private asset tree: KRUNKNATIVE_ASSETS_DIR (read once by assets_path), TMPDIR for the packs
    const char *tmp = getenv("TMPDIR");
    char base[512], dir[600], path[700];
    snprintf(base, sizeof(base), "%s/kn_mod_pack_test_%d", tmp && *tmp ? tmp : "/tmp", (int) getpid());
    mkdir_p(base);
    snprintf(dir, sizeof(dir), "%s/assets", base);
    mkdir_p(dir);
    snprintf(path, sizeof(path), "%s/user-assets", dir), mkdir_p(path);
    snprintf(path, sizeof(path), "%s/user-assets/mdtest", dir), mkdir_p(path);
    snprintf(path, sizeof(path), "%s/user-assets/mdbad", dir), mkdir_p(path);
    snprintf(path, sizeof(path), "%s/textures", dir), mkdir_p(path);
    snprintf(path, sizeof(path), "%s/textures/kntest", dir), mkdir_p(path);
    snprintf(path, sizeof(path), "%s/textures/kntest/tex_a.png", dir);
    write_file(path, "ORIG tex_a", 10);
    snprintf(path, sizeof(path), "%s/user-assets/mdbad/mod.zip", dir);
    write_file(path, "this is not a zip archive at all, not even close", 48);

    char source[700];
    snprintf(source, sizeof(source), "%s/client/tests/data/mod_pack_test.zip", root);
    unsigned char *zip = NULL;
    size_t size = 0;
    if (read_file(source, &zip, &size) || !zip) {
        printf("FAIL: cannot read %s\n", source);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/user-assets/mdtest/mod.zip", dir);
    write_file(path, zip, size);
    free(zip);

    // the real assets for check_real_pack: link md23677 in
    char real[700], root_abs[4096];
    snprintf(real, sizeof(real), "%s/assets/user-assets/md23677", realpath(root, root_abs) ? root_abs : root);
    snprintf(path, sizeof(path), "%s/user-assets/md23677", dir);
    if (symlink(real, path) != 0) printf("note: no md23677 link\n");
    setenv("KRUNKNATIVE_ASSETS_DIR", dir, 1);
    char packs[600];
    snprintf(packs, sizeof(packs), "%s/packs", base);
    setenv("TMPDIR", packs, 1);
    const ModPackHooks hooks = {hook_texture, hook_geometry, hook_gltf, hook_sound};
    mod_pack_set_hooks(&hooks);

    check_urls();
    check_test_pack(packs);
    check_real_pack();
    mod_pack_shutdown();

    // the private tree goes (the pack directory must be empty by now)
    snprintf(path, sizeof(path), "%s/packs", base);
    CHECK(rmdir(path) == 0, "%s is not empty (extracted files left behind)", path);
    snprintf(path, sizeof(path), "%s/user-assets/md23677", dir), remove(path);
    snprintf(path, sizeof(path), "%s/user-assets/mdtest/mod.zip", dir), remove(path);
    snprintf(path, sizeof(path), "%s/user-assets/mdbad/mod.zip", dir), remove(path);
    snprintf(path, sizeof(path), "%s/textures/kntest/tex_a.png", dir), remove(path);
    static const char *const dirs[] = {"user-assets/mdtest", "user-assets/mdbad", "user-assets", "textures/kntest", "textures", ""};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, dirs[i]);
        rmdir(path);
    }
    rmdir(base);
    printf("mod_pack_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
