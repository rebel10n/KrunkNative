#pragma once
// Krunker v7.1.4 custom map validation (doc/v7/map-logic.md §1.1, §1.5.2 "Limits"):
// customGameConfig.js validateMap (:152-368) plus the prepareGame checks (game.js:606-678:
// unknown prefab ids / texture indices, the "(" ")" replacement).
//
// The server's custom-map ingest (host / offline sandbox) and kn_editor_mcp's map_validate tool use
// it. Dependency-light: cJSON + the prefab / trigger tables (prefabs.h, trigger_v7_data.h); no Game.
//
// v7 returns the first error string. map_validate() reports every problem it finds (errors in v7's
// check order, the first one being exactly what v7 would return) plus warnings that v7 would
// not reject but that load differently than authored (string prefab ids, string textures, ...).
#include <stddef.h>
#include <cJSON.h>

typedef enum {
    MAP_ISSUE_ERROR,   // v7 rejects the map (validateMap / prepareGame)
    MAP_ISSUE_WARNING, // v7 accepts it, but the value is ignored, clamped or reinterpreted
} MapIssueSeverity;

#define MAP_ISSUE_CODE_SIZE 32
#define MAP_ISSUE_MESSAGE_SIZE 160

typedef struct {
    MapIssueSeverity severity;
    int object;                           // index into "objects", -1 = map level
    int action;                           // index into the object's "tas", -1 none
    char code[MAP_ISSUE_CODE_SIZE];       // stable id: "object_id", "spawn_count", ...
    char message[MAP_ISSUE_MESSAGE_SIZE]; // v7's message for errors ("Invalid Object.")
} MapIssue;

typedef struct {
    unsigned char featured;     // featured creator / accType > 1: objectLimitF 7500, objectColLimitF 6000
    unsigned char skip_header;  // validateMap(map, featured, true): skip the name / tris / spawns / welMsg / gui checks
    unsigned char no_warnings;  // errors only
    unsigned char first_only;   // stop at the first error (v7's behaviour)
} MapValidateOptions;

// v7.1.4 limits (config.js:895-944) the validation applies.
#define MAP_V7_NAME_MAX 16
#define MAP_V7_WELCOME_MAX 70
#define MAP_V7_OBJECT_LIMIT 5000
#define MAP_V7_OBJECT_LIMIT_FEATURED 7500
#define MAP_V7_COLLIDABLE_LIMIT 4500
#define MAP_V7_COLLIDABLE_LIMIT_FEATURED 6000
#define MAP_V7_SPAWN_LIMIT 20
#define MAP_V7_SIGN_LIMIT 100
#define MAP_V7_SIGN_TEXT_LIMIT 1500
#define MAP_V7_SIGN_FONT_MIN 1
#define MAP_V7_SIGN_FONT_MAX 240
#define MAP_V7_WHEEL_LIMIT 10
#define MAP_V7_LEADERBOARD_LIMIT 2
#define MAP_V7_AI_LIMIT 40
#define MAP_V7_BOT_LIMIT 16
#define MAP_V7_ANCHOR_LIMIT 1000
#define MAP_V7_INTERFACE_LIMIT 30   // objects per interface id
#define MAP_V7_TRIANGLE_LIMIT 5000  // tris.length / 9
#define MAP_V7_GUI_LIMIT 80
#define MAP_V7_POSITION_RANGE 500000.0

// Validates a parsed map document (not modified). Writes up to `max` issues; returns the total
// number found (errors + warnings, may exceed max). *errors (optional) = the number of errors.
size_t map_validate(const cJSON *map, const MapValidateOptions *options, MapIssue *issues, size_t max, size_t *errors);

// v7's answer for a custom map: NULL when it is accepted, else the first error message (static
// storage, valid until the next call). Same as map_validate(first_only, no_warnings).
const char *map_validate_error(const cJSON *map, int featured);

// prepareGame: mapData.replace(/\(/g, "（").replace(/\)/g, "）") on the raw JSON text. Returns a
// malloc'd copy (the caller frees it), NULL on allocation failure.
char *map_prepare_custom_text(const char *json_text);

// The same replacement on an already parsed document (every string value and key), for maps that
// arrive parsed (the server's /api/map-data answer). Returns how many strings changed.
int map_prepare_custom_json(cJSON *map);

// prepareGame + validateMap of a custom map's JSON text: replaces the parens, parses, validates.
// On success returns the parsed document (the caller owns it) and *error = NULL; otherwise NULL
// and *error = v7's message ("Map error occured." for unparsable text).
cJSON *map_prepare_custom(const char *json_text, int featured, const char **error);
