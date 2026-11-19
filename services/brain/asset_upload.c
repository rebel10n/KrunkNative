/*
 * services/brain/asset_upload.c - the map editor's Publish Asset on a Box Office server (asset_upload.h).
 *
 * Krunker's editor (editor-10.0.7.js:20576-20806; the limits in its config chunk, editor-config-10.0.7.js:9197-9215):
 *   1. KE.uploadAsset sends ["uploadA", name, "", category, private, data, sizes, mType, typeCode, uvc] on the social
 *      socket: sizes is {field: bytes} of the picked files (thumb, diffuse, emissive, normal, specular, alpha, bump,
 *      displacement, roughness, light, metalness, model, icon for a model; thumb + data for an object prefab; thumb +
 *      texture / sound / video with typeCode 2 / 3 / 4 and category 16 / 17 / 16), mType the model's extension.
 *   2. The server answers ["pubR", {fileId, srv, funds}] (KE.publishAssetResp) or ["upMp", reason] (uploadResp).
 *   3. The editor POSTs a FormData of fileId and the files to https://<srv>/upload-user-asset: "Asset Uploaded!" on
 *      200, else the answer's text in the popup ("Upload Request Failed" when the request itself failed).
 *
 * Here:
 *   uploadA checks the request against the editor's own limits (thumbnail 250 kB, 500 kB premium; icon 100 kB; each
 *   texture 10 MB; sound 5 MB; video 20 MB; a model's files together 100 MB, 250 MB premium; prefab data 500k
 *   characters), the account's storage quota and upload rate, and opens an upload ticket: fileId is 128 random bits,
 *   the ticket's only key, bound to the account and to the declared files and sizes. ["pubR", {fileId, srv}]: srv is
 *   uploads.public_host, or null (the client then posts to the brain it is talking to).
 *
 *   Deliberate difference: the files do not come as one multipart form. kn_brain takes request bodies up to 1 MiB
 *   (brain.max_body_bytes) and the proxies in front of it 16 MiB (deploy/ansible/roles/openresty), far below a 250 MB
 *   model, so the client sends each file in order as raw chunks of at most 512 KiB:
 *     POST /upload-user-asset?fileId=<id>&key=<field>&offset=<bytes already sent>   (body: the next bytes)
 *       -> 200 {"got": n}; a chunk already received (a re-send) answers 200 too
 *     POST /upload-user-asset?fileId=<id>&done=1
 *       -> 200 {"id": asset id} once every declared file arrived whole and passed its check; 4xx with the reason as text
 *   Each file is checked before anything is stored (never executed or decoded into memory beyond its structure):
 *   PNG / JPEG structure (PNG chunk CRCs, IHDR first, IEND last), MP3 (an ID3 tag or an MPEG audio frame), MP4 (an ftyp
 *   box), OBJ (text with vertices), glTF (JSON with asset.version, only data: URIs), GLB (header, length, a JSON chunk
 *   as for glTF), FBX (binary or ASCII header), the prefab's data.json (a JSON object or array). Never an HTML page.
 *
 *   Krunker's 10.0.7 fix "a failed custom asset upload not being retryable" (versions.txt "UPDATE 10.0.7"; the editor's
 *   upload code is unchanged since 10.0.5, so it is the server's): nothing about an asset changes until the whole upload
 *   is checked and committed, a failed check leaves the ticket open (the files can be sent again), and a new uploadA of
 *   the same account replaces its open ticket, so Publish/Update after any failure starts over without reopening the
 *   editor. A ticket expires uploads.ticket_s (1800) after its last chunk.
 *
 *   The asset: published_assets (migration 0377), ids from USER_ASSET_KN_ID_BASE. A publish of a name the account
 *   already has for the same kind of asset updates it (the popup is "Publish or Update Asset"): same id, version + 1,
 *   its files replaced. data holds the asset id's letters (MapManager.addAsset: the id string + data's keys; the
 *   client's user_asset_parse): t thumbnail; g / q / f a glTF / GLB / FBX model; e n s a m r l p b its companion maps
 *   (applyCompanionTextures, deob-10.0.7.js:159176: p is the displacement map, file model_d.png); d prefab; x texture
 *   (x + v a video); u sound. The files go to uploads.dir/<id>/ under Krunker's names (user_asset_path.h), which
 *   GET /api/user-assets/<id>/<file> answers (user_assets.c); a staging copy lives in uploads.dir/.staging/<fileId>/.
 *
 * Mods (the menu's Upload Mod, windows[18] "mods_pub", deob-10.0.7.js:220943-221050; the api at :27313-27358):
 *   1. submit() checks the name (required), the zip (25 MB, 50 MB premium; MB = 1e6 bytes) and that a thumbnail
 *      needs level 10, then api.mods.publishKey({name, assetSizes: {mod, thumb}}) = POST /mods/publish/key
 *      -> {data: {fileId, uploadUrl}} or {error: {code}} with a MODS_PUBLISH_* code (errors.mods.*, :223312-223324).
 *   2. api.mods.publishUpload(uploadUrl, fileId, zip, thumb) POSTs a FormData {fileId, mod, thumb} to uploadUrl
 *      -> {data: {mod_id}}, or a MODS_UPLOAD_* code; "Mod Published!", the Mod Manager's My Mods tab.
 *   Here publishKey runs the same checks server side (level 5 to publish: errors.mods.publishLevelTooLow; level 10
 *   for a thumbnail; the sizes; a name of another account's mod: MODS_UPLOAD_NAME_TAKEN; a flagged hacker; one key
 *   per uploads.mod_cooldown_ms) and opens a ticket of kind K_MOD with the files "mod" (mod.zip) and "thumb" (thumb.png).
 *   uploadUrl is <uploads.public_host>/mods/publish, or the path /mods/publish (the client puts the brain's URL in
 *   front); the files come as chunks exactly as the assets' (the same deliberate difference), POST
 *   /mods/publish?fileId=&key=&offset= and ?fileId=&done=1 -> {data: {mod_id}}; every refusal on that route is
 *   {error: {code, message}}. The zip is checked by its central directory (check_zip). The mod is a published_mods
 *   row with mod_url "ua" (Krunker's own value: the client loads userAssetsUrl("/md" + mod_id + "/mod.zip"),
 *   :217728, :223453, :226188) and mod_info {"t": 1} with a thumbnail (/md<id>/thumb.png); the files go to
 *   uploads.dir/md<id>/. A publish of a name the account already has updates that mod (mod_updatecounter + 1).
 *
 * Tickets survive a restart: each one is written to uploads.dir/.staging/<fileId>.json when it opens (the account,
 *   the name, the declared files); init reads them back with each file's bytes so far taken from its staged size,
 *   so the client's next chunk (a retry of the one the restart cut off) carries on. A chunk overlapping what is
 *   already staged is written again whole (the same bytes), which also covers a chunk half written at the crash.
 *   Tickets idle for uploads.ticket_s are dropped then, as in a running brain.
 */
#include "asset_upload.h"

#include "accounts.h"
#include "acct_internal.h"
#include "brain.h"
#include "db_procs.h"
#include "v10_api.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "user_asset_path.h"

#define MOD "asset-upload"
#define MAX_TICKETS 64
#define MAX_PARTS 14
#define CHUNK_MAX (512u * 1024u)
#define NAME_MAX_LEN 128 /* published_assets.name (mysql VARCHAR(128)); the popup allows 500 characters */
#define MB 1000000ll     /* the editor measures MB as size / 1000000 (updateFile, editor-10.0.7.js:13942) */
#define KB 1000ll
#define MOD_LEVEL 5              /* MODS_PUBLISH_LEVEL_TOO_LOW below it (errors.mods.publishLevelTooLow names level 5) */
#define MOD_THUMB_LEVEL 10       /* mods_pub: (level || 0) >= 10 for a thumbnail (deob-10.0.7.js:220954, :220997) */
#define ZIP_UNPACKED_MAX (1024ll * MB) /* a mod's entries unpacked together (a zip bomb is refused) */
#define ZIP_ENTRIES_MAX 20000

enum { K_MODEL, K_PREFAB, K_TEXTURE, K_SOUND, K_VIDEO, K_MOD };
enum { V_IMAGE, V_SOUND, V_VIDEO, V_MODEL, V_DATA, V_ZIP };

/* The popup's file fields: FormData key, stored file name (model: model.<mType>), asset id letter (0 = none), check,
 * limit in bytes (0 = only the model total; -1 = the thumbnail's free / premium limit) */
