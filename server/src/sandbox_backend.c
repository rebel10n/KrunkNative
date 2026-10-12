// The server core's backend hooks in a build without server_bin's network frontend (the
// client's offline sandbox, server_sandbox.h): no central auth and no rcon. The brain link is
// server/src/brain.c built without knsvc, which is already the standalone no-op.
#include <central_auth.h>
#include <rcon.h>
#include <stdio.h>

int central_auth_has_joined(const char *auth_url, const uint32_t rid, const char *username, const char *server_hash, const char *ip) {
    (void) auth_url; (void) rid; (void) username; (void) server_hash; (void) ip;
    return -1; // online_mode is never on in the sandbox
}

int central_auth_poll(CentralAuthResult *out) {
    (void) out;
    return 0;
}

void rcon_handle_brain_frame(const cJSON *frame) {
    (void) frame; // only a coordinator sends rcon frames
}
