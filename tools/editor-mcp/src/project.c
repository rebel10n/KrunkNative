// Projects (local map files and live editor projects) and edit transactions.
#define _GNU_SOURCE
#include "ctx.h"
#include "jsonw.h"
#include "objops.h"
#include "schema.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int project_index(const Ctx *ctx, const Project *p) {
    for (int i = 0; i < ctx->project_count; i++) if (ctx->projects[i] == p) return i;
    return -1;
}

void project_refresh_name(Project *p) {
    const char *n = p->doc ? js_str(p->doc->root, "name", NULL) : NULL;
    if (n && n[0]) snprintf(p->name, sizeof(p->name), "%s", n);
    else if (p->path[0]) snprintf(p->name, sizeof(p->name), "%s", path_basename(p->path));
}

static Project *project_push(Ctx *ctx) {
    Project **items = realloc(ctx->projects, sizeof(Project *) * (size_t) (ctx->project_count + 1));
    if (!items) return NULL;
    ctx->projects = items;
    Project *p = calloc(1, sizeof(Project));
    if (!p) return NULL;
    p->groups = cJSON_CreateObject();
    p->live_revision = -1;
    ctx->projects[ctx->project_count++] = p;
    return p;
}

Project *project_add_file(Ctx *ctx, Doc *doc, const char *path, const char *name) {
    Project *p = project_push(ctx);
    if (!p) return NULL;
    p->kind = PROJ_FILE;
    p->doc = doc;
    snprintf(p->id, sizeof(p->id), "f%d", ++ctx->next_file_id);
    if (path) snprintf(p->path, sizeof(p->path), "%s", path);
    if (name && name[0]) snprintf(p->name, sizeof(p->name), "%s", name);
    project_refresh_name(p);
    ctx->focus = ctx->project_count - 1;
    return p;
}

void project_remove(Ctx *ctx, Project *p) {
    const int i = project_index(ctx, p);
    if (i < 0) return;
    doc_free(p->doc);
    cJSON_Delete(p->groups);
    free(p);
    memmove(ctx->projects + i, ctx->projects + i + 1, sizeof(Project *) * (size_t) (ctx->project_count - i - 1));
    ctx->project_count--;
    if (ctx->focus == i) ctx->focus = ctx->project_count ? 0 : -1;
    else if (ctx->focus > i) ctx->focus--;
}

cJSON *project_json(Ctx *ctx, const Project *p) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", p->id);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddStringToObject(o, "path", p->path);
    cJSON_AddStringToObject(o, "backend", p->kind == PROJ_FILE ? "file" : "editor");
    cJSON_AddBoolToObject(o, "dirty", p->kind == PROJ_FILE ? (p->doc && p->doc->dirty) : p->live_dirty);
    if (p->doc) cJSON_AddNumberToObject(o, "objects", cJSON_GetArraySize(js_get(p->doc->root, "objects")));
    cJSON_AddBoolToObject(o, "focused", project_index(ctx, p) == ctx->focus);
    if (p->kind == PROJ_FILE && p->doc) {
        cJSON_AddNumberToObject(o, "undo", p->doc->undo.count);
        cJSON_AddNumberToObject(o, "redo", p->doc->redo.count);
    }
    return o;
}

// ---------------------------------------------------------------- live projects

int projects_refresh_live(Ctx *ctx, char *err, const size_t errsz) {
    if (ctx->live.mode == 0 || !live_bus_present(&ctx->live)) return 0;
    cJSON *res = live_request(ctx, "list", NULL, 10000, err, errsz);
    if (!res) return -1;
    for (int i = 0; i < ctx->project_count; i++) if (ctx->projects[i]->kind == PROJ_LIVE) ctx->projects[i]->live_seen = 0;
    const cJSON *inst;
    cJSON_ArrayForEach(inst, js_get(res, "instances")) {
        const cJSON *pr;
        cJSON_ArrayForEach(pr, js_get(inst, "projects")) {
            const char *id = js_str(pr, "id", NULL);
            if (!id) continue;
            Project *p = NULL;
            for (int i = 0; i < ctx->project_count; i++) {
                if (ctx->projects[i]->kind == PROJ_LIVE && !strcmp(ctx->projects[i]->id, id)) p = ctx->projects[i];
            }
            if (!p) {
                p = project_push(ctx);
                if (!p) break;
                p->kind = PROJ_LIVE;
                snprintf(p->id, sizeof(p->id), "%s", id);
                if (ctx->focus < 0 && js_bool(pr, "active", 0)) ctx->focus = ctx->project_count - 1;
            }
            p->live_seen = 1;
            snprintf(p->name, sizeof(p->name), "%s", js_str(pr, "name", id));
            snprintf(p->path, sizeof(p->path), "%s", js_str(pr, "path", ""));
            p->live_dirty = js_bool(pr, "dirty", 0);
        }
    }
    cJSON_Delete(res);
    for (int i = ctx->project_count - 1; i >= 0; i--) {
        if (ctx->projects[i]->kind == PROJ_LIVE && !ctx->projects[i]->live_seen) project_remove(ctx, ctx->projects[i]);
    }
    if (ctx->focus < 0 && ctx->project_count) ctx->focus = 0;
    return 0;
}