static const struct Field {
    const char *key, *file;
    char letter;
    int check;
    long long limit;
} k_fields[] = {
    {"thumb", "thumb.png", 't', V_IMAGE, -1},            /* aT 250 / aS 500 kB (premiumT), editor-config-10.0.7.js:9199-9200 */
    {"texture", "texture.png", 0, V_IMAGE, 10 * MB},     /* aX 10 MB */
    {"sound", "sound.mp3", 0, V_SOUND, 5 * MB},          /* aV 5 MB */
    {"video", "video.mp4", 0, V_VIDEO, 20 * MB},         /* aW 20 MB */
    {"data", "data.json", 0, V_DATA, 2 * MB},            /* the prefab: 500000 characters (custom.map.upload.limit) */
    {"diffuse", "model.png", 0, V_IMAGE, 10 * MB},       /* aY .. b5: 10 MB each */
    {"emissive", "model_e.png", 'e', V_IMAGE, 10 * MB},
    {"normal", "model_n.png", 'n', V_IMAGE, 10 * MB},
    {"specular", "model_s.png", 's', V_IMAGE, 10 * MB},
    {"alpha", "model_a.png", 'a', V_IMAGE, 10 * MB},
    {"displacement", "model_d.png", 'p', V_IMAGE, 10 * MB},
    {"roughness", "model_r.png", 'r', V_IMAGE, 10 * MB},
    {"metalness", "model_m.png", 'm', V_IMAGE, 10 * MB},
    {"bump", "model_b.png", 'b', V_IMAGE, 10 * MB},
    {"light", "model_l.png", 'l', V_IMAGE, 10 * MB},
    {"model", NULL, 0, V_MODEL, 0},
    {"icon", "icon.png", 0, V_IMAGE, 100 * KB},          /* b6 100 kB */
    {"mod", "mod.zip", 0, V_ZIP, 0},                     /* mods_pub: 25 / 50 MB (premiumT), deob-10.0.7.js:220956 */
};
#define FIELD_COUNT ((int) (sizeof(k_fields) / sizeof(k_fields[0])))

/* the fields each kind may send (indices into k_fields) and the one it must */
static int field_allowed(const int kind, const int f) {
    const char *k = k_fields[f].key;
    if (!strcmp(k, "thumb")) return 1;
    switch (kind) {
        case K_MODEL: return strcmp(k, "texture") && strcmp(k, "sound") && strcmp(k, "video") && strcmp(k, "data") && strcmp(k, "mod");
        case K_PREFAB: return !strcmp(k, "data");
        case K_TEXTURE: return !strcmp(k, "texture");
        case K_SOUND: return !strcmp(k, "sound");
        case K_VIDEO: return !strcmp(k, "video");
        default: return !strcmp(k, "mod");
    }
}

static const char *const k_required[] = {"model", "data", "texture", "sound", "video", "mod"};

typedef struct Part {
    int field;
    long long want, got;
} Part;

typedef struct Ticket {
    int used;
    char id[40];
    int64_t account;
    char username[40];
    char name[NAME_MAX_LEN * 4 + 1];
    int category, kind, priv;
    char ext[8];       /* the model's extension (obj, gltf, glb, fbx) */
    char uvc[160];     /* {letter: channel} JSON, "" = none */
    Part parts[MAX_PARTS];
    int nparts;
    uint64_t touched;  /* kn_now_ms of the last activity */
    int committing;    /* the checks or the database run: chunks and a second done are refused */
} Ticket;

static struct {
    int enabled;
    char dir[1024];      /* uploads.dir with a trailing slash */
    char staging[1100];  /* <dir>.staging/ */
    char host[256];      /* uploads.public_host: pubR's srv ("" = null) */
    int64_t ticket_ms;
    long long quota;     /* uploads.account_quota_mb: an account's stored assets */
    int per_hour;        /* uploads.per_hour: uploadA tickets an account may open per hour */
    int max_assets;      /* uploads.max_assets: an account's assets (and, apart, its mods) */
    int64_t mod_cooldown_ms; /* uploads.mod_cooldown_ms (10000): the MODS_PUBLISH_COOLDOWN window between two mod keys */
} C;

static Ticket T[MAX_TICKETS];
static Brain *g_b;
static atomic_int g_live;

/* per-account hourly ticket count (a small ring: the accounts that opened a ticket recently) */
static struct {
    int64_t account;
    uint64_t window;
    int n;
    uint64_t last; /* kn_now_ms of the account's last mod key (the mods' cooldown) */
} R[128];

/* ------------------------------------------------------------------ files */

static int mkdirs(const char *dir) {
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}

static void rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1400];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode)) rm_tree(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static const char *part_file(const Ticket *t, const Part *p, char *buf, const size_t cap) {
    if (k_fields[p->field].file) return k_fields[p->field].file;
    snprintf(buf, cap, "model.%s", t->ext);
    return buf;
}

static void staging_dir(const Ticket *t, char *out, const size_t cap) { snprintf(out, cap, "%s%s", C.staging, t->id); }
static void ticket_file(const char *id, char *out, const size_t cap) { snprintf(out, cap, "%s%s.json", C.staging, id); }

static void ticket_free(Ticket *t) {
    if (!t->used) return;
    char d[1200];
    staging_dir(t, d, sizeof d);
    rm_tree(d);
    ticket_file(t->id, d, sizeof d);
    unlink(d);
    memset(t, 0, sizeof *t);
}

/* the ticket's record beside its staging directory (read back by tickets_restore after a restart) */
static int ticket_save(const Ticket *t) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "account", (double) t->account);
    cJSON_AddStringToObject(o, "username", t->username);
    cJSON_AddStringToObject(o, "name", t->name);
    cJSON_AddNumberToObject(o, "category", t->category);
    cJSON_AddNumberToObject(o, "kind", t->kind);
    cJSON_AddNumberToObject(o, "priv", t->priv);
    cJSON_AddStringToObject(o, "ext", t->ext);
    cJSON_AddStringToObject(o, "uvc", t->uvc);
    cJSON *parts = cJSON_AddArrayToObject(o, "parts");
    for (int i = 0; i < t->nparts; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "key", k_fields[t->parts[i].field].key);
        cJSON_AddNumberToObject(p, "want", (double) t->parts[i].want);
        cJSON_AddItemToArray(parts, p);
    }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char path[1200], tmp[1300];
    ticket_file(t->id, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = txt ? fopen(tmp, "wb") : NULL;
    int ok = f && fputs(txt, f) >= 0;
    if (f) ok &= fclose(f) == 0;
    free(txt);
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}

static void sweep(const uint64_t now) {
    for (int i = 0; i < MAX_TICKETS; i++)
        if (T[i].used && !T[i].committing && (int64_t) (now - T[i].touched) > C.ticket_ms) {
            KN_INFO(MOD, "ticket of account %lld expired", (long long) T[i].account);
            ticket_free(&T[i]);
        }
}

static Ticket *ticket_by_id(const char *id) {
    if (!id || strlen(id) != 32) return NULL;
    for (int i = 0; i < MAX_TICKETS; i++)
        if (T[i].used && kn_ct_equal(T[i].id, id, 32)) return &T[i];
    return NULL;
}

