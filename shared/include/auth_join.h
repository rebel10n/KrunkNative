#pragma once
// Online-mode join (Mojang-style, doc/backend.md "Central auth"): the serverHash both the
// client and the game server derive, so a session's join only verifies on the server it
// was meant for. Pure C, no OpenSSL (own SHA-256), shared by client and server.
//
//   serverHash = hex(SHA-256("kn-join-v1" 0x00 server_nonce 0x00 public_key 0x00 client_nonce))
//
// server_nonce: NetAuthChallengePacket.server_nonce (per boot), public_key: its public_key
// ("" without a transport key), client_nonce: 32 hex chars the client picks per connection.
// JOIN.client_key of such a join is AUTH_JOIN_KEY_PREFIX + client_nonce. The server verifies
// accounts this way and admits guests too (Krunker: every public lobby takes both).
#include <stddef.h>
#include <stdint.h>

#define AUTH_JOIN_KEY_PREFIX "hj1:"
// JOIN.client_key of a client without an account answering the challenge: "no hasJoined is
// coming, I play as a guest" (online_mode servers admit guests; without it they wait out
// join_timeout for an "hj1:" JOIN first).
#define AUTH_JOIN_GUEST_KEY "hj0"
#define AUTH_JOIN_NONCE_LEN 32       // hex chars of server/client nonces
#define AUTH_SERVER_HASH_LEN 64      // hex chars of a serverHash

void kn_sha256_bytes(const void *data, size_t len, uint8_t out[32]);
// Incremental SHA-256 (the client's updater hashes packages and asset files as they stream in).
typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    size_t fill;
    uint64_t total;
} KnSha256;
void kn_sha256_init(KnSha256 *s);
void kn_sha256_update(KnSha256 *s, const void *data, size_t len);
void kn_sha256_final(KnSha256 *s, uint8_t out[32]);
void kn_sha256_hex(const uint8_t digest[32], char out_hex[65]);
// out_hex: 65 bytes (64 hex + NUL). NULL strings count as "".
void kn_server_hash(const char *server_nonce, const char *public_key, const char *client_nonce, char out_hex[65]);
// 1 when s is 1..64 lowercase hex chars (nonce / hash validation).
int kn_auth_is_hex(const char *s, size_t max_len);
// Constant-time secret check (server password, rcon password): 1 when given == secret. A NULL or
// blank secret never matches, so an unset password can't be "guessed" with an empty one.
int kn_secret_equal(const char *given, const char *secret);
