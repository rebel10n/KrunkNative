#pragma once
// Online / offline mode of the client (doc/backend.md "Client modes",
// client/src/net/online_mode.c).
//
//   ONLINE   accounts live at the central auth service (default
//            https://coordinator.krunk.io, which also runs the community
//            coordinator): login / register, inventory and stats are saved. Joining an
//            online_mode server runs the Mojang-style handshake: the server's
//            NET_PACKET_AUTH_CHALLENGE -> POST <account service>/auth/join {token,
//            serverHash} -> JOIN {name = username, client_key = "hj1:" + client nonce}.
//            Signed out, the answer is JOIN {client_key = AUTH_JOIN_GUEST_KEY}: every
//            server admits guests (unsaved), as every Krunker lobby does.
//            The session token only ever goes to the account service that issued it,
//            never to a game server.
//   OFFLINE  no account service at all: guest, every skin unlocked, nothing saved.
//            Joins every server as a guest, direct connects and practice (--offline).
//
// Mode, first match wins: --mode online|offline, KRUNKNATIVE_MODE, web ?mode=, storage
// `knMode` (Settings > Client > Account Mode), else online. The server finder list
// (GET <finder>?...) comes from --finder <url>, KRUNKNATIVE_FINDER_URL, web ?finder=,
// storage `knFinderUrl`, else ONLINE_DEFAULT_FINDER_URL (both modes; "" / "off" disables).
// No client.h / shared.h here: backend_native.c includes this next to OpenSSL, whose
// `UI` typedef clashes with client.h's. Client / NetAuthChallengePacket are passed as void *.

#define ONLINE_DEFAULT_ACCOUNT_URL "https://coordinator.krunk.io"
#define ONLINE_DEFAULT_FINDER_URL "https://matchmaker.krunk.io/servers"

// Before backend_init(): the CLI values (NULL = not given).
void online_mode_init(const char *cli_mode, const char *cli_finder);
int online_mode_online(void);                 // 1 online, 0 offline
const char *online_mode_name(void);           // "online" / "offline"
const char *online_finder_url(void);          // "" = no finder
// The default account service of the mode ("" offline): backend_init() uses it when no
// --brain / KRUNKNATIVE_BRAIN_URL / knBrainUrl is set.
const char *online_default_account_url(void);

// NET_PACKET_AUTH_CHALLENGE (game_events.c, main thread).
void online_mode_on_challenge(void *client, const void *challenge); // Client *, NetAuthChallengePacket *
// A new connection's challenge arrived (called by online_mode_on_challenge): drops the old join.
void online_mode_on_connect(void);
// Per frame: header bar in offline mode.
void online_mode_tick(void *client);
