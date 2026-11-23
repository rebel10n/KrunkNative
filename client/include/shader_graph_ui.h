#pragma once
// v10.0.7's shader graph node canvas editor (svelte-upgo13 sgEditor, deob-10.0.7.js:122896-124145; its node
// svelte-wbsbny :122642-122700, palette svelte-sxvv3c :122700-122710, value widgets svelte-1i1uhp7 :122535-122640),
// as markup with inline handlers over one graph document: the toolbar (Add node, Undo / Redo, Fit, Templates,
// Import / Export JSON, the zoom), the canvas (pan, zoom, nodes with their ports, params and input values, wires,
// the node palette), the JSON overlay and the footer's validity. The Skin Manager mounts it in Shader Studio
// (kind "surface") and in a paint's editor (kind "paint").
#include <html_dom.h>
#include <ui_logic.h> // StrBuf

struct cJSON;
typedef struct SgEditor SgEditor;

// onChange (debounced 100 ms, :122943-122950): the graph as it now stands (the editor keeps it)
typedef void (*SgChangeFn)(void *user, const struct cJSON *graph);

// `prefix`: the DOM ids' prefix (e.g. "knSmSg"); `call`: the inline handlers' call up to its first argument, e.g.
// "knSkins('sg'": each handler is `<call>,'<op>',...)`, and the owner hands those calls to sg_editor_handle
SgEditor *sg_editor_new(const char *prefix, const char *call, const char *kind, SgChangeFn fn, void *user);
void sg_editor_free(SgEditor *ed);
// the graph prop (:122953-122973): a document other than the one the editor last produced resets the history and the
// selection and fits the view; NULL or not a graph document: the kind's default graph (iìíïïìí, :122370)
void sg_editor_set_graph(SgEditor *ed, const struct cJSON *graph);
const struct cJSON *sg_editor_graph(const SgEditor *ed);
void sg_editor_set_readonly(SgEditor *ed, int readonly);
// the editor's markup, `<div class="sgEditor ...">...</div>`
void sg_editor_html(SgEditor *ed, StrBuf *sb);
// one inline handler call: args[0] the op, then its arguments. Returns the handler's result (0 = preventDefault).
int sg_editor_handle(SgEditor *ed, const HtmlEvent *ev, const HtmlArg *args, int argc);
// per frame: the pending onChange, the message's timeout, a fit waiting for the canvas's size
void sg_editor_tick(SgEditor *ed);
// the pending onChange now (the component's teardown, :122974-122976)
void sg_editor_flush(SgEditor *ed);
