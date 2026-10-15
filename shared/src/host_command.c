// The server_bin command line of the Host Game flow (shared/include/host_command.h).
#include <host_command.h>
#include <game_config.h>
#include <modes.h>
#include <shared.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_COMMAND_DEFAULT_PORT 21015 // NET_DEFAULT_PORT (server/src/main.c --port)
#define HOST_COMMAND_CLASS_COUNT 14     // the Krunker classes a Host Game offers (rules_classes())

static const char *const k_host_type_names[HOST_COMMAND_HOST_TYPES] = {"custom", "comp", "rent", "zombies", "raid"};

// ------------------------------------------------------------------ output

typedef struct {
    char *out;
    size_t size;
    size_t len;
    int error;        // 0 ok, HOST_COMMAND_ERR_SIZE, HOST_COMMAND_ERR_GAME
    size_t game_bytes; // the game settings written so far (the server's game request is 4096 bytes)
} CmdBuf;

static void cb_put(CmdBuf *b, const char *text, const size_t n) {
    if (b->error) return;
    if (b->len + n + 1 > b->size) {
        b->error = HOST_COMMAND_ERR_SIZE;
        return;
    }
    memcpy(b->out + b->len, text, n);
    b->len += n;
    b->out[b->len] = 0;
}

static void cb_str(CmdBuf *b, const char *text) { cb_put(b, text, strlen(text)); }

// "<word>", after a separating space unless it starts the line; quoted when it is not a plain word
static void cb_word(CmdBuf *b, const char *text) {
    char quoted[HOST_COMMAND_WORD_MAX];
    if (host_command_quote(text, quoted, sizeof(quoted)) < 0) {
        b->error = HOST_COMMAND_ERR_SIZE;
        return;
    }
    if (b->len) cb_str(b, " ");
    cb_str(b, quoted);
}

// " --set gameserver.<...>=<value>": the spec is one word (quoted when it has spaces or JSON)
static void cb_set(CmdBuf *b, const char *spec) {
    cb_str(b, " --set");
    cb_word(b, spec);
}

// A game setting: its text counts against the server's game request (4096 bytes, JSON).
static void cb_game_set(CmdBuf *b, const char *key, const char *value) {
    const size_t bytes = strlen(key) + strlen(value) + 4;
    if (b->game_bytes + bytes > HOST_COMMAND_GAME_BYTES) {
        b->error = HOST_COMMAND_ERR_GAME;
        return;
    }
    b->game_bytes += bytes;
    char spec[HOST_COMMAND_WORD_MAX];
    snprintf(spec, sizeof(spec), "gameserver.game.%s=%s", key, value);
    cb_set(b, spec);
}

// ------------------------------------------------------------------ values

// A number as a flag / JSON value: whole numbers bare, others with 7 significant digits.
static void num_text(char *out, const size_t size, const double v) {
    if (floor(v) == v && fabs(v) < 1e15) snprintf(out, size, "%.0f", v);
    else snprintf(out, size, "%.7g", v);
}

// The default of an option on this host type (def_c on comp hosts, as the Host Game rows show it).
static double option_default(const GameConfigOption *o, const int comp) {
    return comp && (o->flags & GC_HAS_DEF_C) ? (double) o->def_c : (double) o->def;
}

// The request's serverConfig row `o` as a value; 0 when it is missing or equals the default.
static int row_value(const GameConfigOption *o, const cJSON *item, const int comp, char *value, const size_t size) {
    if (!item) return 0;
    if (o->kind == GC_BOOL) {
        const int on = cJSON_IsTrue(item);
        if (on == (option_default(o, comp) != 0.0)) return 0;
        snprintf(value, size, "%s", on ? "true" : "false");
        return 1;
    }
    if (o->kind == GC_INPUT) {
        if (!cJSON_IsString(item) || !item->valuestring[0]) return 0;
        if (o->def_str && !strcmp(item->valuestring, o->def_str)) return 0;
        snprintf(value, size, "%s", item->valuestring);
        return 1;
    }
    if (!cJSON_IsNumber(item)) return 0;
    const double def = option_default(o, comp);
    if (fabs(item->valuedouble - def) <= 1e-6 * fmax(1.0, fabs(def))) return 0;
    num_text(value, size, item->valuedouble);
    return 1;
}

// The class list is the default when it names every Krunker class.
static int classes_is_all(const cJSON *classes) {
    if (!cJSON_IsArray(classes)) return 1;
    int seen[HOST_COMMAND_CLASS_COUNT] = {0};
    int count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, classes) {
        if (!cJSON_IsNumber(item)) continue;
        const int id = item->valueint;
        if (id >= 0 && id < HOST_COMMAND_CLASS_COUNT && !seen[id]) {
            seen[id] = 1;
            count++;
        }
    }
    return count >= HOST_COMMAND_CLASS_COUNT;
}

// The mode ids of the request: one mode is --mode <alias>, several are gameserver.modes=[...].
static void write_modes(CmdBuf *b, const cJSON *modes) {
    if (!cJSON_IsArray(modes)) return;
    cJSON *aliases = cJSON_CreateArray();
    if (!aliases) {
        b->error = HOST_COMMAND_ERR_SIZE;
        return;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, modes) {
        if (!cJSON_IsNumber(item)) continue;
        const GameModeConfig *mode = mode_config((int) item->valuedouble);
        if (mode && mode->alias) cJSON_AddItemToArray(aliases, cJSON_CreateString(mode->alias));
    }
    const int count = cJSON_GetArraySize(aliases);
    if (count == 1) {
        cb_str(b, " --mode");
        cb_word(b, cJSON_GetArrayItem(aliases, 0)->valuestring);
    } else if (count > 1) {
        char *text = cJSON_PrintUnformatted(aliases);
        char spec[HOST_COMMAND_WORD_MAX];
        if (text) snprintf(spec, sizeof(spec), "gameserver.modes=%s", text);
        free(text);
        if (text) cb_set(b, spec);
        else b->error = HOST_COMMAND_ERR_SIZE;
    }
    cJSON_Delete(aliases);
}

