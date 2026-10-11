#pragma once
// A C port of the parts of dat.gui (public/libs/dat.gui.min.js, v0.7) Krunker's viewer uses:
// one auto-placed root GUI, folders, boolean / option / function controllers, name(),
// listen(), onChange(), remove(), removeFolder(), open() and the "taller-than-window" resize.
// The markup is dat.gui's own (.dg.ac > .dg.main.a > ul > li.cr ...), styled by
// ui/css/dat-gui.css (dat.gui's injected stylesheet); events come back through inline
// handlers (vgRow / vgCheck / vgSelect / vgTitle) dispatched by the page's handler table.
#include <html_dom.h>

typedef struct VgFolder VgFolder;
typedef struct VgCtrl VgCtrl;

// A property a controller edits. dat.gui writes what the widget gives it: a checkbox a boolean,
// a <select> its option value as a *string* (so the row class of a controller created later
// reflects that: "string" instead of "number").
typedef struct {
    int num;        // numeric value (JS Number(str) for strings that came from a select)
    int is_string;  // typeof value == "string"
    char str[48];   // the string form while is_string
} VgValue;

typedef void (*VgFn)(void);

void vg_init(int width);          // new dat.GUI(); gui.width = width; (appends .dg.ac to <body>)
void vg_fini(void);
void vg_register_handlers(HtmlHandlerEntry *table, int *count); // appends vgRow/vgCheck/vgSelect/vgTitle

VgFolder *vg_root(void);
VgFolder *vg_add_folder(VgFolder *parent, const char *name); // closed until vg_folder_open()
void vg_folder_open(VgFolder *folder);
void vg_folder_remove(VgFolder *parent, VgFolder *folder);

VgCtrl *vg_add_bool(VgFolder *folder, int *value, const char *name, VgFn on_change);
VgCtrl *vg_add_function(VgFolder *folder, VgFn fn, const char *name);
// .options(): labels[i] / values[i] in display order (values as their string form)
VgCtrl *vg_add_options(VgFolder *folder, VgValue *value, const char *name, int count,
                       const char *const *labels, const char *const *values, VgFn on_change);
void vg_remove(VgFolder *folder, VgCtrl *ctrl);

void vg_update_display(void);     // listen(): push every bound value back into its widget
void vg_resize(void);             // window resize: onResizeDebounced
void vg_toggle_hide(void);        // the "H" key
int  vg_contains(float x, float y); // CSS px point over the GUI panel (input routing)
