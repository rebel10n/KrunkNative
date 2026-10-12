#pragma once
// Account persistence and services of the game server (doc/v7/krunkscript.md 2.6, 3.1.12,
// 3.1.22, 5.2, WP6): GAME.STORAGE, the map triggers Save Data (49) / Load Data (57) and the KR
// rewards of GAME.PAYMENTS.giveKR and the Reward KR trigger (32). Part of the server core, so
// the offline sandbox runs it too. Every function runs with g_server->lock held.
//
// Backends (v7 game.storageUpdt / getStoragePlayer / rewardKR went to its database):
//   * kn_brain (server_bin linked to a coordinator): "acct" ops storage / storage_load /
//     map_reward over the internal WebSocket (doc/backend.md 5.2, 7.6); replies "acct-r".
//     Storage needs a published map: the lobby hosts a published map (accounts.c), like v7's
//     defaultSetts in production. The client never talks to the database.
//   * offline file store (the server core runs as the offline sandbox, no brain): one JSON
//     document per map, "ks-storage-<hash>" in the user's config dir
//     ($XDG_CONFIG_HOME/krunknative/ks-storage-<hash>.json, web localStorage through
//     ks_storage_set_offline_io), same validation and semantics. The sandbox player counts as
//     having an account (doc/v7/decisions.md "Offline account"): account name "local". No KR
//     economy offline: rewards do nothing.
//   * none (a standalone server_bin): every request fails with v7's no-database "Error";
//     backend-only effects do nothing (decisions.md "Backend-only triggers").
#include <server.h>
#include <ks_storage_data.h>
#include <cJSON.h>

#define KS_STORAGE_OFFLINE_ACCOUNT "local" // the sandbox player's account name [KN]

// Completion of one request: rows = the stored [key, value] pairs (keys as stored, e.g.
// "V_coins"); error NULL on success. Called exactly once, from ks_storage_poll or
// ks_storage_on_brain_frame (never from inside the request call).
typedef void (*KsStorageDone)(void *user, const KsStoragePair *rows, int count, const char *error);

// game.storageUpdt(name, data, type, scope): validation (the v7 messages: "Error",
// "Storage only supported on published maps", "Missing map data", the data errors of
// ks_storage_data_error, "Invalid storage type", "Player not connected"), then the backend.
// Returns NULL when the request was queued (done follows later), else the error text (done
// is not called). The completion rows are the touched keys as stored after the operation.
const char *ks_storage_update(const char *account_name, const KsStorageData *data, int type, int scope, KsStorageDone done, void *user);
// game.getStoragePlayer(name, mapName): mapName NULL / "" = this map (every row of the
// account), another map = its global rows when it has this map's creator. Errors as above
// (no "Player not connected": any account may be read); a backend failure completes with
// "No data found", no rows with count 0.
const char *ks_storage_load(const char *account_name, const char *map_name, KsStorageDone done, void *user);

// The account name storage uses for a connection's player: the account's username online
// ("" guests), KS_STORAGE_OFFLINE_ACCOUNT for every sandbox connection. 0 = no account.
int ks_storage_account_of(const ClientConnection*, char *name, size_t n);

// game.rewardKR(player, amount) (GJS:4341-4360): the host option KR rewards on a published
// map, an account that is not hack-flagged, |amount| > 0, one reward in flight per player.
// Online: kn_brain pays it out of the map's KR funds; on success the player gets the chat
// line "Rewarded N KR" and the popup. Offline / standalone: nothing.
void ks_storage_reward_kr(const Player*, double amount);

// GameHooks.storage_save / storage_load / reward_kr of the map triggers (net_install_game_hooks).
void ks_storage_install_game_hooks(Game*);

// Main thread, every tick: delivers completions, flushes the offline store (at most once a
// second), times out brain requests (15 s -> "Error").
void ks_storage_poll(void);
// "acct-r" frames from the brain; 1 = one of ours (consumed).
int ks_storage_on_brain_frame(const cJSON *frame);
// The brain link went down: pending brain requests complete with "Error".
void ks_storage_link_down(void);
// Sandbox stop / server shutdown: flushes the offline store, completes everything pending
// with "Error" and forgets the cached documents.
void ks_storage_shutdown(void);

// Offline store I/O: read(key, buf, n) -> length or -1 (html_storage_get), write(key, text)
// (html_storage_set, text NULL = delete). NULL = the built-in files under $XDG_CONFIG_HOME/
// krunknative (else ~/.config/krunknative).
typedef int (*KsStorageRead)(const char *key, char *buf, size_t n);
typedef void (*KsStorageWrite)(const char *key, const char *text);
void ks_storage_set_offline_io(KsStorageRead read, KsStorageWrite write);
// The offline store key of a map ("ks-storage-<fnv1a32 hex>").
void ks_storage_offline_key(const char *map_name, char *out, size_t n);

// Test hook: brain frames go to `sink` (consumed by the caller) instead of brain_post, and the
// brain backend counts as linked while a sink is set.
void ks_storage_set_brain_sink(void (*sink)(cJSON *frame));