/* ------------------------------------------------------------------ content checks (worker thread) */

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_of(const unsigned char *p, const size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static uint32_t be32(const unsigned char *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

/* PNG: signature, IHDR first (1..16384 square pixels at most), every chunk's CRC, IEND last; or a JPEG (SOI .. EOI) */
static const char *check_image(const unsigned char *d, const size_t n) {
    if (n >= 4 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return d[n - 2] == 0xff && d[n - 1] == 0xd9 ? NULL : "Invalid Image";
    if (n < 8 + 25 + 12 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "Invalid Image (PNG only)";
    size_t at = 8;
    int first = 1, end = 0;
    while (at + 12 <= n) {
        const uint32_t len = be32(d + at);
        if (len > n - at - 12) return "Invalid Image";
        const unsigned char *type = d + at + 4;
        if (crc32_of(type, (size_t) len + 4) != be32(d + at + 8 + len)) return "Invalid Image";
        if (first) {
            if (memcmp(type, "IHDR", 4) || len != 13) return "Invalid Image";
            const uint32_t w = be32(type + 4), h = be32(type + 8);
            if (!w || !h || w > 16384 || h > 16384) return "Invalid Image Size";
            first = 0;
        }
        at += 12 + (size_t) len;
        if (!memcmp(type, "IEND", 4)) {
            end = 1;
            break;
        }
    }
    return end ? NULL : "Invalid Image";
}

/* an ID3v2 tag (then an MPEG frame), or an MPEG audio frame header at the start */
static const char *check_sound(const unsigned char *d, const size_t n) {
    size_t at = 0;
    if (n >= 10 && !memcmp(d, "ID3", 3)) {
        const size_t tag = (size_t) (d[6] & 0x7f) << 21 | (size_t) (d[7] & 0x7f) << 14 | (size_t) (d[8] & 0x7f) << 7 | (d[9] & 0x7f);
        at = 10 + tag + (d[5] & 0x10 ? 10 : 0);
    }
    while (at < n && at < 4096 && d[at] == 0) at++; /* padding after the tag */
    if (at + 4 > n) return "Invalid Sound (MP3 only)";
    const int sync = d[at] == 0xff && (d[at + 1] & 0xe0) == 0xe0, layer = (d[at + 1] >> 1) & 3, rate = (d[at + 2] >> 4) & 15;
    return sync && layer && rate != 15 ? NULL : "Invalid Sound (MP3 only)";
}

static const char *check_video(const unsigned char *d, const size_t n) {
    if (n < 16 || memcmp(d + 4, "ftyp", 4)) return "Invalid Video (MP4 only)";
    const uint32_t box = be32(d);
    return box >= 8 && box <= n ? NULL : "Invalid Video (MP4 only)";
}

/* a glTF's JSON: asset.version, and only embedded (data:) buffers and images: the editor uploads one model file */
static const char *check_gltf_json(const char *json, const size_t n) {
    cJSON *g = cJSON_ParseWithLength(json, n);
    if (!cJSON_IsObject(g)) {
        cJSON_Delete(g);
        return "Invalid Model";
    }
    const char *err = NULL;
    const cJSON *asset = cJSON_GetObjectItemCaseSensitive(g, "asset");
    if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(asset, "version"))) err = "Invalid Model";
    static const char *const lists[] = {"buffers", "images"};
    for (size_t l = 0; !err && l < 2; l++) {
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(g, lists[l])) {
            const cJSON *uri = cJSON_GetObjectItemCaseSensitive(it, "uri");
            if (uri && (!cJSON_IsString(uri) || strncmp(uri->valuestring, "data:", 5))) err = "Model must be one file (embedded buffers and images)";
        }
    }
    cJSON_Delete(g);
    return err;
}

static const char *check_model(const char *ext, const unsigned char *d, const size_t n) {
    if (!strcmp(ext, "glb")) {
        if (n < 20 || memcmp(d, "glTF", 4) || le32(d + 4) != 2 || le32(d + 8) != n) return "Invalid Model";
        const uint32_t len = le32(d + 12);
        if (len > n - 20 || memcmp(d + 16, "JSON", 4)) return "Invalid Model";
        /* the BIN chunk's buffer is embedded: a uri-less buffers[0]; check_gltf_json allows that */
        return check_gltf_json((const char *) d + 20, len);
    }
    if (!strcmp(ext, "gltf")) return check_gltf_json((const char *) d, n);
    if (!strcmp(ext, "fbx")) {
        if (n >= 23 && !memcmp(d, "Kaydara FBX Binary  \0", 21)) return NULL;
        const size_t look = n < 4096 ? n : 4096;
        for (size_t i = 0; i + 18 <= look; i++)
            if (!memcmp(d + i, "FBXHeaderExtension", 18)) return NULL;
        return "Invalid Model";
    }
    /* OBJ: text (no NUL bytes) with at least one vertex */
    if (memchr(d, 0, n)) return "Invalid Model";
    int vertex = 0;
    for (size_t i = 0; i + 2 < n && !vertex; i++)
        if ((i == 0 || d[i - 1] == '\n') && d[i] == 'v' && (d[i + 1] == ' ' || d[i + 1] == '\t')) vertex = 1;
    return vertex ? NULL : "Invalid Model";
}

static const char *check_data(const unsigned char *d, const size_t n) {
    cJSON *j = cJSON_ParseWithLength((const char *) d, n);
    const int ok = cJSON_IsObject(j) || cJSON_IsArray(j);
    cJSON_Delete(j);
    return ok ? NULL : "Invalid Prefab Data";
}

static uint16_t le16(const unsigned char *p) { return (uint16_t) (p[0] | p[1] << 8); }

/* a mod pack: a zip whose end record and central directory hold together; every entry stored or deflated
 * (JSZip, which loadModPack reads it with, deob-10.0.7.js:225863), with a relative name (no "..", no "\\", no
 * drive or leading "/"), its local header where the directory says, the entries unpacked 1 GB at most, and at least
 * one file in a folder loadModPack reads (textures/, img/, sound/, models/, scripts/, shaders/, css/, css-img/,
 * :225880-225955). Nothing is unpacked here: the client unpacks it (mod_pack.c), checking each name again. */
static const char *check_zip(const unsigned char *d, const size_t n) {
    static const char *const bad = "Invalid Mod (.zip)";
    if (n < 22 || memcmp(d, "PK\3\4", 4)) return bad;
    size_t eocd = 0;
    const size_t floor = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t i = n - 22 + 1; i-- > floor;)
        if (!memcmp(d + i, "PK\5\6", 4)) {
            eocd = i + 1;
            break;
        }
    if (!eocd--) return bad;
    const unsigned entries = le16(d + eocd + 10);
    const uint32_t cd_size = le32(d + eocd + 12), cd_at = le32(d + eocd + 16);
    if (!entries || entries > ZIP_ENTRIES_MAX || le16(d + eocd + 8) != entries || cd_at > eocd || cd_size > eocd - cd_at) return bad;
    static const char *const dirs[] = {"textures/", "img/", "sound/", "models/", "scripts/", "shaders/", "css/", "css-img/"};
    size_t at = cd_at;
    long long unpacked = 0;
    int known = 0;
    for (unsigned e = 0; e < entries; e++) {
        if (at + 46 > cd_at + cd_size || memcmp(d + at, "PK\1\2", 4)) return bad;
        const unsigned method = le16(d + at + 10), nlen = le16(d + at + 28), xlen = le16(d + at + 30), clen = le16(d + at + 32);
        const uint32_t csize = le32(d + at + 20), usize = le32(d + at + 24), local = le32(d + at + 42);
        if (method != 0 && method != 8) return "Invalid Mod (.zip): stored or deflated entries only";
        if (!nlen || at + 46 + nlen + xlen + clen > cd_at + cd_size) return bad;
        const char *name = (const char *) d + at + 46;
        if (memchr(name, 0, nlen) || memchr(name, '\\', nlen) || name[0] == '/' || (nlen >= 2 && name[1] == ':')) return "Invalid Mod (.zip): bad file name";
        for (size_t i = 0; i + 1 < nlen; i++)
            if (name[i] == '.' && name[i + 1] == '.' && (i == 0 || name[i - 1] == '/') && (i + 2 == nlen || name[i + 2] == '/'))
                return "Invalid Mod (.zip): bad file name";
        if (local > cd_at || local + 30 > cd_at || memcmp(d + local, "PK\3\4", 4) || csize > cd_at - local) return bad;
        unpacked += usize;
        if (unpacked > ZIP_UNPACKED_MAX || (method == 0 && csize != usize)) return bad;
        if (name[nlen - 1] != '/')
            for (size_t k = 0; k < sizeof dirs / sizeof dirs[0] && !known; k++) {
                const size_t dl = strlen(dirs[k]);
                known = nlen > dl && !memcmp(name, dirs[k], dl);
            }
        at += 46 + nlen + xlen + clen;
    }
    return known ? NULL : "Invalid Mod (.zip): no textures/, img/, sound/, models/, scripts/, shaders/, css/ or css-img/ files";
}

static unsigned char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        const long n = ftell(f);
        if (n >= 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t) n + 1))) {
            if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
                free(buf);
                buf = NULL;
            } else {
                buf[n] = 0;
                *len = (size_t) n;
            }
        }
    }
    fclose(f);
    return buf;
}

/* the check of one staged file; the reason or NULL */
static const char *check_file(const Ticket *t, const Part *p) {
    char dir[1200], path[1400], fb[24];
    staging_dir(t, dir, sizeof dir);
    const char *file = part_file(t, p, fb, sizeof fb);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    size_t n = 0;
    unsigned char *d = read_all(path, &n);
    if (!d || (long long) n != p->want) {
        free(d);
        return "Upload incomplete";
    }
    char rel[64];
    snprintf(rel, sizeof rel, "1/%s", file);
    const char *err = !user_asset_content_ok(rel, d, n) ? "Invalid File" : NULL;
    if (!err) {
        switch (k_fields[p->field].check) {
            case V_IMAGE: err = check_image(d, n); break;
            case V_SOUND: err = check_sound(d, n); break;
            case V_VIDEO: err = check_video(d, n); break;
            case V_MODEL: err = check_model(t->ext, d, n); break;
            case V_ZIP: err = check_zip(d, n); break;
            default: err = check_data(d, n); break;
        }
    }
    free(d);
    return err;
}

/* ------------------------------------------------------------------ database */

/* (me, name, category, data, size_kb, private 0/1, uvc|NULL, kind) -> {id, updated}: the account's asset of that name
 * and kind is updated (version + 1), else a new one is inserted. A C proc on every driver (db_procs.h). */
static int data_kind(const char *data) {
    cJSON *d = cJSON_Parse(data ? data : "");
    int k = K_MODEL;
    if (cJSON_GetObjectItemCaseSensitive(d, "d")) k = K_PREFAB;
    if (cJSON_GetObjectItemCaseSensitive(d, "x")) k = cJSON_GetObjectItemCaseSensitive(d, "v") ? K_VIDEO : K_TEXTURE;
    if (cJSON_GetObjectItemCaseSensitive(d, "u")) k = K_SOUND;
    cJSON_Delete(d);
    return k;
}

