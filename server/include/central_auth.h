#pragma once
// Online-mode join verification against the central auth service (doc/backend.md 8.9,
// contract /tmp/kn-tasks/contract-auth.md): GET <auth_url>/auth/hasJoined?username=&serverHash=&ip=
// -> 200 {id, username, profile} | 204 not joined. One worker thread per request; results
// are polled by the main thread (accounts_tick).
#include <cJSON.h>
#include <stdint.h>

typedef struct {
    uint32_t rid;
    int status;          // 1 joined, 0 not joined (204 / 4xx), -1 service error
    cJSON *reply;        // {id, username, profile} when status == 1 (caller deletes)
    char error[128];
} CentralAuthResult;

// Starts the lookup. Returns 0 when the worker started.
int central_auth_has_joined(const char *auth_url, uint32_t rid, const char *username, const char *server_hash, const char *ip);
// Main thread: next finished lookup. Returns 1 while results are pending.
int central_auth_poll(CentralAuthResult *out);
