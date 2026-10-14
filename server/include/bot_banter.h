#pragma once
// Bot chat through kn_brain (shared/include/bot_chat.h, server/src/bot_banter.c): the
// GameHooks.bot_banter hook sends a `bot-banter` frame over the internal WebSocket (brain.h,
// doc/backend.md 5.2) and the `bot-banter-reply` hands the AI's line to bot_chat_deliver.
// Templates only (the hook returns 0) when gameserver.bot_chat_ai is off, the brain is not
// linked, or no signed-in player is in the game (guests do not spend the AI budget).
#include <shared.h>
#include <cJSON.h>

struct BotChatRequest_t;
int bot_banter_request(Game*, const struct BotChatRequest_t*); // GameHooks.bot_banter
int bot_banter_on_brain_frame(const cJSON *frame);           // "bot-banter-reply": 1 = consumed