static kn_db_result *run_asset_publish(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (n < 8) return kn_db_result_user_error("bad call");
    const int kind = atoi(p[7]);
    cJSON *rows = dbx_rows(&x, SQL(.pg = "SELECT id, data::text AS data FROM published_assets WHERE creator_id = $1::bigint AND lower(name) = lower($2)"
                                         " AND deleted_at IS NULL ORDER BY id",
                                   .any = "SELECT id, data FROM published_assets WHERE creator_id = $1 AND lower(name) = lower($2)"
                                          " AND deleted_at IS NULL ORDER BY id"),
                           KN_DB_ARGS(p[0], p[1]));
    int64_t id = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(r, "data");
        char *dt = cJSON_IsString(d) ? NULL : cJSON_PrintUnformatted(d);
        const int k = data_kind(cJSON_IsString(d) ? d->valuestring : dt);
        free(dt);
        if (k == kind) {
            id = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "id"));
            break;
        }
    }
    cJSON_Delete(rows);
    int updated = 0;
    if (id > 0) {
        dbx_exec(&x, SQL(.pg = "UPDATE published_assets SET category = $2::int, data = $3::jsonb, size_kb = $4::int, is_private = $5::boolean,"
                               " uvc = $6::jsonb, version = version + 1, updated_at = now() WHERE id = $1::bigint",
                         .any = "UPDATE published_assets SET category = $2, data = $3, size_kb = $4, is_private = $5, uvc = $6,"
                                " version = version + 1, updated_at = {now} WHERE id = $1"),
                 KN_DB_ARGS(I64S(id), p[2], p[3], p[4], p[5], p[6]));
        updated = 1;
    } else if (!dbx_failed(&x)) {
        const int64_t mine = dbx_int(&x, SQL(.pg = "SELECT count(*) FROM published_assets WHERE creator_id = $1::bigint AND deleted_at IS NULL",
                                             .any = "SELECT count(*) FROM published_assets WHERE creator_id = $1 AND deleted_at IS NULL"),
                                     KN_DB_ARGS(p[0]), 0);
        if (n > 8 && mine >= atoll(p[8])) return dbx_finish(&x, kn_db_result_user_error("Asset limit reached"));
        id = dbx_insert_id(&x,
                           SQL(.pg = "INSERT INTO published_assets (creator_id, name, category, data, size_kb, is_private, uvc)"
                                     " VALUES ($1::bigint, $2, $3::int, $4::jsonb, $5::int, $6::boolean, $7::jsonb) RETURNING id",
                               .sqlite = "INSERT INTO published_assets (creator_id, name, category, data, size_kb, is_private, uvc)"
                                         " VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING id",
                               .any = "INSERT INTO published_assets (creator_id, name, category, data, size_kb, is_private, uvc)"
                                      " VALUES ($1, $2, $3, $4, $5, $6, $7)"),
                           KN_DB_ARGS(p[0], p[1], p[2], p[3], p[4], p[5], p[6]));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) id);
    cJSON_AddNumberToObject(o, "updated", updated);
    return dbx_json(&x, "result", o);
}

static const kn_proc P_ASSET_PUBLISH = {"asset_publish", NULL, run_asset_publish};

/* (me, name, mod_info, max mods) -> {id, updated}: the account's mod of that name is updated (mod_updatecounter + 1,
 * mod_date), else a new published_mods row (mod_url "ua"); another account's live mod of that name refuses with
 * MODS_UPLOAD_NAME_TAKEN (published_mods_name_lower_uq). */
static kn_db_result *run_mod_publish(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (n < 4) return kn_db_result_user_error("bad call");
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT mod_id AS id__i, coalesce(creator_id, 0) AS creator__i FROM published_mods"
                                       " WHERE lower(mod_name) = lower($1) AND deleted_at IS NULL"),
                         KN_DB_ARGS(p[1]));
    int64_t id = 0;
    if (row) {
        if (kn_json_int(row, "creator", 0) != atoll(p[0])) {
            cJSON_Delete(row);
            return dbx_finish(&x, kn_db_result_user_error("MODS_UPLOAD_NAME_TAKEN"));
        }
        id = kn_json_int(row, "id", 0);
        cJSON_Delete(row);
    }
    int updated = 0;
    if (id > 0) {
        dbx_exec(&x, SQL(.pg = "UPDATE published_mods SET mod_url = 'ua', mod_info = $2::jsonb, mod_updatecounter = mod_updatecounter + 1,"
                               " mod_date = now() WHERE mod_id = $1::int",
                         .any = "UPDATE published_mods SET mod_url = 'ua', mod_info = $2, mod_updatecounter = mod_updatecounter + 1,"
                                " mod_date = {now} WHERE mod_id = $1"),
                 KN_DB_ARGS(I64S(id), p[2]));
        updated = 1;
    } else if (!dbx_failed(&x)) {
        const int64_t mine = dbx_int(&x, SQL(.any = "SELECT count(*) FROM published_mods WHERE creator_id = {int:$1} AND deleted_at IS NULL"),
                                     KN_DB_ARGS(p[0]), 0);
        if (mine >= atoll(p[3])) return dbx_finish(&x, kn_db_result_user_error("MODS_PUBLISH_SIZE_LIMIT"));
        /* the id from USER_ASSET_KN_ID_BASE, as the assets': md<id> of a Krunker mod (md43867) stays Krunker's, so the
         * brain's user-assets mirror never answers one for the other (user_asset_rel_is_kn) */
        const int64_t top = dbx_int(&x, SQL(.any = "SELECT coalesce(max(mod_id), 0) FROM published_mods WHERE mod_id >= {int:$1}"),
                                    KN_DB_ARGS(I64S(USER_ASSET_KN_ID_BASE)), 0);
        id = top >= USER_ASSET_KN_ID_BASE ? top + 1 : USER_ASSET_KN_ID_BASE;
        dbx_exec(&x, SQL(.pg = "INSERT INTO published_mods (mod_id, mod_name, creator_id, mod_url, mod_info)"
                               " VALUES ($1::int, $3, $2::int, 'ua', $4::jsonb)",
                         .any = "INSERT INTO published_mods (mod_id, mod_name, creator_id, mod_url, mod_info) VALUES ($1, $3, $2, 'ua', $4)"),
                 KN_DB_ARGS(I64S(id), p[0], p[1], p[2]));
        if (dbx_failed(&x)) id = 0;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) id);
    cJSON_AddNumberToObject(o, "updated", updated);
    return dbx_json(&x, "result", o);
}

static const kn_proc P_MOD_PUBLISH = {"mod_publish", NULL, run_mod_publish};

/* (me, name) -> {premium, score, hack, taken}: what publishKey checks (taken: another account's live mod of the name) */
static kn_db_result *run_mod_key_check(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (n < 2) return kn_db_result_user_error("bad call");
    cJSON *o = dbx_row(&x, SQL(.any = "SELECT coalesce(player_premium, 0) AS premium__i, coalesce(player_score, 0) AS score__i,"
                                     " coalesce(player_hack, 0) AS hack__i FROM players WHERE player_id = {int:$1}"),
                       KN_DB_ARGS(p[0]));
    if (!o) o = cJSON_CreateObject();
    cJSON *acc = dbx_row(&x, SQL(.any = "SELECT username FROM accounts WHERE id = {int:$1}"), KN_DB_ARGS(p[0]));
    cJSON_AddStringToObject(o, "username", kn_json_str(acc, "username", ""));
    cJSON_Delete(acc);
    const int64_t taken = dbx_int(&x, SQL(.any = "SELECT count(*) FROM published_mods WHERE lower(mod_name) = lower($2)"
                                                " AND deleted_at IS NULL AND coalesce(creator_id, 0) <> {int:$1}"),
                                  KN_DB_ARGS(p[0], p[1]), 0);
    cJSON_AddNumberToObject(o, "taken", (double) taken);
    return dbx_json(&x, "result", o);
}

static const kn_proc P_MOD_KEY_CHECK = {"mod_key_check", NULL, run_mod_key_check};

/* ------------------------------------------------------------------ uploadA */

static void refuse(SocialSession *s, const char *msg) {
    cJSON *fr = social_frame("upMp");
    cJSON_AddItemToArray(fr, cJSON_CreateString(msg));
    social_send(s, fr);
}

typedef struct Begin {
    Ticket t;            /* the request, copied into a slot once the account checks passed */
    long long total;     /* bytes declared */
    long long thumb;     /* the thumbnail's declared bytes (its limit needs the account's premium) */
    int model_total;     /* the model's files count against the 100 / 250 MB total */
} Begin;

static int hour_count(const int64_t account, const uint64_t now, const int add) {
    const uint64_t window = now / 3600000u;
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < (int) (sizeof R / sizeof R[0]); i++) {
        if (R[i].account == account) {
            if (R[i].window != window) R[i].window = window, R[i].n = 0;
            R[i].n += add;
            return R[i].n;
        }
        if (!R[i].account && free_slot < 0) free_slot = i;
        if (R[i].window < R[oldest].window) oldest = i;
    }
    const int i = free_slot >= 0 ? free_slot : oldest;
    R[i].account = account;
    R[i].window = window;
    R[i].n = add;
    R[i].last = 0;
    return add;
}

/* kn_now_ms of the account's last mod key, 0 when none is remembered; set_last_mod_key records one (after
 * ticket_open, whose hour_count made the account's entry) */
static uint64_t last_mod_key(const int64_t account) {
    for (int i = 0; i < (int) (sizeof R / sizeof R[0]); i++)
        if (R[i].account == account) return R[i].last;
    return 0;
}

