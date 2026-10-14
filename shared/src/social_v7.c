// player-viewer WP-E part 2b: store sprays, spray choice and limits, prop-hunt taunts
// (shared/include/social_v7.h). Krunker v7.1.4 src/data/players.js:2007-2072 (spray),
// :4552-4557 (taunt), src/config.js:2252-2281 and :2654-2656.
#include <social_v7.h>
#include <account.h>
#include <modes.h>
#include <cJSON.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- store sprays

static struct {
    int loaded;
    StoreSpray *items; // sorted by index
    int count;
} g_store_sprays;

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
    cJSON_ArrayForEach(item, json) {
        const int at = index++;
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
    if (!game || !player || !player->active || index < 0 || index >= TAUNT_COUNT || !game_is_authority(game)) return 0;
    // KrunkNative: the server takes taunts from the prop team only (v7's client sends them only
    // then; its server plays any player's "prpt").
    if (!game_player_is_prop(game, player)) return 0;
    // `!lastTaunt || game.now - lastTaunt >= 0` with lastTaunt = now + tm
    if (player->taunt_ready_at > 0.0f && game->now < player->taunt_ready_at) return 0;
    player->taunt_ready_at = game->now + g_taunts[index].time;
    const float pitch = 0.9f + 0.1f * (float) ldexp((double) pcg32_random(), -32); // UTILS.randFloat(0.9, 1)
    game_play_sound(game, g_taunts[index].sound, player->uid, player->position, TAUNT_VOLUME, pitch);
    return 1;
}
