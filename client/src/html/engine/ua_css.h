/*
 * ua_css.h - the subset of Chrome's user-agent stylesheet the Krunker UI
 * depends on (doc/html-engine-plan.md, "Verification: Chrome UA stylesheet
 * subset").  Loaded by the cascade as the UA origin before the author sheets.
 *
 * Only what the shipped markup uses (audited against Chromium's html.css for
 * the elements of index/social/editor/viewer.html and their window/JS markup):
 * block/inline defaults, headings, hr, lists, links, b/i,
 * replaced-element intrinsic sizes, Chrome's refreshed form-control defaults
 * (input 13.333px Arial, borders/padding/colors, checkbox 13x13, range 129x20,
 * color 50x27, select menulist), ul/li, [hidden] and the hidden-input rules.
 * Values were cross-checked against the window captures in
 * doc/ref/krunker-v3.1.1/.
 */
#ifndef KN_UA_CSS_H
#define KN_UA_CSS_H

static const char KN_UA_CSS[] =
    /* ---- document / block boxes ---------------------------------------- */
    "html { display: block; }\n"
    "head, link, meta, script, style, title, template { display: none; }\n"
    "body { display: block; margin: 8px; }\n"
    "div, p, ul, ol, li, h1, h2, h3, h4, h5, h6, form, header, footer, section, "
    "nav, main, article, aside, figure, figcaption, pre, blockquote, hr, "
    "address, fieldset, legend, details, summary, dl, dt, dd, center { display: block; }\n"
    "li { display: list-item; }\n"
    "ul { list-style-type: disc; margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }\n"
    "ol { list-style-type: decimal; margin-top: 1em; margin-bottom: 1em; padding-left: 40px; }\n"
    "p { margin-top: 1em; margin-bottom: 1em; }\n"
    "h1 { font-size: 2em; font-weight: bold; margin-top: 0.67em; margin-bottom: 0.67em; }\n"
    "h2 { font-size: 1.5em; font-weight: bold; margin-top: 0.83em; margin-bottom: 0.83em; }\n"
    "h3 { font-size: 1.17em; font-weight: bold; margin-top: 1em; margin-bottom: 1em; }\n"
    "h4 { font-weight: bold; margin-top: 1.33em; margin-bottom: 1.33em; }\n"
    "h5 { font-size: 0.83em; font-weight: bold; margin-top: 1.67em; margin-bottom: 1.67em; }\n"
    "h6 { font-size: 0.67em; font-weight: bold; margin-top: 2.33em; margin-bottom: 2.33em; }\n"
    "hr { overflow: hidden; unicode-bidi: isolate; margin: 0.5em auto; border-style: inset; "
    "border-width: 1px; color: gray; }\n"
    "blockquote { margin: 1em 40px; }\n"
    "figure { margin: 1em 40px; }\n"
    "dl { margin-top: 1em; margin-bottom: 1em; }\n"
    "dd { margin-left: 40px; }\n"
    "address { font-style: italic; }\n"
    "ul ul, ol ul { list-style-type: circle; }\n"
    "ol ol ul, ol ul ul, ul ol ul, ul ul ul { list-style-type: square; }\n"
    "ul ul, ul ol, ol ul, ol ol { margin-top: 0; margin-bottom: 0; }\n"
    "form { margin-top: 0; }\n"
    "pre { white-space: pre; font-family: monospace; margin-top: 1em; margin-bottom: 1em; }\n"
    "center { text-align: center; }\n"
    "[hidden] { display: none; }\n"
    "br { display: inline; }\n"

    /* ---- inline text elements ------------------------------------------ */
    "b, strong { font-weight: bolder; }\n"
    "i, em, var, cite, dfn { font-style: italic; }\n"
    "u, ins { text-decoration: underline; }\n"
    "mark { background-color: yellow; color: black; }\n"
    "big { font-size: larger; }\n"
    "nobr { white-space: nowrap; }\n"
    "s, strike, del { text-decoration: line-through; }\n"
    "a:link { color: #0000ee; text-decoration: underline; cursor: pointer; }\n"
    "a:link:active { color: #ff0000; }\n"
    "small { font-size: smaller; }\n"
    "sub { vertical-align: sub; font-size: smaller; }\n"
    "sup { vertical-align: super; font-size: smaller; }\n"
    "code, kbd, samp, tt { font-family: monospace; }\n"

    /* ---- tables -------------------------------------------------------- */
    "table { display: table; border-collapse: separate; border-spacing: 2px; box-sizing: border-box; border-color: gray; }\n"
    "thead, tbody, tfoot { display: table-row-group; vertical-align: middle; border-color: inherit; }\n"
    "tr { display: table-row; vertical-align: inherit; border-color: inherit; }\n"
    "td, th { display: table-cell; vertical-align: inherit; padding: 1px; }\n"
    "th { font-weight: bold; text-align: center; }\n"

    /* ---- replaced elements --------------------------------------------- */
    "img { display: inline; overflow: clip; overflow-clip-margin: content-box; }\n"
    "canvas { display: inline; width: 300px; height: 150px; }\n"
    "video { display: inline; object-fit: contain; width: 300px; height: 150px; }\n"
    "iframe { display: inline; border: 2px inset; width: 300px; height: 150px; }\n"

    /* ---- form controls (Chrome refreshed defaults) --------------------- */
    "input, select, button, textarea, option { "
    "display: inline-block; text-align: start; font-family: Arial; font-size: 13.3333px; "
    "font-weight: 400; font-style: normal; line-height: normal; letter-spacing: normal; "
    "text-transform: none; text-shadow: none; color: #000000; cursor: auto; "
    "text-indent: 0; word-spacing: normal; text-rendering: auto; margin: 0; }\n"
    /* text-like inputs and textarea are content-box in Chrome (select, buttons,
     * checkbox/radio, color and search are border-box below) */
    "input, textarea, select { background-color: #ffffff; border: 2px inset; border-color: #767676; "
    "border-radius: 0; padding: 1px 2px; "
    "appearance: auto; }\n"
    "input[type=\"search\"] { box-sizing: border-box; }\n"
    "input { border-width: 2px; border-style: inset; padding: 1px 2px; "
    "-webkit-rtl-ordering: logical; cursor: text; }\n"
    "input::placeholder { color: #757575; }\n"
    "input:disabled, select:disabled { color: #6d6d6d; background-color: #efefef; border-color: #a9a9a9; cursor: default; }\n"
    "input[type=\"hidden\"] { display: none; }\n"
    "input[type=\"checkbox\"], input[type=\"radio\"] { "
    "appearance: auto; box-sizing: border-box; width: 13px; height: 13px; "
    "margin: 3px 3px 3px 4px; padding: 0; border: 0; background-color: transparent; "
    "cursor: default; }\n"
    "input[type=\"radio\"] { margin: 3px 3px 0 5px; }\n"
    "input[type=\"range\"] { "
    "appearance: auto; margin: 2px; padding: 0; border: 0; "
    "background-color: transparent; color: #101010; cursor: default; }\n"
    /* thumb: 16x16 (zoomed) while appearance is auto, whatever its CSS size;
     * track: flex: 1, align-self: center (widgets.c range_geom) */
    "input[type=\"range\"]::-webkit-slider-thumb { appearance: auto; box-sizing: border-box; display: block; }\n"
    "input[type=\"range\"]::-webkit-slider-runnable-track { box-sizing: border-box; display: block; }\n"
    "input[type=\"color\"] { "
    "appearance: auto; width: 50px; height: 27px; padding: 1px 2px; "
    "border: 1px solid #767676; border-radius: 2px; background-color: #efefef; cursor: default; }\n"
    "input[type=\"color\"]::-webkit-color-swatch-wrapper { padding: 4px 2px; box-sizing: border-box; width: 100%; height: 100%; }\n"
    "input[type=\"color\"]::-webkit-color-swatch { border: 1px solid #777777; width: 100%; height: 100%; }\n"
    "input[type=\"number\"] { appearance: auto; }\n"
    "input[type=\"number\"]::-webkit-inner-spin-button, input[type=\"number\"]::-webkit-outer-spin-button { "
    "appearance: auto; display: inline-block; cursor: default; width: 15px; height: 100%; }\n"
    "input[type=\"button\"], input[type=\"submit\"], input[type=\"reset\"], button { "
    "appearance: auto; background-color: #efefef; color: #000000; border: 2px outset #767676; "
    "padding: 1px 6px; cursor: default; text-align: center; box-sizing: border-box; }\n"
    "select { appearance: menulist; box-sizing: border-box; border: 1px solid #767676; "
    "border-radius: 0; padding: 0; background-color: #ffffff; color: #000000; cursor: default; "
    "white-space: nowrap; align-items: center; }\n"
    "option { display: none; white-space: nowrap; min-height: 1.2em; padding: 0 2px 1px; }\n"
    "label { cursor: default; }\n"
    "textarea { white-space: pre-wrap; overflow-wrap: break-word; resize: auto; "
    "overflow: auto; font-family: monospace; border: 1px solid #767676; "
    "padding: 2px; cursor: text; }\n"
    "textarea::placeholder { color: #757575; }\n"
    "input::-webkit-input-placeholder { color: #757575; }\n"
    "input:focus, select:focus { outline: auto; }\n";

#endif /* KN_UA_CSS_H */
