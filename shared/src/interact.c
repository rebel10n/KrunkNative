// Interaction ("int"): Krunker v3.1.1 players.interact (src/data/players.js) and the
// terminal part of Player's interact checks. Map terminals (objects.js addTerminal) are
// boxes centred on (x, y + h/2, z) with half extents s/2 and tRadius 7; a player in range
// that presses the interact key gets "ot" (NET_PACKET_TERMINAL) and the client opens
// #terminalHolder. Gates ("gte") and deposit boxes ("bnk") share the radius; their state
// lives in shared/src/triggers.c. Weapon pickups are not ported yet.
#include <shared.h>
#include <modes.h>
#include <stdio.h>

int player_collides(Player*, const Object*, float pad); // shared/src/player.c (collidesE)

// Player.collides(other, pad) for a box (the borderH pad only applies to borders, which
// terminals never are).
static int collides_padded(const Player *p, const Object *o, const float pad) {
    const float hx = o->scale.x * 0.5f + pad, hz = o->scale.z * 0.5f + pad, hy = o->scale.y * 0.5f;
    const float cx = o->position.x, cy = o->position.y + o->scale.y * 0.5f, cz = o->position.z;
    return p->position.x - p->scale < cx + hx && p->position.x + p->scale > cx - hx &&
           p->position.z - p->scale < cz + hz && p->position.z + p->scale > cz - hz &&
           p->position.y <= cy + hy && p->position.y + p->height >= cy - hy;
}

// (!F.team || !game.mode || !game.mode.teams || F.team == player.team)
static int team_allows(const Game *game, const Player *p, const Object *o) {
    return !o->team || !game->mode || !game->mode->config.teams || (int) o->team == p->team;
}

static int terminal_usable(const Game *game, const Player *p, const Object *o, const int prompt) {
    return o && o->terminal && o->active && (!prompt || !o->no_msg) && collides_padded(p, o, GAME_TERMINAL_RADIUS) &&
           team_allows(game, p, o);
}

// players.interact gates: an open gate needs `closeable` to close; the price is score
// (method 0 pays it, 1 only needs it) or kills (method 2).
static int gate_usable(const Game *game, const Player *p, const Object *o) {
    if (!o->gate || !(o->active || o->closeable) || !collides_padded(p, o, GAME_INTERACT_RADIUS) || !team_allows(game, p, o)) return 0;
    return o->gate_method < 2 ? p->score >= (int) o->score_points : (int) p->kills >= (int) o->score_points;
}

static void use_gate(Game *game, Player *player, Object *gate) {
    if (gate->score_points) {
        NetMedalPacket medal = {.score = 0}; // IO.send(player.id, "am", [method ? "Unlocked" : "Purchased", null])
        snprintf(medal.text, sizeof(medal.text), "%s", gate->gate_method ? "Unlocked" : "Purchased");
        game_send(game, player->uid, NET_PACKET_MEDAL, &medal, sizeof(medal));
        if (!gate->gate_method) player_score(player, -(int) gate->score_points, 0);
    }
    game_set_gate(game, gate, !gate->active);
}

// Deposit (interactKey) or withdraw (interactSecKey) depositAmnt / withdrawAmnt (0 = all).
static void use_bank(Game *game, Player *player, Object *bank, const int withdraw) {
    if (bank->deposited != 0 && withdraw) {
        if (bank->withdraw_amount && bank->deposited > bank->withdraw_amount) {
            bank->deposited -= bank->withdraw_amount;
            player_score(player, bank->withdraw_amount, 0);
        } else {
            player_score(player, bank->deposited, 0);
            bank->deposited = 0;
        }
    } else if (player->score != 0 && !withdraw) {
        if (bank->deposit_amount && player->score > bank->deposit_amount) {
            bank->deposited += bank->deposit_amount;
            player_score(player, -bank->deposit_amount, 0);
        } else {
            bank->deposited += player->score;
            player_score(player, -player->score, 0);
        }
    }
    int uid = -1;
    for (size_t i = 0; i < game->map->object_count; i++) if (game->map->objects[i] == bank) uid = (int) i;
    const NetObjectStatePacket packet = {.uid = uid, .kind = NET_OBJECT_BANK, .value = bank->deposited};
    game_send(game, NET_TARGET_ALL, NET_PACKET_OBJECT_STATE, &packet, sizeof(packet)); // "bnk"
}

// Player's collision loop: changeInteract(type, data, priority) for every object in order.
// A prompt without priority replaces a different type; type 4 (onInteract) has priority 1.
int game_interact_prompt(const Game *game, const Player *player, int *object_index) {
    int type = GAME_INTERACT_NONE, priority = 0, index = -1;
    if (object_index) *object_index = -1;
    if (!game || !game->map || !player || !player->active || player->noclip) return type;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        int want = GAME_INTERACT_NONE;
        if ((o->gate || o->bank || o->terminal) && (o->active || o->closeable) && collides_padded(player, o, GAME_INTERACT_RADIUS) &&
            team_allows(game, player, o)) {
            if (o->gate) want = o->no_msg ? GAME_INTERACT_NONE : GAME_INTERACT_GATE;
            else if (o->bank) want = GAME_INTERACT_BANK;
            else if (!o->no_msg) want = GAME_INTERACT_TERMINAL;
            if (want != GAME_INTERACT_NONE && want != type) {
                type = want;
                priority = 0;
                index = (int) i;
            }
        }
        if (o->active && game_object_event(o) == TRIGGER_EVENT_ON_INTERACT &&
            player_collides((Player *) player, o, game_constants.collision_padding) && 1 > priority) {
            type = GAME_INTERACT_TRIGGER;
            priority = 1;
            index = (int) i;
        }
    }
    if (object_index) *object_index = index;
    return type;
}

int game_terminal_in_range(const Game *game, const Player *player, const int prompt) {
    if (!game || !game->map || !player || !player->active) return -1;
    for (size_t i = 0; i < game->map->object_count; i++) {
        if (terminal_usable(game, player, game->map->objects[i], prompt)) return (int) i;
    }
    return -1;
}

void game_player_interact(Game *game, Player *player, const int action, const int secondary) {
    if (!game || !player || !player->active || !game->map || !game_is_authority(game)) return;
    if (action != 1) return; // spray (0) and drop (2) are not ported
    if (game->now - player->last_interact < GAME_INTERACT_TIMER) return;
    player->last_interact = game->now;
    if (!secondary) {
        // (pickups: not ported) then the gates, while no waitTimer holds the clock
        if (!game->mode || !game->mode->wait_timer_count || game->mode->wait_timers[0].cont_time) {
            for (size_t i = 0; i < game->map->object_count; i++) {
                Object *gate = game->map->objects[i];
                if (!gate_usable(game, player, gate)) continue;
                use_gate(game, player, gate);
                break;
            }
        }
        for (size_t i = 0; i < game->map->object_count; i++) {
            if (!terminal_usable(game, player, game->map->objects[i], 0)) continue;
            const NetTerminalPacket packet = {.uid = (int32_t) i};
            game_send(game, player->uid, NET_PACKET_TERMINAL, &packet, sizeof(packet)); // IO.send(player.id, "ot", F.uid)
        }
    }
    for (size_t i = 0; i < game->map->object_count; i++) {
        Object *bank = game->map->objects[i];
        if (!bank->bank || !bank->active || !collides_padded(player, bank, GAME_INTERACT_RADIUS) || !team_allows(game, player, bank)) continue;
        use_bank(game, player, bank, secondary);
        break;
    }
}
