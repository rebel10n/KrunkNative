// The map editor's AI panel (KrunkNative; brain side: services/brain/ai.h). Modules:
//   editor_ai.c        the panel: model picker, conversation, streaming (kn_brain's /ai/chat or the
//                      user's own OpenAI-compatible endpoint), thinking traces, tool cards,
//                      approvals, history, usage
//   editor_ai_md.c     markdown-ish text -> the panel's HTML
//   editor_ai_tools.c  the editor tools offered to the model (tools/editor-mcp's tools, run in this
//                      editor against the open map)
#pragma once
#include "editor_internal.h"

// ---- editor_ai.c
void ed_ai_install(HtmlHandlerEntry *table, size_t *count, size_t max);
void ed_ai_tick(void);                       // page_tick: stream callbacks, timers
void ed_ai_toggle(void);                     // toolbar / canvas button
void ed_ai_reset(void);                      // page leave: stop streams, drop the panel
int ed_ai_keydown(const HtmlEvent *ev);      // Escape closes popovers; 1 = handled
int ed_ai_script(const char *cmd, const char *arg); // KRUNKNATIVE_EDITOR_SCRIPT "ai:..." (1 = handled)

// ---- editor_ai_tools.c
int ai_tools_available(void);                // 1 when this build runs the editor tools
const char *ai_tools_unavailable_reason(void);
cJSON *ai_tools_defs(void);                  // chat-shape tool definitions (caller frees); NULL = none
int ai_tool_known(const char *name);
int ai_tool_needs_approval(const char *name); // destructive / writes files: the user confirms first
int ai_tool_read_only(const char *name);
// Runs one tool on the shown map: {text (for the model), is_error, images?:[data URLs]} (caller frees)
cJSON *ai_tool_run(const char *name, const char *args_json);

// ---- editor_ai_md.c
void ai_md_html(const char *md, EdBuf *out);
void ai_html_escape(EdBuf *out, const char *s);
