// MCP (Model Context Protocol) server core of kn_editor_mcp: JSON-RPC 2.0 over stdio
// (newline-delimited) or Streamable HTTP (--http), lifecycle (initialize / initialized / ping),
// tools, resources, prompts, logging and the tool result helpers the tool modules use.
#pragma once
#include "ctx.h"
#include <cJSON.h>

#define MCP_PROTOCOL_LATEST "2025-06-18"

typedef struct {
    cJSON *content;    // array of content blocks
    cJSON *structured; // structuredContent (object) or NULL
    int is_error;
} ToolResult;

typedef int (*ToolFn)(Ctx *ctx, const cJSON *args, ToolResult *out);

typedef struct {
    const char *name;
    const char *title;
    const char *description;
    const char *input_schema; // JSON text
    unsigned char read_only, destructive, idempotent, open_world;
    ToolFn fn;
} ToolDef;

// JSON text from tokens: S({"type":"object"}) -> "{\"type\":\"object\"}". Two levels so token
// macros inside (PROJECT_ARG) expand first.
#define S_(...) #__VA_ARGS__
#define S(...) S_(__VA_ARGS__)
// The common optional "project" argument, as JSON tokens for S().
#define PROJECT_ARG "project":{"type":"string","description":"project id (f1 or 1.2), path, basename or map name; default: the focused project"}

void tr_text(ToolResult *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Sets structuredContent (taken over) and adds its JSON as a text block.
void tr_json(ToolResult *r, cJSON *structured);
void tr_image(ToolResult *r, const char *base64_png, const char *mime);
// Marks the result as a tool error with a text message; returns 0 so handlers can `return tr_error(...)`.
int tr_error(ToolResult *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Tool tables of the modules (each ends with a {0} entry).
extern const ToolDef g_tools_project[];
extern const ToolDef g_tools_objects[];
extern const ToolDef g_tools_logic[];
extern const ToolDef g_tools_build[];
extern const ToolDef g_tools_check[];
extern const ToolDef g_tools_render[];
extern const ToolDef g_tools_script[];

// Common argument helpers
Project *tool_project(Ctx *ctx, const cJSON *args, ToolResult *r); // NULL after tr_error
int *tool_ids(const cJSON *arr, int *count);                       // malloc'd, NULL when not an int array
const char *tool_str(const cJSON *args, const char *key, const char *def);

// Runs the stdio loop until EOF. Returns the exit code.
int mcp_run_stdio(Ctx *ctx);
// Streamable HTTP transport on host:port (port 0 = any); writes the URL to announce_path if set.
int mcp_run_http(Ctx *ctx, const char *listen, const char *announce_path);
// Handles one JSON-RPC message (object or batch array). Returns the response (NULL for
// notifications) as owned cJSON.
cJSON *mcp_handle(Ctx *ctx, const cJSON *msg);
// Calls a tool directly (tests / --call). Returns the MCP result object.
cJSON *mcp_call_tool(Ctx *ctx, const char *name, const cJSON *args);
