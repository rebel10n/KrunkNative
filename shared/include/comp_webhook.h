#pragma once
// The comp host's match-result webhook (host option "webhook", config.serverConfig "Webhook", host type 1:
// deob-10.0.3.js:20077-20082). v10's game keeps it only for a comp game with a non-empty value
// (applyConfig, deob-10.0.3.js:216122-216129: `this.webhook = isComp && webhook.length > 0 ? webhook : null`).
// The post itself is Krunker's server code, which is not in the bundle: the bundle only hands the server the
// match stats at the end (`onGameEnd(game, gameStatsTracker.getEndGameStats())`, deob-10.0.3.js:221913-221914;
// the entries at :215345-215364 and :215434-215492). The body below is the shape that receivers of Krunker's
// comp webhook read (a third-party tournament bot's README and handler, 2026-10-24: `type` "match_end",
// `map`, `winner`, `teams[{team, name, score}]`, `players[{team, name, score, kills, deaths,
// objective_score, damage_done, ...}]`); the rest of a player's keys are getEndGameStats' entry keys
// (doc/v10/ranked-impl.md "Comp webhook"). server_bin posts it (server/src/comp_webhook.c); this file is
// the pure part: the URL rule, the address rule and the body.
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

#define COMP_WEBHOOK_URL_MAX 200 // the option's input length (config.serverConfig "Webhook", game_config.c)

// 1 when url may be posted to: https://host[:port][/path], at most COMP_WEBHOOK_URL_MAX bytes of printable
// ASCII, no user info, a dotted DNS name (letters, digits, '-', '.') that is not a local name (localhost,
// *.localhost, *.local, *.internal, *.lan, *.home.arpa), or a public dotted-quad IPv4. IPv6 literals and other
// numeric host forms are refused. KrunkNative rule: v10's server code is not in the bundle.
int comp_webhook_url_ok(const char *url);
// 1 when a connect address is public: not 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24,
// 192.0.2/24, 192.168/16, 198.18/15, 198.51.100/24, 203.0.113/24, 224/4 or 240/4 (host byte order).
int comp_webhook_ipv4_public(uint32_t addr);
// The same for IPv6 (16 bytes, network order): not ::, ::1, IPv4-mapped / -compatible / NAT64 forms of a
// non-public IPv4, 100::/64, 2001:db8::/32, fc00::/7, fe80::/10, fec0::/10 or ff00::/8.
int comp_webhook_ipv6_public(const uint8_t addr[16]);

typedef struct {
    int team;          // 1 Alpha, 2 Bravo
    const char *name;  // the player's name (an account's name)
    int player_id;     // account id, 0 for a guest
    int score, kills, deaths, assists;
    int damage_done, objective_score, headshots;
    int shots, hits;   // accuracy = hits / shots * 100 (0 with no shot)
    int kr;            // the end reward
} CompWebhookPlayer;

typedef struct {
    const char *map;            // the map's name
    int mode;                   // modeIndex
    const char *team_names[2];  // nameTeam1 / nameTeam2
    int round_wins[2];          // teamRoundWins[1], [2]
    const CompWebhookPlayer *players;
    size_t player_count;
} CompWebhookMatch;

// The winning team of getEndGameStats (deob-10.0.3.js:215438-215440, ON_DRAW_FAVOUR_TEAM :215308): the one with
// more round wins, Alpha on a tie.
int comp_webhook_winner(const CompWebhookMatch *match);
// {type: "match_end", map, mode, winner, teams: [{team, name, score: round wins}], players: [{team, name,
// player_id, score, kills, deaths, assists, accuracy, headshots, damage_done, objective_score, rounds_won,
// victory, kr}]}, the players on a team (1 / 2) by score, highest first. The caller owns the result.
cJSON *comp_webhook_payload(const CompWebhookMatch *match);
