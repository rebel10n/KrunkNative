// player-viewer WP-E part 2b: store sprays, spray choice and limits, prop-hunt taunts
// (shared/include/social_v7.h). Krunker v7.1.4 src/data/players.js:2007-2072 (spray),
// :4552-4557 (taunt), src/config.js:2252-2281 and :2654-2656.
#include <social_v7.h>
#include <account.h>
#include <modes.h>
#include <cJSON.h>
#include <pcg_basic.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- store sprays

// What getSkins (deob-10.0.7.js:217752-217795) reads of each store item, one byte per store index
enum {
    ITEM_FREE = 1 << 0,       // free && !blocked (:217752)
    ITEM_KPD = 1 << 1,        // kpd: a moderator's or a deputy's
    ITEM_SKIN_MAKER = 1 << 2, // !creator && seas >= 6 && !blocked: a skin maker's
};
#define ITEM_NAME_STREAMER 0x80000000u // StoreItemName.at: a streamers entry (else a creators one)

// One item.creators / item.streamers entry: its item and the name's offset in the pool (| ITEM_NAME_STREAMER)
typedef struct {
    int32_t item;
    uint32_t at;
} StoreItemName;

static struct {
    int loaded;
    StoreSpray *items; // sorted by index
    int count;
    unsigned char *item_flags; // ITEM_*, one per store index
    int item_count;
    StoreItemName *names; // every creators / streamers name, in store index order
    int name_count;
    char *name_pool; // the names, NUL-separated
} g_store_sprays;

// The creators / streamers names of the store items (each as written: v10 compares them with the lower-cased
// account name / Twitch)
static void store_names_load(const cJSON *json) {
    static const char *const keys[2] = {"creators", "streamers"};
    size_t bytes = 0;
    int n = 0;
    const cJSON *item, *e;
    cJSON_ArrayForEach(item, json) {
        for (int k = 0; k < 2; k++) {
            cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(item, keys[k])) {
                if (cJSON_IsString(e) && e->valuestring[0]) n++, bytes += strlen(e->valuestring) + 1;
            }
        }
    }
    if (!n) return;
    g_store_sprays.names = malloc((size_t) n * sizeof(StoreItemName));
    g_store_sprays.name_pool = malloc(bytes);
    if (!g_store_sprays.names || !g_store_sprays.name_pool) {
        free(g_store_sprays.names), free(g_store_sprays.name_pool);
        g_store_sprays.names = NULL, g_store_sprays.name_pool = NULL;
        return;
    }
    size_t at = 0;
    int index = 0;
    cJSON_ArrayForEach(item, json) {
        for (int k = 0; k < 2; k++) {
            cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(item, keys[k])) {
                if (!cJSON_IsString(e) || !e->valuestring[0]) continue;
                const size_t len = strlen(e->valuestring) + 1;
                memcpy(g_store_sprays.name_pool + at, e->valuestring, len);
                g_store_sprays.names[g_store_sprays.name_count++] = (StoreItemName) {index, (uint32_t) at | (k ? ITEM_NAME_STREAMER : 0)};
                at += len;
            }
        }
        index++;
    }
}

