/*
 * knsvc/json.h - small cJSON conveniences used across the services.
 * Getters never fail: they return `def` when the key is absent or mistyped.
 */
#ifndef KNSVC_JSON_H
#define KNSVC_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

const char *kn_json_str(const cJSON *obj, const char *key, const char *def);
int64_t kn_json_int(const cJSON *obj, const char *key, int64_t def);
double kn_json_num(const cJSON *obj, const char *key, double def);
int kn_json_bool(const cJSON *obj, const char *key, int def);   /* accepts true/false and numbers */
/* Serialize compactly (cJSON_PrintUnformatted); caller frees with free(). */
char *kn_json_print(const cJSON *json);
/* Parse len bytes (not necessarily NUL-terminated). NULL on error. */
cJSON *kn_json_parse(const char *data, size_t len);

#endif