static void set_last_mod_key(const int64_t account, const uint64_t now) {
    for (int i = 0; i < (int) (sizeof R / sizeof R[0]); i++)
        if (R[i].account == account) R[i].last = now;
}

/* a new ticket from the checked request (its id drawn, its staging directory and record made); NULL with *why */
static Ticket *ticket_open(const Ticket *req, const uint64_t now, const char **why) {
    /* a new upload of the account replaces its open one of the same flow (the retry after a failure): an asset's
     * (the editor) and a mod's (the menu) are apart */
    Ticket *slot = NULL;
    for (int i = 0; i < MAX_TICKETS; i++)
        if (T[i].used && T[i].account == req->account && !T[i].committing && (T[i].kind == K_MOD) == (req->kind == K_MOD))
            ticket_free(&T[i]);
    for (int i = 0; i < MAX_TICKETS && !slot; i++)
        if (!T[i].used) slot = &T[i];
    if (!slot) {
        *why = "Server busy, try again later";
        return NULL;
    }
    *slot = *req;
    if (kn_random_hex(slot->id, sizeof slot->id, 16) != 0) {
        memset(slot, 0, sizeof *slot);
        *why = "Upload Request Failed";
        return NULL;
    }
    slot->used = 1;
    slot->touched = now;
    char dir[1200];
    staging_dir(slot, dir, sizeof dir);
    if (mkdirs(dir) != 0 || ticket_save(slot) != 0) {
        KN_WARN(MOD, "cannot create %s: %s", dir, strerror(errno));
        ticket_free(slot);
        *why = "Upload Request Failed";
        return NULL;
    }
    hour_count(req->account, now, 1);
    return slot;
}

static void begin_done(SocialSession *s, kn_db_result *res, void *extra) {
    Begin *b = extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        refuse(s, "Upload Request Failed");
        return;
    }
    const int premium = g_acct.free_premium || kn_db_get_int64(res, 0, 0, 0) > 0;
    const long long used_kb = kn_db_get_int64(res, 0, 1, 0);
    if (b->thumb > (premium ? 500 : 250) * KB) {
        refuse(s, "File size Limit Reached");
        return;
    }
    if (b->model_total && b->total > (premium ? 250 : 100) * MB) {
        refuse(s, "File size Limit Reached");
        return;
    }
    if (used_kb * KB + b->total > C.quota) {
        refuse(s, "Asset storage limit reached");
        return;
    }
    const uint64_t now = kn_now_ms();
    sweep(now);
    if (hour_count(s->account_id, now, 0) >= C.per_hour) {
        refuse(s, "Too many uploads, try again later");
        return;
    }
    const char *why = NULL;
    const Ticket *slot = ticket_open(&b->t, now, &why);
    if (!slot) {
        refuse(s, why);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fileId", slot->id);
    if (C.host[0]) cJSON_AddStringToObject(o, "srv", C.host);
    else cJSON_AddNullToObject(o, "srv");
    cJSON *fr = social_frame("pubR");
    cJSON_AddItemToArray(fr, o);
    social_send(s, fr);
}

void asset_upload_begin(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    if (!C.enabled || !g_b || !g_b->db) {
        refuse(s, "File uploads are not supported on this server");
        return;
    }
    Begin *b = calloc(1, sizeof *b);
    if (!b) return;
    Ticket *t = &b->t;
    t->account = s->account_id;
    snprintf(t->username, sizeof t->username, "%s", s->username);
    /* the name: trimmed, 1..128 characters */
    const char *name = fa_str(f, 1);
    while (name && (*name == ' ' || *name == '\t')) name++;
    size_t nl = name ? strlen(name) : 0;
    while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\t')) nl--;
    if (!nl) {
        free(b);
        refuse(s, "Invalid Name");
        return;
    }
    const size_t keep = brain_utf16_prefix(name, NAME_MAX_LEN);
    snprintf(t->name, sizeof t->name, "%.*s", (int) (keep < nl ? keep : nl), name);
    const cJSON *code = fa(f, 8);
    const int type = cJSON_IsNumber(code) ? (int) code->valuedouble : 0;
    const cJSON *sizes = fa(f, 6);
    const int prefab = cJSON_GetObjectItemCaseSensitive(sizes, "data") != NULL;
    t->kind = type == 2 ? K_TEXTURE : type == 3 ? K_SOUND : type == 4 ? K_VIDEO : prefab ? K_PREFAB : K_MODEL;
    t->category = (int) fa_int(f, 3, -1);
    /* config.assetCat: 18 categories, 16 Texture and 17 Sound only for those kinds (publishAssetPopup skips them) */
    const int cat_ok = t->kind == K_SOUND ? t->category == 17 : t->kind >= K_TEXTURE ? t->category == 16
                                                                 : t->category >= 0 && t->category < 18 && t->category != 16 && t->category != 17;
    if (!cat_ok) {
        free(b);
        refuse(s, "Invalid Category");
        return;
    }
    const cJSON *pv = fa(f, 4);
    t->priv = cJSON_IsTrue(pv) || (cJSON_IsNumber(pv) && pv->valuedouble != 0);
    const char *ext = fa_str(f, 7);
    snprintf(t->ext, sizeof t->ext, "%s", ext && *ext ? ext : "obj");
    for (char *c = t->ext; *c; c++) *c = (char) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    if (t->kind == K_MODEL && strcmp(t->ext, "obj") && strcmp(t->ext, "gltf") && strcmp(t->ext, "glb") && strcmp(t->ext, "fbx")) {
        free(b);
        refuse(s, "Invalid Model Type");
        return;
    }
    /* the files */
    if (!cJSON_IsObject(sizes)) {
        free(b);
        refuse(s, "Missing File");
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, sizes) {
        int fi = -1;
        for (int i = 0; i < FIELD_COUNT; i++)
            if (it->string && !strcmp(it->string, k_fields[i].key)) fi = i;
        const double v = cJSON_IsNumber(it) ? it->valuedouble : -1;
        if (fi < 0 || !field_allowed(t->kind, fi) || !(v >= 1) || v > 300 * MB || t->nparts >= MAX_PARTS) {
            free(b);
            refuse(s, "Invalid File");
            return;
        }
        int dup = 0;
        for (int i = 0; i < t->nparts; i++) dup |= t->parts[i].field == fi;
        if (dup) continue;
        const long long want = (long long) v;
        if (k_fields[fi].limit > 0 && want > k_fields[fi].limit) {
            free(b);
            refuse(s, "File size Limit Reached");
            return;
        }
        if (k_fields[fi].limit < 0) b->thumb = want;
        t->parts[t->nparts++] = (Part){fi, want, 0};
        b->total += want;
    }
    int has_required = 0;
    for (int i = 0; i < t->nparts; i++) has_required |= !strcmp(k_fields[t->parts[i].field].key, k_required[t->kind]);
    if (!has_required) {
        free(b);
        refuse(s, "Missing File");
        return;
    }
    b->model_total = t->kind == K_MODEL;
    /* the per-map UV channels ({key: 1..7} with the popup's keys, editor-10.0.7.js:20700-20716): kept by letter */
    const cJSON *uvc = fa(f, 9);
    if (t->kind == K_MODEL && cJSON_IsObject(uvc)) {
        cJSON *keep_uv = cJSON_CreateObject();
        cJSON_ArrayForEach(it, uvc) {
            for (int i = 0; i < FIELD_COUNT; i++) {
                if (!it->string || strcmp(it->string, k_fields[i].key) || !cJSON_IsNumber(it)) continue;
                const int ch = (int) it->valuedouble;
                const char letter[2] = {k_fields[i].letter ? k_fields[i].letter : 'c', 0}; /* c: the diffuse (colour) map */
                if (ch >= 1 && ch <= 7) cJSON_AddNumberToObject(keep_uv, letter, ch);
            }
        }
        char *txt = cJSON_GetArraySize(keep_uv) > 0 ? cJSON_PrintUnformatted(keep_uv) : NULL;
        if (txt && strlen(txt) < sizeof t->uvc) snprintf(t->uvc, sizeof t->uvc, "%s", txt);
        free(txt);
        cJSON_Delete(keep_uv);
    }
    char me[24];
    social_query(s, SQL(.pg = "SELECT coalesce((SELECT player_premium FROM players WHERE player_id = $1::bigint), 0),"
                              " coalesce((SELECT sum(size_kb) FROM published_assets WHERE creator_id = $1::bigint AND deleted_at IS NULL), 0)",
                        .any = "SELECT coalesce((SELECT player_premium FROM players WHERE player_id = $1), 0),"
                               " coalesce((SELECT sum(size_kb) FROM published_assets WHERE creator_id = $1 AND deleted_at IS NULL), 0)"),
                 KN_DB_ARGS(acct_itoa(me, s->account_id)), begin_done, b);
}

/* ------------------------------------------------------------------ POST /upload-user-asset */

static void reply_text(kn_http_req *req, const int status, const char *text) {
    brain_cors_headers(req);
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond(req, status, "text/plain; charset=utf-8", text, strlen(text));
}

