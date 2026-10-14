// kn_editor_mcp server state: options, allowed directories, projects (local map files and live
// editor projects over the editor bus), and the edit transaction every mutating tool uses.
#pragma once
#include "doc.h"
#include <cJSON.h>
#include <editor_bus.h>
#include <limits.h>

#define KN_MCP_VERSION "0.3.0"
#define KN_MCP_MAX_ROOTS 16

typedef enum { PROJ_FILE, PROJ_LIVE } ProjKind;

typedef struct Project {
    char id[32];          // "f1" (file) / "1.2" (editor)
    ProjKind kind;
    Doc *doc;             // file: the document; live: the mirror (export full), NULL until synced
    char name[160];
    char path[PATH_MAX];
    int live_revision;    // live: editor revision the mirror was taken at (-1 none)
    int live_dirty;
    int live_seen;        // still listed by the editor
    cJSON *groups;        // {"name": {"ids": [..]}} session groups
} Project;

// Live editor bus client (live.c)
typedef struct {
    int mode;             // 0 off (--no-live), 1 auto (attach when a bus exists), 2 required (--live)
    char bus_dir[512];    // "" = editor_bus default
    unsigned want_scopes; // EDBUS_SCOPE_*
    EdBusConn conn;
    int connected;
    int handshake_done;
    unsigned granted;
    char decision[32];
    char reason[256];
    int denied;           // cached decision: no automatic retry
    int next_id;
    int peer;
    char last_error[256];
    double permission_timeout_s;
    // In-process editor (client_bin's AI panel runs these tools itself: client/src/editor/
    // editor_ai_tools.c): requests go straight to the editor's own handler, not over the bus
    // socket; every scope is granted. NULL = the bus. Returns the op's result or NULL + err.
    cJSON *(*loopback)(const char *op, const cJSON *args, char *err, size_t errsz);
} Live;

typedef struct Ctx {
    char root[PATH_MAX];                  // KrunkNative checkout (assets, binaries)
    char workspace[PATH_MAX];             // scratch dir for new maps / prefab library / renders
    char roots[KN_MCP_MAX_ROOTS][PATH_MAX]; // allowed map directories (resolved)
    int root_count;
    char client_bin[PATH_MAX];            // resolved client_bin for renders ("" = search)
    char schema_path[PATH_MAX];
    int read_only;
    Project **projects;
    int project_count;
    int focus;                            // index into projects, -1 none
    int next_file_id;
    Live live;
    double render_timeout_s;
    int log_level;                        // MCP logging/setLevel
} Ctx;

// ---- paths / safety (ctx.c)
// Adds an allowed map directory (resolved; duplicates ignored). 0 ok, -1 not a directory / full.
int ctx_add_root(Ctx *ctx, const char *dir);
// Resolves `path` (relative to the root) and checks it lies under an allowed directory.
int ctx_safe_path(Ctx *ctx, const char *path, int for_write, char *out, size_t size, char *err, size_t errsz);
// Allowed roots as a JSON array.
cJSON *ctx_roots_json(const Ctx *ctx);

// ---- projects (project.c)
Project *project_add_file(Ctx *ctx, Doc *doc, const char *path, const char *name);
void project_remove(Ctx *ctx, Project *p);
// ref: NULL/"" = focused; id, path, basename (with or without .json) or map name. Live projects
// are synced (mirror refreshed) before return. 0 on success.
int project_resolve(Ctx *ctx, const char *ref, Project **out, char *err, size_t errsz);
int project_sync(Ctx *ctx, Project *p, char *err, size_t errsz);
void project_refresh_name(Project *p);
cJSON *project_json(Ctx *ctx, const Project *p);
// Refreshes the live project list from the editor (no-op without a bus). 0 ok.
int projects_refresh_live(Ctx *ctx, char *err, size_t errsz);
int project_index(const Ctx *ctx, const Project *p);

// ---- edit transactions (project.c). Tools mutate p->doc between begin and commit.
typedef struct {
    Ctx *ctx;
    Project *p;
    char label[128];
    char *before;            // file: snapshot text
    int *uids;
    int uid_count;
    int next_uid;
    cJSON *before_tree;      // live: mirror before
    int temp_next;           // live: next temporary (negative) id for new objects
    int *temp_ids, *real_ids;
    int remap_count;
    int changed;             // set by commit
    cJSON *summary;          // {added, updated, removed, map_keys}
} Edit;

int edit_begin(Ctx *ctx, Project *p, const char *label, Edit *e, char *err, size_t errsz);
// Adds obj (taken over) to the project's document; returns its (possibly temporary) id.
int edit_add_object(Edit *e, cJSON *obj, int index);
int edit_commit(Edit *e, char *err, size_t errsz); // 0 ok (also when nothing changed)
void edit_abort(Edit *e);
int edit_final_id(const Edit *e, int id);           // temporary -> editor id after commit
void edit_end(Edit *e);                             // frees the remap table / summary (after commit)

// ---- live bus (live.c)
void live_init(Live *l);
int live_bus_present(Live *l);  // socket (or port file) exists
// Connects + hello + permission handshake when needed and checks `need` scopes. 0 ok.
int live_ensure(Ctx *ctx, unsigned need, char *err, size_t errsz);
// Host op request; returns the owned result object or NULL (err set).
cJSON *live_request(Ctx *ctx, const char *op, cJSON *args, int timeout_ms, char *err, size_t errsz);
// "call" op on a project.
cJSON *live_call(Ctx *ctx, const char *project, const char *tool, cJSON *args, int timeout_ms, char *err, size_t errsz);
cJSON *live_status_json(Ctx *ctx);
void live_disconnect(Live *l);