static void store_sprays_load(void) {
    if (g_store_sprays.loaded) return;
    g_store_sprays.loaded = 1;
    char *path = concat(assets_path(), "skins.json");
    unsigned char *buffer = NULL;
    size_t length = 0;
    if (!path || !ensure_asset_file(path) || read_file(path, &buffer, &length)) { // web: fetched on demand
        fprintf(stderr, "sprays: cannot read %s (every spray is the default %d)\n", path ? path : "skins.json", SPRAY_DEFAULT_INDEX);
        free(path);
        return;
    }
    free(path);
    cJSON *json = cJSON_ParseWithLength((const char *) buffer, length);
    free(buffer);
    if (!cJSON_IsArray(json)) {
        cJSON_Delete(json);
        return;
    }
    int n = 0, index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        n += cJSON_IsNumber(type) && type->valueint == SPRAY_ITEM_TYPE;
    }
    g_store_sprays.items = calloc((size_t) (n > 0 ? n : 1), sizeof(StoreSpray));
    if (!g_store_sprays.items) {
        cJSON_Delete(json);
        return;
    }
    const int total = cJSON_GetArraySize(json);
    g_store_sprays.item_flags = calloc((size_t) (total > 0 ? total : 1), 1);
    g_store_sprays.item_count = g_store_sprays.item_flags ? total : 0;
    store_names_load(json);
    cJSON_ArrayForEach(item, json) {
        const int at = index++;
        if (at < g_store_sprays.item_count) {
            const int blocked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "blocked"));
            const cJSON *creator = cJSON_GetObjectItemCaseSensitive(item, "creator");
            const cJSON *seas = cJSON_GetObjectItemCaseSensitive(item, "seas");
            const int has_creator = cJSON_IsString(creator) ? creator->valuestring[0] != 0
                                    : cJSON_IsNumber(creator) ? creator->valuedouble != 0
                                                              : cJSON_IsTrue(creator);
            g_store_sprays.item_flags[at] =
                (unsigned char) ((cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "free")) && !blocked ? ITEM_FREE : 0) |
                                 (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "kpd")) ? ITEM_KPD : 0) |
                                 (!has_creator && cJSON_IsNumber(seas) && seas->valuedouble >= 6 && !blocked ? ITEM_SKIN_MAKER : 0));
        }
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsNumber(type) || type->valueint != SPRAY_ITEM_TYPE) continue;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(item, "sclW");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(item, "sclH");
        const cJSON *snd = cJSON_GetObjectItemCaseSensitive(item, "spraySnd");
        g_store_sprays.items[g_store_sprays.count++] = (StoreSpray) {
            .index = at,
            .id = cJSON_IsNumber(id) ? id->valueint : 0,
            // `sclW || 1`: 0 / absent is 1
            .scl_w = cJSON_IsNumber(w) && w->valuedouble ? (float) w->valuedouble : 1.0f,
            .scl_h = cJSON_IsNumber(h) && h->valuedouble ? (float) h->valuedouble : 1.0f,
            .free = (unsigned char) cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "free")),
            .spray_snd = cJSON_IsNumber(snd) ? snd->valueint : 0,
        };
    }
    cJSON_Delete(json);
}