/* a refusal on the mods' route: {error: {code, message}} with Krunker's MODS_UPLOAD_* code (errors.mods.*,
 * deob-10.0.7.js:223312-223324), the reason as the message */
static void reply_fail(kn_http_req *req, const int mod, const int status, const char *text) {
    if (!mod) {
        reply_text(req, status, text);
        return;
    }
    const char *code = !strncmp(text, "MODS_", 5)                                      ? text
                       : status == 404 || status == 410                                ? "MODS_UPLOAD_INVALID_KEY"
                       : !strcmp(text, "Upload incomplete") || !strcmp(text, "File size Limit Reached") ||
                                 !strcmp(text, "Upload out of order") || !strcmp(text, "Invalid Chunk") ? "MODS_UPLOAD_SIZE_MISMATCH"
                       : !strcmp(text, "Invalid File")                                 ? "MODS_UPLOAD_MISSING_FILE"
                       : status == 400                                                 ? "MODS_UPLOAD_WRONG_TYPE"
                                                                                       : "MODS_UPLOAD_STORAGE_FAIL";
    brain_cors_headers(req);
    kn_http_set_header(req, "Cache-Control", "no-store");
    v10_reply_error(req, status, code, strncmp(text, "MODS_", 5) ? text : "The mod could not be stored, try again");
}

static void reply_got(kn_http_req *req, const long long got) {
    char body[64];
    snprintf(body, sizeof body, "{\"got\":%lld}", got);
    brain_cors_headers(req);
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond(req, 200, "application/json", body, strlen(body));
}

typedef struct Commit {
    Ticket *t;
    char id[40];
    int mod;           /* the mods' route: its answers are v10's {data} / {error} */
    kn_http_req *req;
    const char *err;   /* the worker's verdict */
    char data[160];    /* the asset's data JSON (its letters) */
    long long size_kb;
} Commit;

static void commit_db_done(kn_db_result *res, void *ud);

/* loop thread: the checks are done */
static void commit_checked(kn_loop *loop, void *ud) {
    (void) loop;
    Commit *c = ud;
    Ticket *t = ticket_by_id(c->id);
    if (!t || t != c->t) { /* cannot happen while committing; be safe */
        if (kn_http_req_alive(c->req)) reply_fail(c->req, c->mod, 410, "Upload expired");
        free(c);
        return;
    }
    if (c->err) {
        t->committing = 0;
        t->touched = kn_now_ms();
        if (kn_http_req_alive(c->req)) reply_fail(c->req, c->mod, 400, c->err);
        free(c);
        return;
    }
    char me[24], cat[24], kb[24], kind[24];
    if (t->kind == K_MOD) {
        if (kn_db_call(g_b->db, &P_MOD_PUBLISH, KN_DB_ARGS(acct_itoa(me, t->account), t->name, c->data, acct_itoa(kb, C.max_assets)),
                       commit_db_done, c) != 0) {
            t->committing = 0;
            if (kn_http_req_alive(c->req)) reply_fail(c->req, 1, 503, "MODS_UPLOAD_STORAGE_FAIL");
            free(c);
        }
        return;
    }
    if (kn_db_call(g_b->db, &P_ASSET_PUBLISH,
                   KN_DB_ARGS(acct_itoa(me, t->account), t->name, acct_itoa(cat, t->category), c->data, acct_itoa(kb, c->size_kb),
                              t->priv ? "1" : "0", t->uvc[0] ? t->uvc : NULL, acct_itoa(kind, t->kind), acct_itoa((char[24]){0}, C.max_assets)),
                   commit_db_done, c) != 0) {
        t->committing = 0;
        if (kn_http_req_alive(c->req)) reply_text(c->req, 503, "Upload Request Failed");
        free(c);
    }
}

static void *commit_thread(void *arg) {
    Commit *c = arg;
    const Ticket *t = c->t; /* committing: the loop leaves it alone */
    for (int i = 0; i < t->nparts && !c->err; i++) c->err = check_file(t, &t->parts[i]);
    if (!c->err) {
        /* the asset id's letters, in the popup's order (thumbnail first) */
        cJSON *d = cJSON_CreateObject();
        long long bytes = 0;
        for (int i = 0; i < t->nparts; i++) {
            const struct Field *f = &k_fields[t->parts[i].field];
            bytes += t->parts[i].want;
            if (f->letter) {
                const char k[2] = {f->letter, 0};
                cJSON_AddNumberToObject(d, k, 1);
            }
        }
        if (t->kind == K_MODEL && strcmp(t->ext, "obj")) {
            const char k[2] = {!strcmp(t->ext, "gltf") ? 'g' : !strcmp(t->ext, "glb") ? 'q' : 'f', 0};
            cJSON_AddNumberToObject(d, k, 1);
        }
        if (t->kind == K_PREFAB) cJSON_AddNumberToObject(d, "d", 1);
        if (t->kind == K_TEXTURE || t->kind == K_VIDEO) cJSON_AddNumberToObject(d, "x", 1);
        if (t->kind == K_VIDEO) cJSON_AddNumberToObject(d, "v", 1);
        if (t->kind == K_SOUND) cJSON_AddNumberToObject(d, "u", 1);
        /* a mod's mod_info: {"t": 1} with a thumbnail (the Mod Manager shows /md<id>/thumb.png then, :217723) */
        char *txt = cJSON_PrintUnformatted(d);
        snprintf(c->data, sizeof c->data, "%s", txt ? txt : "{}");
        free(txt);
        cJSON_Delete(d);
        c->size_kb = (bytes + 999) / 1000;
    }
    kn_loop_post_threadsafe(g_b->loop, commit_checked, c);
    atomic_fetch_sub(&g_live, 1);
    return NULL;
}

/* the files of the committed asset into uploads.dir/<id>/ (an update replaces the old directory whole) */
static int install(const Ticket *t, const int64_t id) {
    char from[1200], to[1200], old[1300];
    staging_dir(t, from, sizeof from);
    snprintf(to, sizeof to, "%s%s%lld", C.dir, t->kind == K_MOD ? "md" : "", (long long) id);
    snprintf(old, sizeof old, "%s.old-%s", to, t->id);
    struct stat st;
    const int had = !stat(to, &st);
    if (had && rename(to, old) != 0) return -1;
    if (rename(from, to) != 0) {
        if (had) rename(old, to);
        return -1;
    }
    if (had) rm_tree(old);
    return 0;
}

static void commit_db_done(kn_db_result *res, void *ud) {
    Commit *c = ud;
    Ticket *t = ticket_by_id(c->id);
    const char *user = kn_db_user_message(res);
    if (!t) {
        if (kn_http_req_alive(c->req)) reply_fail(c->req, c->mod, 410, "Upload expired");
    } else if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        t->committing = 0;
        if (!user) KN_WARN(MOD, "publish of %s failed: %s", t->name, kn_db_error(res));
        if (kn_http_req_alive(c->req))
            reply_fail(c->req, c->mod, user ? 400 : 503, user ? user : c->mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload Request Failed");
    } else {
        cJSON *o = cJSON_Parse(kn_db_get(res, 0, 0));
        const int64_t id = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "id"));
        const int updated = (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "updated"));
        cJSON_Delete(o);
        if (id <= 0 || install(t, id) != 0) {
            KN_WARN(MOD, "storing %s %lld of %s failed: %s", t->kind == K_MOD ? "mod" : "asset", (long long) id, t->username, strerror(errno));
            t->committing = 0;
            if (kn_http_req_alive(c->req)) reply_fail(c->req, c->mod, 503, c->mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload Request Failed");
        } else {
            KN_INFO(MOD, "%s %s %s %lld \"%s\" (%lld kB)", t->username, updated ? "updated" : "published", t->kind == K_MOD ? "mod" : "asset",
                    (long long) id, t->name, c->size_kb);
            if (kn_http_req_alive(c->req)) {
                brain_cors_headers(c->req);
                if (c->mod) { /* publishUpload: {data: {mod_id}} */
                    cJSON *d = cJSON_CreateObject();
                    cJSON_AddNumberToObject(d, "mod_id", (double) id);
                    cJSON_AddNumberToObject(d, "updated", updated);
                    v10_reply_data(c->req, 200, d);
                } else {
                    char body[64];
                    snprintf(body, sizeof body, "{\"id\":%lld,\"updated\":%d}", (long long) id, updated);
                    kn_http_respond(c->req, 200, "application/json", body, strlen(body));
                }
            }
            ticket_free(t);
        }
    }
    free(c);
}

