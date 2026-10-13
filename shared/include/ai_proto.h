#ifndef KRUNKNATIVE_AI_PROTO_H
#define KRUNKNATIVE_AI_PROTO_H

// OpenAI-compatible chat protocol shared by kn_brain's AI proxy (services/brain/ai.c) and the
// editor's AI panel (client/src/editor/editor_ai*.c): request bodies and streaming answers of
// both endpoint styles.
//
//   AI_API_CHAT       POST <base>/chat/completions   SSE "data: {chat.completion.chunk}" lines
//   AI_API_RESPONSES  POST <base>/responses          SSE "event: response.*" + "data: {...}"
//
// The conversation is always kept in the chat-completions message shape (role system / user /
// assistant / tool, content text, assistant tool_calls [{id,type:"function",function:{name,
// arguments}}], tool tool_call_id); ai_request_body converts it for the responses style.
// The stream parser detects the style from the events themselves, so one parser serves the
// brain (usage accounting while it relays) and the client (the panel's live text).
//
// Thinking traces: chat deltas' reasoning_content / reasoning, responses' reasoning_text /
// reasoning_summary_text deltas, and <think>...</think> blocks at the start of the content
// (deepseek-r1 distills, qwq) all go to the reasoning channel.
#include <stddef.h>
#include <stdint.h>
#include <cJSON.h>

typedef enum { AI_API_CHAT = 0, AI_API_RESPONSES = 1 } AiApi;

AiApi ai_api_parse(const char *s, AiApi def); // "chat" / "chat_completions" / "responses"
const char *ai_api_name(AiApi api);           // "chat" / "responses"
const char *ai_api_path(AiApi api);           // "/chat/completions" / "/responses"

// Upstream request body (malloc'd JSON). messages: chat-shape array (borrowed). tools: chat-shape
// [{type:"function",function:{name,description,parameters}}] or NULL. effort: "" / NULL = none,
// else "low" / "medium" / "high". max_tokens <= 0: not sent.
char *ai_request_body(AiApi api, const char *model, const cJSON *messages, const cJSON *tools,
                      const char *effort, int stream, int max_tokens);

enum { AI_DELTA_TEXT = 0, AI_DELTA_REASONING = 1, AI_DELTA_TOOL = 2 };
#define AI_MAX_TOOL_CALLS 16

typedef struct {
    char *data;
    size_t len, cap;
} AiBuf;

typedef struct {
    char id[96];
    char name[128];
    char item_id[96];   // responses: the output item id the argument deltas name
    AiBuf args;         // arguments JSON text as streamed
} AiToolCall;

typedef void (*AiDeltaFn)(void *user, int kind, const char *text, size_t len);
// Extension events of kn_brain's stream ("event: kn.<name>" + a JSON object: kn.tool_start,
// kn.server_tool, kn.done, ...): handed over as parsed JSON, never mixed into the answer.
typedef void (*AiEventFn)(void *user, const char *event, const cJSON *data);

typedef struct AiStream {
    AiBuf line;         // SSE line being assembled
    char event[64];     // current "event:" name
    AiBuf data;         // "data:" lines of the current event
    AiBuf text;         // answer text so far
    AiBuf reasoning;    // thinking trace so far
    AiToolCall calls[AI_MAX_TOOL_CALLS];
    int ncalls;
    int api;            // detected style, -1 until the first event
    int think_state;    // <think> split: 0 start of content, 1 inside <think>, 2 plain text
    AiBuf think_hold;   // content held while deciding whether it opens <think>
    int64_t prompt_tokens, completion_tokens, reasoning_tokens, cached_tokens;
    int has_usage;
    char finish[32];    // finish_reason / response status ("stop", "tool_calls", "length", ...)
    char model[96];
    char error[512];    // upstream error event ("" none)
    int done;           // [DONE] / response.completed seen
    int events;         // events parsed
    AiDeltaFn on_delta; // live deltas (may be NULL)
    AiEventFn on_event; // kn.* events (may be NULL)
    void *user;
} AiStream;

void ai_stream_init(AiStream *s, AiDeltaFn on_delta, void *user);
void ai_stream_free(AiStream *s);
// Feed raw response bytes (any split). Returns the number of events parsed by this call.
int ai_stream_feed(AiStream *s, const char *bytes, size_t n);
// End of the body: flushes a last event without a blank line and any held <think> text.
void ai_stream_finish(AiStream *s);
// A non-streamed JSON answer (chat.completion or response object) into the same fields.
int ai_parse_response(AiStream *s, const char *json, size_t n);

// The assistant message of a finished stream in chat shape: {role:"assistant", content,
// reasoning_content?, tool_calls?}. Caller frees.
cJSON *ai_stream_message(const AiStream *s);
// Usage of a finished stream: {prompt_tokens, completion_tokens, reasoning_tokens?, cached_tokens?}
cJSON *ai_stream_usage(const AiStream *s);

// GET /models of an OpenAI-compatible endpoint -> [{id, name, context, max_tokens, reasoning,
// tools, vision, efforts, price_in, price_out, currency, description}] sorted by id (unknown
// fields omitted; price_* per million tokens).
// NULL when the body is not a model list. Caller frees.
cJSON *ai_models_normalise(const char *json, size_t n);

// Error text of an upstream error body ({"error":{"message"}} / {"error":"..."} / {"message"} /
// plain text), clipped to n. Returns out.
char *ai_error_text(const char *body, size_t len, char *out, size_t n);

void ai_buf_add(AiBuf *b, const char *s, size_t n);
void ai_buf_free(AiBuf *b);

// ---- kn_brain's own stream (what the brain relays to the editor, whatever the upstream style):
// chat.completion.chunk SSE events, so one AiStream parses the brain and a custom endpoint alike.
//   data: {"choices":[{"index":0,"delta":{"content"|"reasoning_content":"..."}}]}
//   data: {"choices":[{"index":0,"delta":{"tool_calls":[{index,id,type,function:{name,arguments}}]}}]}
//   data: {"choices":[{"index":0,"delta":{},"finish_reason":"..."}],"usage":{...},"model":"..."}
//   event: kn.<name> + data: {...}     extension events (AiEventFn)
//   event: error + data: {"error":{"message","code"}}
//   data: [DONE]
// Each helper returns a malloc'd SSE event (ends with the blank line). Caller frees.
char *ai_sse_delta(int kind, const char *text, size_t len); // AI_DELTA_TEXT / AI_DELTA_REASONING
char *ai_sse_tool_calls(const AiStream *s);                 // the finished stream's tool calls (NULL: none)
char *ai_sse_finish(const char *finish, const cJSON *usage, const char *model);
char *ai_sse_event(const char *event, const cJSON *data);   // "event: <event>\ndata: <json>\n\n"
char *ai_sse_error(const char *code, const char *message);
#define AI_SSE_DONE "data: [DONE]\n\n"

// Estimated cost in USD of a usage with a model row of ai_models_normalise (price_in /
// price_out per million tokens); -1 when the model has no prices.
double ai_usage_cost(const cJSON *model_row, int64_t prompt_tokens, int64_t completion_tokens);

#endif