const StoreSpray *store_spray(const int index) {
    store_sprays_load();
    int lo = 0, hi = g_store_sprays.count - 1;
    while (lo <= hi) { // items are in index order
        const int mid = (lo + hi) / 2;
        const int at = g_store_sprays.items[mid].index;
        if (at == index) return &g_store_sprays.items[mid];
        if (at < index) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int store_item_free(const int index) {
    store_sprays_load();
    return index >= 0 && index < g_store_sprays.item_count && (g_store_sprays.item_flags[index] & ITEM_FREE);
}

int store_item_exists(const int index) {
    store_sprays_load();
    return index >= 0 && index < g_store_sprays.item_count;
}

// s lower-cased into out (JS toLowerCase on the ASCII letters account and Twitch names use)
static void lower_copy(char *out, const size_t size, const char *s) {
    size_t n = 0;
    for (; s && s[n] && n + 1 < size; n++) out[n] = (char) tolower((unsigned char) s[n]);
    out[n] = 0;
}

int store_item_owner_match(const int index, const StoreOwner *owner) {
    store_sprays_load();
    if (!owner || index < 0 || index >= g_store_sprays.item_count) return 0;
    const unsigned char flags = g_store_sprays.item_flags[index];
    if ((flags & ITEM_KPD) && owner->kpd_officer) return 1;        // item.kpd && (acc.isMod || acc.isDeputy)
    if ((flags & ITEM_SKIN_MAKER) && owner->skin_maker) return 1; // !item.creator && item.seas >= 6 && acc.isSkinMaker && !blocked
    int lo = 0, hi = g_store_sprays.name_count; // the first name of the item
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (g_store_sprays.names[mid].item < index) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g_store_sprays.name_count || g_store_sprays.names[lo].item != index) return 0;
    char name[64], twitch[64];
    lower_copy(name, sizeof(name), owner->name);
    lower_copy(twitch, sizeof(twitch), owner->twitch);
    for (int i = lo; i < g_store_sprays.name_count && g_store_sprays.names[i].item == index; i++) {
        const uint32_t at = g_store_sprays.names[i].at;
        const char *entry = g_store_sprays.name_pool + (at & ~ITEM_NAME_STREAMER);
        // item.creators.indexOf(acc.name.toLowerCase()), item.streamers.indexOf(acc.twitch.toLowerCase()) (acc.twitch set)
        const char *mine = at & ITEM_NAME_STREAMER ? twitch : name;
        if (mine[0] && !strcmp(entry, mine)) return 1;
    }
    return 0;
}

int store_spray_from_legacy(const int index) {
    return index >= 0 && index < SPRAY_LEGACY_COUNT ? SPRAY_DEFAULT_INDEX + index : index;
}

int store_spray_to_legacy(const int index) {
    return index >= SPRAY_DEFAULT_INDEX && index < SPRAY_DEFAULT_INDEX + SPRAY_LEGACY_COUNT ? index - SPRAY_DEFAULT_INDEX : index;
}

// ---------------------------------------------------------------- spray choice and limit

int game_spray_pick(const Game *game, const Player *player, const int wheel_index) {
    AccountInfo account;
    const int sandbox = game && game->is_sandbox;
    const int premium = game_account_info(game, player, &account) && account.has_account && account.premium;
    // (isSandbox || account && account.premiumT > 0) && index != null || (index = player.sprayIndex)
    const int index = (sandbox || premium) && wheel_index >= 0 ? wheel_index : store_spray_from_legacy(player ? player->spray_index : 0);
    // skin && type == 4 && (skin.free || isSandbox) || type == 4 && canUseSkin(index, ...) || (index = 2482)
    const StoreSpray *spray = store_spray(index);
    if (!spray) return SPRAY_DEFAULT_INDEX;
    if (spray->free || sandbox) return index;
    return game && game->hooks.can_use_skin && game->hooks.can_use_skin(game, player, index) ? index : SPRAY_DEFAULT_INDEX;
}

int game_spray_limit(const Game *game, const Player *player) {
    AccountInfo account;
    int limit = game && game->is_sandbox ? SPRAY_MAX_FEATURED : SPRAY_MAX_DEFAULT;
    if (game_account_info(game, player, &account) && account.premium) limit = account.verified ? SPRAY_MAX_FEATURED : SPRAY_MAX_PREMIUM;
    return limit; // player.unlimitedSprays (10000) is never set in v7.1.4
}

// ---------------------------------------------------------------- taunts

// config.taunts: {nm, id (networkSounds index), tm}
const TauntConfig g_taunts[TAUNT_COUNT] = {
    {"taunt_0", 0.5f}, {"taunt_1", 1.0f}, {"taunt_2", 2.3f}, {"taunt_3", 3.2f}, {"taunt_4", 3.2f}, {"taunt_5", 1.0f},
};

int game_player_is_prop(const Game *game, const Player *player) {
    if (!game || !game->mode || !player || player->team < 1 || player->team > 2) return 0;
    const char *option = game->mode->config.team_options[player->team - 1];
    return option && !strcmp(option, "Prop");
}

int game_player_taunt(Game *game, Player *player, const int index) {
    return game_player_taunt_ex(game, player, index, 0);
}

int game_player_taunt_ex(Game *game, Player *player, const int index, const int force) {
    if (!game || !player || !player->active || index < 0 || index >= TAUNT_COUNT || !game_is_authority(game)) return 0;
    // KrunkNative: the server takes taunts from the prop team only (v7's client sends them only
    // then; its server plays any player's "prpt").
    if (!game_player_is_prop(game, player)) return 0;
    // `(force || !lastTaunt || game.now - lastTaunt >= 0)` with lastTaunt = now + tm (players.taunt :173947)
    if (!force && player->taunt_ready_at > 0.0f && game->now < player->taunt_ready_at) return 0;
    player->taunt_ready_at = game->now + g_taunts[index].time;
    const float pitch = 0.9f + 0.1f * (float) ldexp((double) pcg32_random(), -32); // UTILS.randFloat(0.9, 1)
    game_play_sound(game, g_taunts[index].sound, player->uid, player->position, TAUNT_VOLUME, pitch);
    return 1;
}