/* POST /upload-user-asset (ud NULL) and POST /mods/publish (ud 1: a mod's ticket, v10's answers) */
static void h_upload(kn_http_req *req, void *ud) {
    const int mod = ud != NULL;
    char id[40] = "", key[24] = "", off[24] = "", done[8] = "";
    kn_http_query_get(req, "fileId", id, sizeof id);
    kn_http_query_get(req, "key", key, sizeof key);
    kn_http_query_get(req, "offset", off, sizeof off);
    kn_http_query_get(req, "done", done, sizeof done);
    if (!C.enabled) {
        reply_fail(req, mod, 503, mod ? "MODS_UPLOAD_STORAGE_FAIL" : "File uploads are not supported on this server");
        return;
    }
    const uint64_t now = kn_now_ms();
    sweep(now);
    Ticket *t = ticket_by_id(id);
    if (!t || (t->kind == K_MOD) != mod) {
        reply_fail(req, mod, 404, "Upload expired");
        return;
    }
    if (t->committing) {
        reply_fail(req, mod, 409, mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload in progress");
        return;
    }
    t->touched = now;
    if (done[0] == '1') {
        for (int i = 0; i < t->nparts; i++)
            if (t->parts[i].got != t->parts[i].want) {
                reply_fail(req, mod, 400, "Upload incomplete");
                return;
            }
        Commit *c = calloc(1, sizeof *c);
        if (!c) {
            reply_fail(req, mod, 503, mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload Request Failed");
            return;
        }
        c->t = t;
        c->mod = mod;
        c->req = req;
        snprintf(c->id, sizeof c->id, "%s", t->id);
        t->committing = 1;
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        atomic_fetch_add(&g_live, 1);
        const int ok = pthread_create(&th, &attr, commit_thread, c) == 0;
        pthread_attr_destroy(&attr);
        if (!ok) {
            atomic_fetch_sub(&g_live, 1);
            t->committing = 0;
            free(c);
            reply_fail(req, mod, 503, mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload Request Failed");
        }
        return;
    }
    Part *p = NULL;
    for (int i = 0; i < t->nparts; i++)
        if (!strcmp(k_fields[t->parts[i].field].key, key)) p = &t->parts[i];
    if (!p) {
        reply_fail(req, mod, 400, "Invalid File");
        return;
    }
    char *end = NULL;
    const long long at = strtoll(off, &end, 10);
    const long long len = (long long) req->body_len;
    if (!off[0] || !end || *end || at < 0 || len < 1 || len > (long long) CHUNK_MAX) {
        reply_fail(req, mod, 400, "Invalid Chunk");
        return;
    }
    if (at + len <= p->got) { /* a chunk already received: a re-send after a lost answer */
        reply_got(req, p->got);
        return;
    }
    /* a chunk starting inside what is staged (a re-send of the one a brain restart cut off, half written) is
     * written again whole: the same bytes, and the check at done reads the file as it ends up */
    if (at > p->got) {
        reply_fail(req, mod, 409, "Upload out of order");
        return;
    }
    if (at + len > p->want) {
        reply_fail(req, mod, 400, "File size Limit Reached");
        return;
    }
    char dir[1200], path[1400], fb[24];
    staging_dir(t, dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, part_file(t, p, fb, sizeof fb));
    const int fd = open(path, O_WRONLY | O_CREAT | (at == 0 ? O_TRUNC : 0), 0644);
    if (fd >= 0 && at > 0 && at < p->got && ftruncate(fd, (off_t) at) != 0) { /* drop what follows: it is sent again */
        close(fd);
        reply_fail(req, mod, 503, mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload Request Failed");
        return;
    }
    const int wrote = fd >= 0 && pwrite(fd, req->body, (size_t) len, (off_t) at) == (ssize_t) len;
    if (fd >= 0) close(fd);
    if (!wrote) {
        KN_WARN(MOD, "writing %s failed: %s", path, strerror(errno));
        reply_fail(req, mod, 503, mod ? "MODS_UPLOAD_STORAGE_FAIL" : "Upload Request Failed");
        return;
    }
    p->got = at + len;
    reply_got(req, p->got);
}

/* ------------------------------------------------------------------ POST /mods/publish/key */

typedef struct ModKey {
    kn_http_req *req;
    Ticket t;
    long long mod, thumb; /* assetSizes */
} ModKey;

static void mod_key_fail(ModKey *k, const int status, const char *code, const char *message) {
    if (kn_http_req_alive(k->req)) v10_reply_error(k->req, status, code, message);
    free(k);
}

#define KEY_FAIL(...)              \
    do {                           \
        mod_key_fail(__VA_ARGS__); \
        return;                    \
    } while (0)

static void mod_key_checked(kn_db_result *res, void *ud) {
    ModKey *k = ud;
    if (!kn_http_req_alive(k->req)) {
        free(k);
        return;
    }
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) {
        KEY_FAIL(k, 503, "MODS_UPLOAD_STORAGE_FAIL", "The mod could not be stored, try again");
    }
    const int premium = g_acct.free_premium || kn_json_int(o, "premium", 0) > 0;
    const int level = accounts_level(kn_json_int(o, "score", 0), NULL);
    const int hack = kn_json_int(o, "hack", 0) > 0, taken = kn_json_int(o, "taken", 0) > 0;
    snprintf(k->t.username, sizeof k->t.username, "%s", kn_json_str(o, "username", ""));
    cJSON_Delete(o);
    const uint64_t now = kn_now_ms(), last = last_mod_key(k->t.account);
    if (hack) KEY_FAIL(k, 403, "MODS_PUBLISH_HACKER_FLAGGED", "Publishing mods is blocked for this account for now");
    if (level < MOD_LEVEL) KEY_FAIL(k, 403, "MODS_PUBLISH_LEVEL_TOO_LOW", "Mods can be published from level 5");
    if (k->thumb && level < MOD_THUMB_LEVEL)
        KEY_FAIL(k, 403, "MODS_PUBLISH_THUMB_LEVEL", "A mod thumbnail needs level 10");
    if (k->mod > (premium ? 50 : 25) * MB || k->thumb > (premium ? 500 : 250) * KB)
        KEY_FAIL(k, 400, "MODS_PUBLISH_SIZE_LIMIT", "The mod is larger than this account may upload");
    if (taken) KEY_FAIL(k, 409, "MODS_UPLOAD_NAME_TAKEN", "Another player already has a mod of that name");
    sweep(now);
    if ((last && (int64_t) (now - last) < C.mod_cooldown_ms) || hour_count(k->t.account, now, 0) >= C.per_hour)
        KEY_FAIL(k, 429, "MODS_PUBLISH_COOLDOWN", "Too soon after the last mod upload, try again shortly");
    k->t.parts[k->t.nparts++] = (Part){FIELD_COUNT - 1, k->mod, 0}; /* "mod" */
    if (k->thumb) k->t.parts[k->t.nparts++] = (Part){0, k->thumb, 0};
    const char *why = NULL;
    const Ticket *t = ticket_open(&k->t, now, &why);
    if (!t) KEY_FAIL(k, 503, "MODS_UPLOAD_STORAGE_FAIL", why);
    set_last_mod_key(k->t.account, now);
    char url[400];
    if (C.host[0]) snprintf(url, sizeof url, "%s%s/mods/publish", strstr(C.host, "://") ? "" : "https://", C.host);
    else snprintf(url, sizeof url, "/mods/publish");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "fileId", t->id);
    cJSON_AddStringToObject(d, "uploadUrl", url);
    KN_INFO(MOD, "%s opened a mod upload \"%s\" (%lld bytes%s)", t->username, t->name, k->mod, k->thumb ? ", a thumbnail" : "");
    v10_reply_data(k->req, 200, d);
    free(k);
}

static void mod_key_account(void *ud, const char *err, const cJSON *acc) {
    ModKey *k = ud;
    const int64_t id = err || !acc ? 0 : kn_json_int(acc, "id", 0);
    if (!kn_http_req_alive(k->req)) {
        free(k);
        return;
    }
    if (id <= 0) KEY_FAIL(k, 401, "LOGIN_REQUIRED", "You must be logged in to do this");
    k->t.account = id;
    char me[24];
    if (!g_b->db || kn_db_call(g_b->db, &P_MOD_KEY_CHECK, KN_DB_ARGS(acct_itoa(me, id), k->t.name), mod_key_checked, k) != 0)
        mod_key_fail(k, 503, "MODS_UPLOAD_STORAGE_FAIL", "The mod could not be stored, try again");
}

/* api.mods.publishKey({name, assetSizes: {mod, thumb}}) -> {data: {fileId, uploadUrl}} */
static void h_mod_key(kn_http_req *req, void *ud) {
    (void) ud;
    if (!C.enabled || !g_b || !g_b->db) {
        v10_reply_error(req, 503, "MODS_UPLOAD_STORAGE_FAIL", "Mod uploads are not available on this server");
        return;
    }
    cJSON *in = kn_http_body_json(req);
    ModKey *k = calloc(1, sizeof *k);
    if (!k) {
        cJSON_Delete(in);
        v10_reply_error(req, 500, "SERVER_ERROR", "Server error.");
        return;
    }
    k->req = req;
    k->t.kind = K_MOD;
    snprintf(k->t.ext, sizeof k->t.ext, "zip");
    /* the name: trimmed (submit() trims it too), 1..128 characters, no control characters or markup */
    const char *name = kn_json_str(in, "name", "");
    while (*name == ' ' || *name == '\t') name++;
    size_t nl = strlen(name);
    while (nl && (name[nl - 1] == ' ' || name[nl - 1] == '\t')) nl--;
    int restricted = 0;
    for (size_t i = 0; i < nl; i++) restricted |= (unsigned char) name[i] < 0x20 || name[i] == '<' || name[i] == '>' || name[i] == '"';
    const size_t keep = brain_utf16_prefix(name, NAME_MAX_LEN);
    snprintf(k->t.name, sizeof k->t.name, "%.*s", (int) (keep < nl ? keep : nl), name);
    const cJSON *sizes = cJSON_GetObjectItemCaseSensitive(in, "assetSizes");
    const cJSON *zm = cJSON_GetObjectItemCaseSensitive(sizes, "mod"), *zt = cJSON_GetObjectItemCaseSensitive(sizes, "thumb");
    k->mod = cJSON_IsNumber(zm) && zm->valuedouble >= 1 && zm->valuedouble <= 300 * MB ? (long long) zm->valuedouble : 0;
    k->thumb = cJSON_IsNumber(zt) && zt->valuedouble >= 1 && zt->valuedouble <= 300 * MB ? (long long) zt->valuedouble : 0;
    const int bad_thumb = zt && !cJSON_IsNull(zt) && !k->thumb;
    cJSON_Delete(in);
    if (!nl) KEY_FAIL(k, 400, "BAD_REQUEST", "Mod name is required");
    if (restricted) KEY_FAIL(k, 400, "MODS_PUBLISH_NAME_RESTRICTED", "That name cannot be used for a mod");
    if (!k->mod || bad_thumb) KEY_FAIL(k, 400, "MODS_UPLOAD_MISSING_FILE", "The mod needs a .zip file");
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, sizeof token, "%s", bearer ? bearer : "");
    }
    if (!token[0]) KEY_FAIL(k, 401, "LOGIN_REQUIRED", "You must be logged in to do this");
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(g_b, "token", args, mod_key_account, k);
    cJSON_Delete(args);
}