static void mirror_take_uuids(Doc *doc) {
    for (cJSON *o = doc_objects(doc)->child; o; o = o->next) {
        const cJSON *u = js_get(o, "objUUID");
        o->valueint = cJSON_IsNumber(u) ? (int) u->valuedouble : 0;
        cJSON_DeleteItemFromObjectCaseSensitive(o, "objUUID");
    }
    doc_assign_missing_ids(doc);
}

int project_sync(Ctx *ctx, Project *p, char *err, const size_t errsz) {
    if (p->kind != PROJ_LIVE) return 0;
    cJSON *args = cJSON_CreateObject();
    cJSON *info = live_call(ctx, p->id, "info", args, 10000, err, errsz);
    if (!info) return -1;
    const int revision = js_int(info, "revision", -2);
    p->live_dirty = js_bool(info, "dirty", 0);
    if (js_str(info, "name", NULL)) snprintf(p->name, sizeof(p->name), "%s", js_str(info, "name", ""));
    if (js_str(info, "path", NULL)) snprintf(p->path, sizeof(p->path), "%s", js_str(info, "path", ""));
    cJSON_Delete(info);
    if (p->doc && revision >= 0 && revision == p->live_revision) return 0;
    args = cJSON_CreateObject();
    cJSON_AddTrueToObject(args, "full");
    cJSON *ex = live_call(ctx, p->id, "export", args, 30000, err, errsz);
    if (!ex) return -1;
    cJSON *map = cJSON_DetachItemFromObjectCaseSensitive(ex, "map");
    const int rev = js_int(ex, "revision", revision);
    cJSON_Delete(ex);
    if (!cJSON_IsObject(map)) {
        cJSON_Delete(map);
        snprintf(err, errsz, "editor export returned no map");
        return -1;
    }
    Doc *doc = calloc(1, sizeof(Doc));
    if (!doc) {
        cJSON_Delete(map);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    doc->root = map;
    doc->next_uid = 1;
    doc->editor_form = 1;
    snprintf(doc->path, sizeof(doc->path), "%s", p->path);
    mirror_take_uuids(doc);
    doc_free(p->doc);
    p->doc = doc;
    p->live_revision = rev;
    return 0;
}

static int name_matches(const Project *p, const char *ref) {
    if (!strcmp(p->id, ref)) return 1;
    if (p->path[0]) {
        char resolved[PATH_MAX];
        if (path_resolve(ref, resolved, sizeof(resolved)) == 0 && !strcmp(resolved, p->path)) return 1;
        const char *base = path_basename(p->path);
        if (!strcasecmp(base, ref)) return 1;
        const size_t n = strlen(ref);
        if (!strncasecmp(base, ref, n) && !strcasecmp(base + n, ".json")) return 1;
    }
    return p->name[0] && !strcasecmp(p->name, ref);
}

int project_resolve(Ctx *ctx, const char *ref, Project **out, char *err, const size_t errsz) {
    *out = NULL;
    if (!ref || !ref[0]) {
        if (ctx->focus < 0 && ctx->live.mode && live_bus_present(&ctx->live)) {
            char e2[256];
            projects_refresh_live(ctx, e2, sizeof(e2));
        }
        if (ctx->focus < 0 || ctx->focus >= ctx->project_count) {
            snprintf(err, errsz, "no project is open: call map_open (or map_new / project_open) first");
            return -1;
        }
        *out = ctx->projects[ctx->focus];
    } else {
        for (int pass = 0; pass < 2 && !*out; pass++) {
            for (int i = 0; i < ctx->project_count && !*out; i++) if (name_matches(ctx->projects[i], ref)) *out = ctx->projects[i];
            if (!*out && pass == 0 && ctx->live.mode && live_bus_present(&ctx->live)) {
                char e2[256];
                projects_refresh_live(ctx, e2, sizeof(e2));
            }
        }
        if (!*out) {
            snprintf(err, errsz, "no open project matches '%s' (projects_list shows them)", ref);
            return -1;
        }
    }
    return project_sync(ctx, *out, err, errsz);
}

// ---------------------------------------------------------------- edits

int edit_begin(Ctx *ctx, Project *p, const char *label, Edit *e, char *err, const size_t errsz) {
    memset(e, 0, sizeof(*e));
    e->ctx = ctx;
    e->p = p;
    snprintf(e->label, sizeof(e->label), "%s", label ? label : "edit");
    if (ctx->read_only) {
        snprintf(err, errsz, "the server runs --read-only");
        return -1;
    }
    if (!p->doc) {
        snprintf(err, errsz, "project has no document");
        return -1;
    }
    if (p->kind == PROJ_FILE) {
        e->before = doc_snapshot(p->doc, &e->uids, &e->uid_count);
        e->next_uid = p->doc->next_uid;
        if (!e->before) {
            snprintf(err, errsz, "out of memory");
            return -1;
        }
    } else {
        if (live_ensure(ctx, EDBUS_SCOPE_EDIT, err, errsz) != 0) return -1;
        e->before_tree = cJSON_Duplicate(p->doc->root, 1);
        // keep ids on the duplicate (cJSON_Duplicate copies valueint)
        e->temp_next = -1;
    }
    return 0;
}

int edit_add_object(Edit *e, cJSON *obj, const int index) {
    if (e->p->kind == PROJ_LIVE) obj->valueint = e->temp_next--;
    return doc_add_object(e->p->doc, obj, index);
}

int edit_final_id(const Edit *e, const int id) {
    for (int i = 0; i < e->remap_count; i++) if (e->temp_ids[i] == id) return e->real_ids[i];
    return id;
}

void edit_end(Edit *e) {
    free(e->temp_ids);
    free(e->real_ids);
    e->temp_ids = e->real_ids = NULL;
    e->remap_count = 0;
    cJSON_Delete(e->summary);
    e->summary = NULL;
}

static void edit_free(Edit *e) {
    free(e->before);
    free(e->uids);
    cJSON_Delete(e->before_tree);
    e->before = NULL;
    e->uids = NULL;
    e->before_tree = NULL;
}

void edit_abort(Edit *e) {
    if (e->p && e->p->doc) {
        if (e->p->kind == PROJ_FILE && e->before) {
            doc_restore(e->p->doc, e->before, e->uids, e->uid_count, e->next_uid);
            e->p->doc->revision--; // restore counted a change that never happened
        } else if (e->before_tree) {
            cJSON_Delete(e->p->doc->root);
            e->p->doc->root = e->before_tree;
            e->before_tree = NULL;
        }
    }
    edit_free(e);
    free(e->temp_ids);
    free(e->real_ids);
    e->temp_ids = e->real_ids = NULL;
    cJSON_Delete(e->summary);
    e->summary = NULL;
}

// The editor resolves objects on their own: pooled sizes and palette colours become inline
// s / c / e (objops_portable_copy), so the mirror's "colors" never has to travel.
static cJSON *strip_data(const Doc *doc, const cJSON *obj) {
    cJSON *d = objops_portable_copy(doc, obj);
    if (d) d->valueint = 0;
    return d;
}

static int live_commit(Edit *e, char *err, const size_t errsz) {
    Doc *doc = e->p->doc;
    cJSON *batch = cJSON_CreateObject();
    cJSON_AddStringToObject(batch, "label", e->label);
    cJSON *add = cJSON_AddArrayToObject(batch, "add");
    cJSON *replace = cJSON_AddArrayToObject(batch, "replace");
    cJSON *remove = cJSON_AddArrayToObject(batch, "remove");
    cJSON *map_set = cJSON_AddObjectToObject(batch, "map_set");
    cJSON *map_unset = cJSON_AddArrayToObject(batch, "map_unset");
    const cJSON *before_objects = js_get(e->before_tree, "objects");
    int n_add = 0, n_rep = 0, n_rem = 0, n_keys = 0;
    int *temps = NULL;
    for (const cJSON *o = js_get(doc->root, "objects") ? js_get(doc->root, "objects")->child : NULL; o; o = o->next) {
        const cJSON *old = NULL;
        if (o->valueint > 0) {
            for (const cJSON *b = before_objects ? before_objects->child : NULL; b; b = b->next) {
                if (b->valueint == o->valueint) {
                    old = b;
                    break;
                }
            }
        }
        if (!old) {
            cJSON_AddItemToArray(add, strip_data(doc, o));
            int *t = realloc(temps, sizeof(int) * (size_t) (n_add + 1));
            if (t) {
                temps = t;
                temps[n_add] = o->valueint;
            }
            n_add++;
        } else if (!cJSON_Compare(old, o, 1)) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "uuid", o->valueint);
            cJSON_AddItemToObject(r, "data", strip_data(doc, o));
            cJSON_AddItemToArray(replace, r);
            n_rep++;
        }
    }
    for (const cJSON *b = before_objects ? before_objects->child : NULL; b; b = b->next) {
        int found = 0;
        for (const cJSON *o = js_get(doc->root, "objects") ? js_get(doc->root, "objects")->child : NULL; o && !found; o = o->next)
            found = o->valueint == b->valueint;
        if (!found && b->valueint > 0) {
            cJSON_AddItemToArray(remove, cJSON_CreateNumber(b->valueint));
            n_rem++;
        }
    }
    for (const cJSON *k = doc->root->child; k; k = k->next) {
        if (!strcmp(k->string, "objects") || !strcmp(k->string, "colors") || !strcmp(k->string, "xyz")) continue;
        const cJSON *old = js_get(e->before_tree, k->string);
        if (!old || !cJSON_Compare(old, k, 1)) {
            cJSON_AddItemToObject(map_set, k->string, cJSON_Duplicate(k, 1));
            n_keys++;
        }
    }
    for (const cJSON *k = e->before_tree->child; k; k = k->next) {
        if (!strcmp(k->string, "objects") || !strcmp(k->string, "colors") || !strcmp(k->string, "xyz")) continue;
        if (!js_get(doc->root, k->string)) {
            cJSON_AddItemToArray(map_unset, cJSON_CreateString(k->string));
            n_keys++;
        }
    }
    e->summary = cJSON_CreateObject();
    cJSON_AddNumberToObject(e->summary, "added", n_add);
    cJSON_AddNumberToObject(e->summary, "updated", n_rep);
    cJSON_AddNumberToObject(e->summary, "removed", n_rem);
    cJSON_AddNumberToObject(e->summary, "map_keys", n_keys);
    if (!n_add && !n_rep && !n_rem && !n_keys) {
        cJSON_Delete(batch);
        free(temps);
        e->changed = 0;
        return 0;
    }
    cJSON *res = live_call(e->ctx, e->p->id, "batch", batch, 60000, err, errsz);
    if (!res) {
        free(temps);
        return -1;
    }
    const cJSON *uuids = js_get(res, "uuids");
    e->temp_ids = temps;
    e->real_ids = calloc((size_t) (n_add ? n_add : 1), sizeof(int));
    e->remap_count = n_add;
    for (int i = 0; i < n_add && e->real_ids; i++) {
        const cJSON *u = cJSON_GetArrayItem(uuids, i);
        e->real_ids[i] = cJSON_IsNumber(u) ? (int) u->valuedouble : 0;
    }
    cJSON_Delete(res);
    e->changed = 1;
    e->p->live_revision = -1; // force a fresh mirror
    char e2[256];
    project_sync(e->ctx, e->p, e2, sizeof(e2));
    return 0;
}

int edit_commit(Edit *e, char *err, const size_t errsz) {
    int rc = 0;
    if (e->p->kind == PROJ_FILE) {
        char *after = jsonw_print(e->p->doc->root, JSONW_COMPACT);
        e->changed = !after || strcmp(after, e->before) != 0;
        free(after);
        if (e->changed) {
            doc_history_push(e->p->doc, e->label, e->before, e->uids, e->uid_count, e->next_uid);
            e->before = NULL;
            e->uids = NULL;
            project_refresh_name(e->p);
        }
    } else {
        rc = live_commit(e, err, errsz);
        if (rc != 0) {
            // put the mirror back; the editor did not take the batch
            cJSON_Delete(e->p->doc->root);
            e->p->doc->root = e->before_tree;
            e->before_tree = NULL;
        }
    }
    edit_free(e);
    return rc;
}
