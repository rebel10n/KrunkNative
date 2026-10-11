/*
 * knsvc/config.h - service configuration: config/config.json + KN_* env + CLI flags.
 *
 * Lookup order for every getter (first hit wins):
 *   1. CLI:  --set <path>=<value>  or  --<path>=<value>  where <path> contains a dot
 *            (e.g. --brain.port=9001, --set gameserver.region=eu)
 *   2. env:  KN_<PATH> with '.' -> '_' and upper-cased (brain.public_url -> KN_BRAIN_PUBLIC_URL,
 *            database.password -> KN_DATABASE_PASSWORD)
 *   3. file: the JSON document (dot path, e.g. "gameserver.lobby.max_players")
 *   4. the `def` argument
 *
 * File location: --config=<file> / --config <file>, else $KN_CONFIG, else the first
 * config/config.json found in the cwd or up to 4 parent directories (so binaries
 * also work when started from a build dir). A missing file is not an error (all
 * defaults); an unreadable/invalid one is.
 *
 * Override values are parsed as JSON when they look like JSON (numbers, true/false,
 * null, [..], {..}, "..."), otherwise taken as strings. Overrides are also merged
 * into the document at load time, so kn_config_get() subtrees reflect them
 * whenever the path already exists in the file (or was given on the CLI).
 *
 * Paths: dot separated keys; array elements by index ("brain.regions.0.id").
 * Arguments the loader consumed (--config, --set, --a.b=..) are reported by
 * kn_config_arg_consumed() so programs can parse the rest themselves.
 */
#ifndef KNSVC_CONFIG_H
#define KNSVC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct kn_config kn_config;

/* path: explicit file (NULL -> search as above). argc/argv may be 0/NULL.
 * Returns NULL on error with a message in err. */
kn_config *kn_config_load(const char *path, int argc, char **argv, char *err, size_t errlen);
/* Empty config from a JSON string (tests, embedded defaults). */
kn_config *kn_config_from_string(const char *json, char *err, size_t errlen);
void kn_config_free(kn_config *cfg);

/* File actually loaded ("" when none), and the repository root guessed from it
 * (the parent of the directory holding config.json, or the cwd). */
const char *kn_config_path(const kn_config *cfg);
const char *kn_config_root(const kn_config *cfg);
/* Resolve a relative path against kn_config_root(); absolute paths are copied.
 * Returns out, or NULL when it does not fit. */
char *kn_config_resolve_path(const kn_config *cfg, const char *rel, char *out, size_t cap);

int kn_config_has(const kn_config *cfg, const char *path);
const char *kn_config_str(const kn_config *cfg, const char *path, const char *def);
int64_t kn_config_int(const kn_config *cfg, const char *path, int64_t def);
double kn_config_num(const kn_config *cfg, const char *path, double def);
int kn_config_bool(const kn_config *cfg, const char *path, int def); /* true/false/1/0/yes/no/on/off */
/* Raw JSON node (object/array/scalar) after override merging; NULL if absent.
 * Owned by cfg. */
const cJSON *kn_config_get(const kn_config *cfg, const char *path);
/* Array of strings (JSON array, or a comma separated string/override).
 * Fills out[] with pointers owned by cfg; returns the element count (may exceed max). */
size_t kn_config_str_list(const kn_config *cfg, const char *path, const char **out, size_t max);

/* Programmatic override (highest priority, as if given on the CLI). */
int kn_config_set(kn_config *cfg, const char *path, const char *value);

/* 1 if argv[i] was consumed by kn_config_load (and argv[i+1] for "--set x"/"--config x"). */
int kn_config_arg_consumed(const kn_config *cfg, int i);

/* JSON text of the effective document with every key containing "password",
 * "secret", "token" or "key" replaced by "***". Caller frees with free(). */
char *kn_config_dump_redacted(const kn_config *cfg);

#endif
