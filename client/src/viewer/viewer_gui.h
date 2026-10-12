#pragma once
// A C port of the parts of dat.gui (public/libs/dat.gui.min.js, v0.7) Krunker's viewer uses:
// one auto-placed root GUI, folders, boolean / option / function / number (box) / colour
// controllers, name(), listen(), onChange(), min() / max() / step(), remove(), removeFolder(),
// open() and the "taller-than-window" resize.
// The markup is dat.gui's own (.dg.ac > .dg.main.a > ul > li.cr ...), styled by
// ui/css/dat-gui.css (dat.gui's injected stylesheet); events come back through inline
// handlers (vgRow / vgCheck / vgSelect / vgTitle / vgNumber / vgColor / vgDrag) dispatched by the
// page's handler table.
#include <html_dom.h>
#include <stddef.h>

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
void vg_register_handlers(HtmlHandlerEntry *table, int *count); // appends the vg* handlers (7)

VgFolder *vg_root(void);
VgFolder *vg_add_folder(VgFolder *parent, const char *name); // closed until vg_folder_open()
void vg_folder_open(VgFolder *folder);
void vg_folder_remove(VgFolder *parent, VgFolder *folder);

VgCtrl *vg_add_bool(VgFolder *folder, int *value, const char *name, VgFn on_change);
VgCtrl *vg_add_function(VgFolder *folder, VgFn fn, const char *name);
// .options(): labels[i] / values[i] in display order (values as their string form)
VgCtrl *vg_add_options(VgFolder *folder, VgValue *value, const char *name, int count,
                       const char *const *labels, const char *const *values, VgFn on_change);
// gui.add(obj, prop).min(min).max(max).step(step) on a number: a NumberControllerBox, which
// becomes a NumberControllerSlider (slider + box) once both min and max are set. min / max NAN =
// unset, step <= 0 = unset. setValue clamps and snaps to step; the box shows the value rounded to
// the step's decimals; dragging the box up / down steps it, the slider maps x to [min, max].
VgCtrl *vg_add_number(VgFolder *folder, double *value, const char *name, double min, double max, double step, VgFn on_change);
// gui.addColor(obj, prop) over 0xRRGGBB. *format is how the property holds it: a number (shown
// "0x" + hex), a "#rrggbb" or an "rgb(r,g,b)" string; a typed colour sets it to its own form.
// The saturation / hue selector opens on hover.
enum { VG_COLOR_HEX, VG_COLOR_CSS_HEX, VG_COLOR_CSS_RGB };
VgCtrl *vg_add_color(VgFolder *folder, int *hex, int *format, const char *name, VgFn on_change);
void vg_remove(VgFolder *folder, VgCtrl *ctrl);
void vg_folder_set_visible(VgFolder *folder, int visible); // folder.domElement.style.display

// A number box / colour field drag follows the window's mouse (CSS px) until the button is up.
int  vg_dragging(void);
void vg_drag_move(float x, float y);
void vg_drag_end(void);

// Tests: act on the controller named `path` ("Name" or "Folder/Name") as the user would: a select
// / number box / colour box gets `value` and its change event, a checkbox `value` ("true" /
// "false", empty = toggle), a function row or a folder title is clicked. 0 when there is none.
int  vg_test_action(const char *path, const char *value);

// String(number) of JS: the shortest round-trip digits (Export Config, the number boxes).
void vg_js_number(double value, char *buf, size_t n);

void vg_update_display(void);     // listen(): push every bound value back into its widget
void vg_resize(void);             // window resize: onResizeDebounced
void vg_toggle_hide(void);        // the "H" key
int  vg_contains(float x, float y); // CSS px point over the GUI panel (input routing)
