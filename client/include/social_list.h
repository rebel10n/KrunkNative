#pragma once
// The hub's friends / conversations list (Krunker v10.0.6, the event bus store, deob-10.0.6.js:141245-141372): pure
// cJSON helpers for client/src/ui/v10_social.c (client/tests/social_list_test.c).
#include <cJSON.h>
#include <stdint.h>

// friends:list with an offset > 0 (:141252-141265): a page's friends join the list unless one with the same id is
// already there. Returns how many copies it appended: they are the list's last entries, in the page's order.
int social_friends_merge(cJSON *list, const cJSON *page);

// msg:recv (:141342-141364): the sender's entry gets the message as its last one (unread, lastId, lastText, lastTs,
// lastFromMe false). A sender not in the list is added first, at the top, as {id, name, online: true}, when its name
// is known (the message's name, or one seen before); with no name nothing is added. Returns the entry, or NULL.
cJSON *social_dm_received(cJSON *list, int64_t from, const char *name, int unread, int64_t id, const char *text, double ts);

// Opening a conversation (iìiíiíï, :141567-141575): the peer's entry reads unread 0 (msg:read goes to the bus).
// Returns the entry, or NULL when the peer has none.
cJSON *social_conversation_read(cJSON *list, int64_t peer);