// classes / weps / cls / prks: their JSON as one --set value (the server's game request keeps them)
static void write_override(CmdBuf *b, const char *key, const cJSON *item) {
    char *text = cJSON_PrintUnformatted(item);
    if (!text) {
        b->error = HOST_COMMAND_ERR_SIZE;
        return;
    }
    if (strlen(text) + strlen(key) + 4 > HOST_COMMAND_WORD_MAX) {
        cJSON_free(text);
        b->error = HOST_COMMAND_ERR_GAME;
        return;
    }
    cb_game_set(b, key, text);
    cJSON_free(text);
}

// ------------------------------------------------------------------ the line

int host_command_build(const HostCommand *spec, const cJSON *request, char *out, const size_t size) {
    if (!spec || !out || !size) return HOST_COMMAND_ERR_SIZE;
    CmdBuf b = {out, size, 0, 0, 0};
    out[0] = 0;
    const int host_type = spec->host_type;
    const int comp = host_type == GAME_HOST_COMP;
    const unsigned int flags = host_type >= 0 ? game_host_type_flags(host_type) : 0;
    const int choices = !(flags & (GAME_HOST_FLAG_COMP | GAME_HOST_FLAG_RESTRICT));

    cb_word(&b, spec->program && spec->program[0] ? spec->program : "server_bin");
    if (spec->map_file && spec->map_file[0]) cb_word(&b, spec->map_file);
    if (spec->port > 0 && spec->port != HOST_COMMAND_DEFAULT_PORT) {
        char text[16];
        snprintf(text, sizeof(text), "%d", spec->port);
        cb_str(&b, " --port");
        cb_word(&b, text);
    }
    if (spec->hostname && spec->hostname[0]) {
        cb_str(&b, " --hostname");
        cb_word(&b, spec->hostname);
    }
    if (spec->password && spec->password[0]) {
        cb_str(&b, " --password");
        cb_word(&b, spec->password);
    }
    if (spec->public_host && spec->public_host[0]) {
        cb_str(&b, " --public-host");
        cb_word(&b, spec->public_host);
    }
    if (spec->public_port > 0) {
        char text[16];
        snprintf(text, sizeof(text), "%d", spec->public_port);
        cb_str(&b, " --public-port");
        cb_word(&b, text);
    }
    // the map: a positional file wins; a catalogue key is the rotation's one map
    if (!(spec->map_file && spec->map_file[0]) && spec->map_key && spec->map_key[0]) {
        char text[HOST_COMMAND_WORD_MAX];
        snprintf(text, sizeof(text), "gameserver.maps=%s", spec->map_key);
        cb_set(&b, text);
    }
    if (host_type >= 0 && host_type < HOST_COMMAND_HOST_TYPES) {
        char text[64];
        snprintf(text, sizeof(text), "gameserver.host_type=%s", k_host_type_names[host_type]);
        cb_set(&b, text);
    }
    if (request) {
        if (choices) write_modes(&b, cJSON_GetObjectItemCaseSensitive(request, "modes"));

        // the serverConfig rows that differ from their defaults (the Settings tab)
        size_t count = 0;
        const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
        for (size_t k = 0; k < count && !b.error; k++) {
            const GameConfigOption *o = &options[k];
            char value[256];
            if (!row_value(o, cJSON_GetObjectItemCaseSensitive(request, o->var), comp, value, sizeof(value))) continue;
            if (!strcmp(o->var, "maxPlayers")) { // the lobby size: the server's --max-players as well
                cb_str(&b, " --max-players");
                cb_word(&b, value);
            }
            cb_game_set(&b, o->var, value);
        }

        if (choices && !b.error) {
            const cJSON *classes = cJSON_GetObjectItemCaseSensitive(request, "classes");
            if (cJSON_IsArray(classes) && !classes_is_all(classes)) write_override(&b, "classes", classes);
            const char *overrides[] = {"weps", "cls", "prks"};
            for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]) && !b.error; i++) {
                const cJSON *item = cJSON_GetObjectItemCaseSensitive(request, overrides[i]);
                if (cJSON_IsObject(item) && cJSON_GetArraySize(item)) write_override(&b, overrides[i], item);
            }
        }
    }
    if (spec->headless) cb_str(&b, " --headless");
    return b.error ? b.error : (int) b.len;
}

int host_command_quote(const char *value, char *out, const size_t size) {
    if (!value || !out || !size) return HOST_COMMAND_ERR_SIZE;
    int plain = value[0] != 0;
    for (const char *p = value; *p && plain; p++) {
        const char c = *p;
        plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("_-./:=,+@%", c) != NULL;
    }
    if (plain) {
        if (strlen(value) + 1 > size) return HOST_COMMAND_ERR_SIZE;
        strcpy(out, value);
        return (int) strlen(out);
    }
    size_t n = 0;
    if (size < 3) return HOST_COMMAND_ERR_SIZE;
    out[n++] = '\'';
    for (const char *p = value; *p; p++) {
        // ' closes the quotes, \' is the quote itself, ' reopens them
        const size_t need = *p == '\'' ? 4 : 1;
        if (n + need + 2 > size) return HOST_COMMAND_ERR_SIZE;
        if (*p == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *p;
        }
    }
    if (n + 2 > size) return HOST_COMMAND_ERR_SIZE;
    out[n++] = '\'';
    out[n] = 0;
    return (int) n;
}
