/*
 * css_cascade.h - selector matching + cascade + inheritance -> CssComputed
 * for every element of a Dom (plus ::before/::after and the other
 * pseudo-elements on demand).
 *
 * Cascade order (lowest to highest): UA normal < author normal (sheet order,
 * then specificity, then source order) < inline style < author !important <
 * inline !important < UA !important.  Declarations are applied in ascending
 * order through css_computed_apply(), so shorthand/longhand interleaving and
 * duplicate properties resolve naturally (last applied wins).
 *
 * Custom properties and font-size are applied before every other property
 * so var() and em resolve against the element's own values.  var()
 * substitution looks at the element's custom properties, which already
 * include the inherited ones (css_computed_inherit copies them).
 *
 * Dirty protocol (see dom.h): cascade_restyle() visits elements whose
 * ElemData.dirty has STYLE/DESCENDANTS/HAS_DIRTY, recomputes them (children
 * of an element whose inherited values changed are recomputed too), clears
 * those bits and raises DOM_DIRTY_STYLE_CHANGED where the computed record or
 * the ::before/::after records changed.  Subtrees under display:none are not
 * computed (like browsers); they are restyled when the ancestor becomes
 * displayed again.
 *
 * Sheets are not owned; the last set passed to cascade_restyle() is kept for
 * cascade_elem_computed() / cascade_pseudo_computed().
 */
#ifndef KN_CSS_CASCADE_H
#define KN_CSS_CASCADE_H

#include <stdint.h>

#include "css_sheet.h"
#include "css_types.h"
#include "dom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Recomputes every dirty element.  `author` are the author sheets in
 * document order (index.html <link> order); `ua` may be NULL.  Returns the
 * number of elements whose style was recomputed. */
uint32_t cascade_restyle(Dom *dom, CssSheet *ua, CssSheet **author, int n_author);

/* Same, restricted to the subtree rooted at `e` (e.g. after a hover change);
 * ancestors are computed first when needed. */
uint32_t cascade_restyle_elem(Dom *dom, HtmlElem e, CssSheet *ua, CssSheet **author, int n_author);

/* Computed style of an element.  Computes it on demand (with the sheets of
 * the last cascade_restyle call) when it is missing or dirty, including
 * elements inside display:none subtrees.  NULL for an invalid handle. */
const CssComputed *cascade_elem_computed(Dom *dom, HtmlElem e);
/* ::before / ::after computed styles, NULL when content is none/normal. */
const CssComputed *cascade_elem_before(Dom *dom, HtmlElem e);
const CssComputed *cascade_elem_after(Dom *dom, HtmlElem e);

/* Computes the style of a pseudo-element (scrollbar parts, slider thumb,
 * spin buttons, colour swatch, placeholder) into *out, inheriting from the
 * element.  Returns the number of matched declarations (0 == nothing styles
 * it; *out still holds the inherited/initial values). */
uint32_t cascade_pseudo_computed(Dom *dom, HtmlElem e, int pe, CssComputed *out);

/* Keyframes lookup across the current sheets (last author sheet wins). */
const CssKeyframes *cascade_find_keyframes(const char *name);

/* Frees the module's scratch buffers (optional, for leak checkers). */
void cascade_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* KN_CSS_CASCADE_H */
