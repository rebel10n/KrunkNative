#pragma once
// The "queued for ranked" notice's social frame (ranked.c queue_notice, v10 server.message.queued). Pure: no database,
// no socket, so the frame a test checks is the one the brain sends.
#include <stdint.h>

#include "cJSON.h"

// The social socket's ["eb", {type: "ranked:queued"}] object: the queued player by its account id and name, the template
// ("$" = name, or "A player queued for ranked" with no name) and the ranked green. No composed line is sent: the client
// formats the notice from the template and the name (player_names_account), so the text never carries the name ahead of
// the client's own Hide player names choice. Caller frees.
cJSON *ranked_notice_social_event(int64_t account, const char *name);