/* ------------------------------------------------------------------ module */

/* init: the tickets a previous run left (their records and staged files); anything else in .staging/ goes */
static void tickets_restore(void) {
    DIR *d = opendir(C.staging);
    if (!d) return;
    struct dirent *e;
    char p[1300];
    const time_t wall = time(NULL);
    int kept = 0;
    while ((e = readdir(d))) {
        const size_t nl = strlen(e->d_name);
        if (e->d_name[0] == '.' || nl != 32 + 5 || strcmp(e->d_name + 32, ".json")) continue;
        char id[40];
        snprintf(id, sizeof id, "%.32s", e->d_name);
        snprintf(p, sizeof p, "%s%s", C.staging, e->d_name);
        struct stat st;
        size_t len = 0;
        unsigned char *txt = stat(p, &st) == 0 ? read_all(p, &len) : NULL;
        cJSON *o = txt ? cJSON_ParseWithLength((const char *) txt, len) : NULL;
        free(txt);
        Ticket *t = NULL;
        for (int i = 0; i < MAX_TICKETS && !t; i++)
            if (!T[i].used) t = &T[i];
        int ok = o && t && kn_json_int(o, "account", 0) > 0;
        for (const char *c = id; ok && *c; c++) ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f');
        if (ok) {
            memset(t, 0, sizeof *t);
            snprintf(t->id, sizeof t->id, "%s", id);
            t->account = kn_json_int(o, "account", 0);
            snprintf(t->username, sizeof t->username, "%s", kn_json_str(o, "username", ""));
            snprintf(t->name, sizeof t->name, "%s", kn_json_str(o, "name", ""));
            t->category = (int) kn_json_int(o, "category", 0);
            t->kind = (int) kn_json_int(o, "kind", 0);
            t->priv = (int) kn_json_int(o, "priv", 0);
            snprintf(t->ext, sizeof t->ext, "%s", kn_json_str(o, "ext", "obj"));
            snprintf(t->uvc, sizeof t->uvc, "%s", kn_json_str(o, "uvc", ""));
            ok = t->name[0] && t->kind >= K_MODEL && t->kind <= K_MOD;
            /* the idle time: since the newest of the record and the staged files (a chunk touches its file) */
            time_t newest = st.st_mtime;
            char dir[1200], fp[1400], fb[24];
            staging_dir(t, dir, sizeof dir);
            const cJSON *it;
            cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, "parts")) {
                int fi = -1;
                for (int i = 0; i < FIELD_COUNT; i++)
                    if (!strcmp(kn_json_str(it, "key", ""), k_fields[i].key)) fi = i;
                const long long want = kn_json_int(it, "want", 0);
                if (fi < 0 || want < 1 || t->nparts >= MAX_PARTS || !field_allowed(t->kind, fi)) {
                    ok = 0;
                    break;
                }
                Part *pt = &t->parts[t->nparts++];
                *pt = (Part){fi, want, 0};
                struct stat fs;
                snprintf(fp, sizeof fp, "%s/%s", dir, part_file(t, pt, fb, sizeof fb));
                if (stat(fp, &fs) == 0) {
                    pt->got = (long long) fs.st_size < want ? (long long) fs.st_size : want;
                    if (fs.st_mtime > newest) newest = fs.st_mtime;
                }
            }
            if (ok && (int64_t) (wall - newest) * 1000 > C.ticket_ms) ok = 0;
            if (ok && stat(dir, &st) != 0) ok = mkdirs(dir) == 0;
            if (ok) {
                t->used = 1;
                t->touched = kn_now_ms();
                kept++;
            } else {
                t->used = 1; /* ticket_free removes its files */
                ticket_free(t);
            }
        }
        cJSON_Delete(o);
        if (!ok) {
            unlink(p);
            snprintf(p, sizeof p, "%s%s", C.staging, id);
            rm_tree(p);
        }
    }
    /* staging directories without a live ticket, and half-written records */
    rewinddir(d);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int live = 0;
        for (int i = 0; i < MAX_TICKETS && !live; i++)
            live = T[i].used && !strncmp(e->d_name, T[i].id, 32) && (!e->d_name[32] || !strcmp(e->d_name + 32, ".json"));
        if (live) continue;
        snprintf(p, sizeof p, "%s%s", C.staging, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode)) rm_tree(p);
        else unlink(p);
    }
    closedir(d);
    if (kept) KN_INFO(MOD, "%d upload ticket%s kept from the last run", kept, kept == 1 ? "" : "s");
}

int asset_upload_path(const char *rel, char *out, const size_t cap) {
    if (!C.enabled || !rel) return 0;
    return snprintf(out, cap, "%s%s", C.dir, rel) < (int) cap;
}

static void dir_with_slash(char *out, const size_t cap, const char *path) {
    snprintf(out, cap, "%s", path);
    const size_t n = strlen(out);
    if (n && out[n - 1] != '/' && n + 1 < cap) out[n] = '/', out[n + 1] = 0;
}

int asset_upload_init(Brain *b) {
    g_b = b;
    crc_init();
    C.enabled = kn_config_bool(b->cfg, "uploads.enabled", 1);
    char path[1024];
    const char *dir = kn_config_str(b->cfg, "uploads.dir", "data/user-uploads");
    if (!kn_config_resolve_path(b->cfg, dir, path, sizeof path)) snprintf(path, sizeof path, "%s", dir);
    dir_with_slash(C.dir, sizeof C.dir, path);
    snprintf(C.staging, sizeof C.staging, "%s.staging/", C.dir);
    snprintf(C.host, sizeof C.host, "%s", kn_config_str(b->cfg, "uploads.public_host", ""));
    C.ticket_ms = kn_config_int(b->cfg, "uploads.ticket_s", 1800) * 1000;
    C.quota = (long long) kn_config_int(b->cfg, "uploads.account_quota_mb", 2048) * MB;
    C.per_hour = (int) kn_config_int(b->cfg, "uploads.per_hour", 30);
    C.max_assets = (int) kn_config_int(b->cfg, "uploads.max_assets", 500);
    C.mod_cooldown_ms = kn_config_int(b->cfg, "uploads.mod_cooldown_ms", 10000);
    if (C.ticket_ms < 60000) C.ticket_ms = 60000;
    if (C.per_hour < 1) C.per_hour = 1;
    if (C.enabled) {
        if (mkdirs(C.staging) != 0) {
            KN_WARN(MOD, "cannot create %s (%s): uploads off", C.staging, strerror(errno));
            C.enabled = 0;
        } else {
            tickets_restore(); /* the open tickets of the last run carry on (see the notes at the top) */
        }
    }
    kn_http_route(b->http, "POST", "/upload-user-asset", h_upload, NULL);
    kn_http_route(b->http, "POST", "/api/upload-user-asset", h_upload, NULL);
    /* the mods' publishKey and publishUpload (deob-10.0.7.js:27315-27350) */
    kn_http_route(b->http, "POST", "/mods/publish/key", h_mod_key, NULL);
    kn_http_route(b->http, "POST", "/mods/publish", h_upload, (void *) 1);
    if (C.enabled) KN_INFO(MOD, "asset uploads on: %s", C.dir);
    return 0;
}

void asset_upload_shutdown(Brain *b) {
    (void) b;
    for (int i = 0; i < 500 && atomic_load(&g_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    /* the open tickets stay on disk: the next start carries them on (tickets_restore) */
    memset(T, 0, sizeof T);
}
