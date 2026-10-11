# Krunker v3.1.1 UI: feature inventory (engine spec)

Machine-counted (2026-10-10) over ui/index.html, ui/css/*.css and the window
HTML strings in ui/js/krunker-windows.js. This is what a JS-free HTML/CSS
engine must implement to render the menu and HUD with full parity. Reference
renders: doc/ref/krunker-v3.1.1/.

## Headline numbers

| | |
|---|---|
| HTML fragments | 23: index.html, 12 window strings, 3 per-item templates (class card, map item, host map card), settings head + 6 tabs |
| Elements | 4,119 across 22 tag types (index.html alone: 447) |
| Inline event attributes | 979, calling 60 distinct functions (+2 bare expressions) |
| Form controls | 780 `<input>` (436 range, 226 checkbox, 74 number, 15 color, 17 text, 11 url, 1 password), 21 `<select>` / 459 `<option>`, 226 wrapping `<label>` (0 `for`), 0 button/textarea |
| Inline `style=""` | 980 attributes, 35 properties |
| Stylesheets | 6 files, 9,570 lines, 1,372 rules, 1,429 selectors, 5,835 declarations, 129 distinct properties, 145 `!important` |
| At-rules | 6 `@font-face`, 13 `@keyframes`; no `@media`/`@import`/`@supports`/`@layer` |
| Live CSS | ~606 of 1,429 selectors can match shipped + generated markup; all 241 flag.css selectors are dead |

## Surprises / traps

1. Host Custom Game window is 138 KB, 2,111 elements, 508 inputs (343 custom-weapon sliders).
2. Seven inline handlers are undefined in the current JS: `updateWSliderLabel` (343), `updateSliderLabel` (19), `changeContSet` (6), `aspectSelect`, `windows[7].updateSet` (18), `windows[7].changeWep`, `windows[7].toggleWep`.
3. Secular One font is loaded but never used (files missing); Material Icons is the only WOFF2 that matters.
4. `z-index` values exceed int32 (`99999999999999999999`): clamp to 2147483647 like Chrome, ties by document order.
5. `#chatList` and `.classList` use `direction: rtl` to put the scrollbar on the left; `.classList` is `overflow-y: scroll`.
6. `position: sticky` (`.settingsHeader`, `.serverHeader`) inside the `#menuWindow` scroller.
7. `#knPreviewHole` uses `box-shadow: 0 0 0 4000px rgba(0,0,0,.5)` clipped by `overflow:hidden`.
8. Malformed markup to parse like Chrome: `</br>` (= `<br>`), stray `</img>`/`</input>`, `type="undefined"`/`type="input"` (= text), 564 `&quot;` in handler attributes.
9. Duplicate ids (`customWSet110..120`, `androidPayB`, `endAMerger`, `endAContainer`, `setHed_mobile`, `setBod_mobile`): first match wins.
10. `animation: sprayAnm` has no keyframes. `@keyframes grow` has only `from` and animates `font-size` (re-layout every frame, `0.8s infinite alternate` on `#instructions`).
11. Missing assets: Secular One woff2 ×3, `img/world_0.png`, `img/chals/icon_0.png`, fandom-*.png, lock-icon-*.svg, dropper_*.svg (render as nothing, like Chrome).

## 1. HTML

Tags: div 1,712, input 780, span 765, option 459, label 226, i 56, img 43, select 21, a 18, br 6 (+`</br>`), canvas 4, video 1, b 3, li/ul 3/1, link/meta/script 7/5/5. No table/button/textarea/iframe/svg/p/h*.

Behaviour attributes (979): oninput 467 (input), onclick 294 (div 107, span 90, input 84, a 6, i 4, img 3), onkeypress 74 (`return delayExecute(...)` on number inputs), onmouseenter 60 (`playTick()`), onmouseover 42, onchange 38 (select 19, input 19), oncontextmenu 2, onfocus/onblur 1/1 (`focusChat`).

Handlers (become C handlers): updateWSliderLabel 343, setSetting 193, playTick 98, delayExecute 74, unbindCont/changeCont 38/38, showWindow 34, windows[0].collapseFolder 22, updateSliderLabel 19, windows[7].updateSet 18, openURL 10, changeContSet / windows[3].switchTab / windows[9].switchTab / windows[0].changeTab 6 each, selectSkinColor 5, SOUND.play 4, windows[2].action / windows[13].premium / windows[7].presetAction 3 each, checkTerms, openHostWindow, focusChat, windows[1].searchList, setExpandedRegion, quickJoinRegion, selectHostMap, windows[7].toggle 2 each, and 30 singletons (sub2FA, bdayPlay, openTab, copyInviteLink, openJoinWindow, toggleTeam, claimReward, toggleSpect, shoPolicePop, windows[13].purchase, window.open, clearPops, closeTwitch, sendReaction, toggleHF, skinSelector, selectStat, registerAcc, loginAcc, selectClass, playMap, favoriteM, createPrivateRoom, joinGame, resetSettings, exportSettings, importSettings, selectSettingPre, windows[0].searchList, aspectSelect, windows[7].changeWep/toggleWep). Dynamic: knRetry, knHostOffline, knHostMap.

Form controls: range `.sliderM` (track 15px #a8a8a8; `::-webkit-slider-thumb` 15x25 #2196f3 radius 4; appearance none); checkbox as switches (hidden input inside `label.switch` 187, `.hostOpt` 35, `switch2`/`switchsml` 3, `.hostMap` 1+15) styled via `input:checked + .slider:before`; number `.sliderVal` with spin buttons hidden; color 55x24 with 4px `var(--blue)` border and `::-webkit-color-swatch`; text/url/password with placeholder (27), maxlength (4), autocomplete off; select with `onchange` (19) and `selected` options (19). Other attrs: title 192 (tooltips), loading=lazy 17, checked 80, disabled 1, tabindex 1, draggable 1, align=center 2.

Inline styles (980): margin-left 453, margin-bottom 134, margin 116, color 104 (`var(--red)` 41), margin-right 82, display 77 (none 59, block 9, flex 5, inline-block 4), border-width 74, font-size 61 (13–40px), margin-top 36, float 30, width/height 17/12, padding* ~19 (incl. `1em`), text-align, background-color, border:none, background, vertical-align, border-radius ≤7 each, flex-grow/flex-basis/-webkit-box-flex 4/3/3, cursor, position, opacity, pointer-events, z-index, left/bottom/top, image-rendering:pixelated, overflow, list-style ≤3. JS injects: `width:N%`, background-color, opacity, margin-right, color.

Media: 43 `<img>` (5 without src: render nothing); 4 `<canvas>` (transparent boxes; `#classPreviewCanvas` 660x350); 1 `<video>` (no source). Entities: `&amp;` 11, `&nbsp;` 1, `&quot;` 564 in attributes; U+200E inserted around names.

## 2. CSS properties (129 distinct)

| Property | Uses | Values / syntax |
|---|---:|---|
| color | 388 | #hex, `rgba(r, g, b, a)`, 3-arg `rgba()` ×3, `rgb(255 255 255)`, `var(--yellow/--green)`, named |
| width | 337 | px, %, auto, 0, `1.5em`, `calc(100% - 35px)` (+ -moz/-webkit/-o-calc) |
| font-size | 328 | 0–165px, `0.5em` |
| position | 302 | absolute 231, relative 65, fixed 2, sticky 2 (+2 -webkit-sticky) |
| background-color | 290 | 75 values incl. transparent, `var()` |
| display | 288 | inline-block 150, none 65, flex 26, block 25, grid 10, table 7, table-cell 4, inherit 1 |
| background-position | 245 | 241 flag sprites (dead), center, `right 8px center`, `background-position-x: calc(50% - 5px), center` |
| border-radius | 230 | single and 4-value forms, no slash; also border-top-left-radius / border-bottom-right-radius |
| height | 225 | px/%, `min-content` ×2 |
| text-align | 180 | center/left/right |
| top/left/right/bottom | 173/167/65/62 | px, %, negatives, unset, calc |
| padding (+sides) | 172 (+96/76/38/43) | 1–4 values |
| z-index | 146 | -1 … 1e20 (clamp int32) |
| margin-* / margin | 145/95/84/82 / 26 | negatives, auto, -50%, -100% |
| transform | 143 | see list |
| pointer-events | 132 | all 56, auto 39, none 37 |
| cursor | 132 | pointer, default, none, crosshair |
| text-shadow | 83 | 4/8/12/16-layer zero-blur outlines (±1–4px), `0 Npx 0` stacked bevels (5–10 layers), `#000 0 0 3px`, none, unset |
| vertical-align | 82 | top 40, middle 25, bottom 13, sub 2, text-top, super |
| box-shadow | 55 | inset bottom bars (`inset 0 -7px 0 0 #b3b3b3`), `0 0 5px 1px`, `0 9px 0 0`, `#000 0 0 1000px inset`, `0 0 0 4000px`, `0 0 0 2px`; blur 0–8px; one shadow per value |
| opacity | 54 | 0–1 |
| float / clear | 52 / 6 | right 40, left 11 / none, both |
| background | 48 | colors; `linear-gradient(to right, …)` ×5 (+legacy -moz/-webkit `left`), % stops incl. hard stops; `radial-gradient(ellipse at 50% 40%, …)`; `url() no-repeat right 8px center` |
| background-image | 47 | url() PNG; two multi-layer values |
| border (+sides) | 45 (+9/4/4/2) | solid/dashed/dotted; transparent borders as CSS triangles; `rgb(255 255 255/12%)` |
| transition (+prefixed) | 43 (+24) | `all 0.15s ease-in-out`, `all 0.2s`, `0.4s`, `transform 0.1s ease-out`, `0.2s opacity`, `width 1s ease-in-out`, `opacity 0.5s linear`, `line-height`/`font-size 0.2s`; ease, ease-out, ease-in-out, linear |
| overflow / -y / -x | 41 / 12 / 2 | hidden, visible / auto 8, scroll 2, hidden 2 / hidden |
| line-height | 41 | px, 0, unitless 1 |
| image-rendering | 40 | pixelated 18, crisp-edges 9 (+prefixed), auto 4 |
| white-space | 28 | nowrap |
| background-size | 25 | cover 13, contain 6, `100% 100%`, `0% 100%` (JS `N% auto`), `18px auto`, `90%, 100%`, `60%, contain` |
| align-items / justify-content / flex-direction / flex-grow / flex-wrap | 22 / 14 / 21 / 3 / 1 | center, flex-end, flex-start / center, space-between, space-around / column 12, row 9 / 1 / wrap |
| text-overflow | 21 | ellipsis |
| filter (+-webkit-) | 19 (+11) | grayscale, brightness(0/1.2), contrast(0), saturate(40), hue-rotate(-40deg), opacity(0.5), drop-shadow chains ×4 (zero blur), drop-shadow 7px/10px blur |
| will-change | 16 | transform (ignore) |
| text-decoration | 15 | underline, none |
| animation (+prefixed, longhands) | 15 (+8, +4) | linear, ease-in-out, `steps(n)`, `cubic-bezier(0.5,0,0.5,1)`; alternate, infinite, delay, fill both; negative delays |
| background-repeat | 11 | no-repeat, round, `no-repeat, no-repeat` |
| box-sizing | 11 | border-box |
| outline | 11 | none |
| max-height / max-width / min-width / min-height | 10 / 7 / 5 / 3 | `calc(100% - 480px/461px/390px)` |
| grid-template-columns / row-gap / column-gap | 9 / 5 / 4 | auto tracks, px tracks (`40px 50px auto 50px 40px`), `calc(100%/4)` ×4; gaps 10px; no fr/repeat/areas |
| content | 8 | `""` ×7, `attr(data-badge)` |
| font-family | 5 | gamefont (`*` rule), headerfont, inherit, `material icons` |
| text-transform | 5 | uppercase, none |
| user-select (+prefixed) | 3 (+12) | none (`*`), text (inputs) |
| appearance (+prefixed) | 2 (+5) | none, textfield |
| word-wrap / word-break / overflow-wrap | 4 / 1 / 1 | break-word / break-all / break-word |
| border-collapse / border-spacing | 4 / 2 | collapse, separate / 0, 10px |
| direction | 3 | rtl ×2, ltr |
| font-weight | 3 | 900, 999, 400 (+`<b>`): synthetic bold needed (single face per family) |
| font-style | 2 | italic (synthetic oblique), normal |
| letter-spacing | 2 | -2px (#ammoDisplay), normal |
| clip-path | 2 | `polygon(0 0, 100% 0, 100% 100%, 11% 100%, 0 0)` and mirror |
| object-fit | 1 | cover |
| visibility | 1 | hidden (JS toggles) |
| transform-origin | 1 | `0 0` (#uiBase); default center elsewhere |
| custom properties | 9 | `--yellow --blue --green --red` on :root; `--kn-cross-size --kn-hit-len --kn-hit-thick --kn-hit-spacing --kn-hit-color` |

No `font` shorthand, no `gap` shorthand, no `order`/`align-self`/`flex` shorthand, no `hsl()`, no rem/vw/vh/fr/ch/pt.

transform values (44): `translate(-50%,-50%)` ×35, `translateX(-50%)` ×14, `translateZ(0)` ×14, `translate(-50%,0)` ×8, translateY(-60%/-50%/-75%/-70%/-63%/9px/-96px/-180px/25px), translateX(13px/30px/20px), scale(0…1.6), scaleX(-1) ×4, rotate(90deg/45deg), composites `translate(-50%,-50%) scale(1.1)`, `translateX(-50%) rotate(6.5deg)`, `translate(-50%,-50%) rotate(-35deg)`, `translateY(-50%) scale(1.1)`, `translateX(-50%) translateY(-50%)`, unset. Percentages refer to the element's own box.

calc() (32): `100% - 35px`, `50% + 339px`, `100% - 480px/461px/390px/135px/40px`, `50% - 130px`, `50% + 705px - 210px`, `100% / 4`, `20%/80%/40%/60%/50% - 5px`, nested with var(): `var(--kn-cross-size) / -2`, `var(--kn-cross-size) / 2 - 21px`, `var(--kn-hit-len) / -2 ± var(--kn-hit-spacing)`, `var(--kn-hit-thick) / -2`. var(): 30 uses, no fallbacks.

Units: px 3,706; 0 1,864; other unitless 997; % 534; s 92; deg 17; em 7; ms 2. Colors: #rrggbb 698, #rgb 332, rgba() 370, rgb() space syntax 2, named (transparent, black, Crimson, orange, lime, MediumBlue, red, grey, gray, lightgrey, green).

@keyframes (13): blinkTextCursor (border-right-color), typewriter (width 0→25.25em), grow (from only: opacity .3, font-size 30px; `#instructions 0.8s infinite alternate`, LIVE), lds-ring (rotate 0→360, negative delays), spin (rotate), scaleBob (scale 1→.9; `#merchImg` LIVE, #claimImg), rainbow/rainbowT/rainbowB/rainbowBG (6 frames: color/border-color/background-color), rgbHue (`filter: hue-rotate(10→360deg) brightness(2)`, steps(36)), poT (5 frames color), shake (20–100% translate ±6–10px). `sprayAnm` referenced, undefined.

## 3. Selectors

1,429 selectors: 965 with class, 469 with id, 105 with type; universal `*` ×4 (`*` sets outline none, gamefont, color #353535, user-select none). Combinators: descendant 92, child 20, adjacent `+` 24, general `~` 0. Attribute: `[type="color"]` ×4, `[type="number"]`, `[data-badge]`, `[src*="captcha"]`. Pseudo-classes: :hover 125 (incl. `input:hover + .x`, `.menuItem:hover .iconX`), :active 16, :checked 10 (`input:checked + .sibling`), :visited 4, :nth-child(1/2/3) 3, :disabled 3, :focus 3, :not() 5 (`:not(.kn-menu-open)`, `:not(:placeholder-shown)`, `:not(:first-child)`, `:not(:disabled)`), :placeholder-shown, :first-child, :root. Pseudo-elements: :before 11, :after 5, `::-webkit-scrollbar/-track/-thumb` 2 each, `::-webkit-slider-thumb`, `::-moz-range-thumb`, `::-webkit-inner/outer-spin-button`, `::-webkit-color-swatch/-wrapper`, `::placeholder`. `!important` 145. Max specificity (2,1,0).

@font-face: gamefont → `css/fonts/font2.ttf` (40,760 B); headerfont → `css/fonts/header.ttf` (22,096 B); Material Icons → woff2 (128,352 B); Secular One ×3 woff2 unicode-range (missing, unused).

## 4. Fonts

WOFF2 only needed for Material Icons: convert to TTF at import (or subset) to avoid a brotli decoder. Ligatures used (17): delete_forever ×41, keyboard_arrow_down ×23, add_circle_outline ×3, clear ×3, signal_cellular_alt ×2, navigate_next ×2, colorize, person, email, mic, star, keyboard_arrow_up, remove_red_eye, play_arrow, grade, check_circle; dynamic: keyboard_arrow_right. Implement via name→PUA codepoint table (no shaper needed). Synthetic bold (400/900/999/`<b>`) and oblique required.

## 5. Images

All raster assets are PNG (color types 2, 3 with tRNS, 6). Only `img/medal.svg` exists (dead rule) → no SVG rasteriser needed. Class icons 8x8 upscaled with `image-rendering: pixelated` (essential). Map thumbs 200x80 with `object-fit: cover`. menu-icons 128x128 `background-size: contain` with hover swap. social-buttons 80x92 sprites. logo_4 1024x302, merch 512x512, phone_r 256x512, recticle 1024x1024, skulls 32–64px, timer 32x32, weapons/melee 256x128, giveaway 1027x578 (cover).

## 6. Layout

- Flex: row/column, wrap only `.headerBar`; justify center/space-between/space-around; align center/flex-end/flex-start; `flex-grow:1` (`.menuItem`), `flex-basis:200px` inline. No gap/order/align-self.
- Grid: `grid-template-columns` only (auto, px, calc-% tracks), row/column-gap 10px. Live: `#settingsTabLayout` (6 auto columns); 9 others in profile/end screens.
- Tables: `display: table/table-cell` for `.menuTabsNew/.menuTabNew`, `.serverHeader/.sideTabs/.centerTab`. No `<table>`.
- Positioning: fixed (#canvas, #loadingBg); sticky (.settingsHeader, .serverHeader in #menuWindow); floats 51 CSS + 30 inline + clear both; inline-block 150 → real line boxes with vertical-align.
- Scroll containers: #menuWindow (max-height `calc(100% - 480px)` / `- 461px` with `.stickyHeader`), #chatList (rtl), .classList (rtl, scroll), #tradeChatHolder, #terminalHolder, #newLeaderDisplay, #endTable, #supportMapCreator, .tList, .tabBody. Scrollbars: global `::-webkit-scrollbar` 12px, track #f5f5f5 radius 10 `inset 0 0 6px`, thumb #868686 radius 10 `inset 0 0 2px`; #chatList 8px rgba(0,0,0,.4) square. JS sets `scrollTop = scrollHeight` on #chatList.
- scaleBase(): `custom = 2 - scaleUI` (0.8 → 1.2); `dw = 1700*custom`, `dh = 900*custom`; `sx = innerWidth/dw`, `sy = innerHeight/dh`; if `sy < sx`: scale(sy), width `innerWidth/sy`, height `dh`; else scale(sx), width `dw`, height `innerHeight/sx`; factors `toFixed(3)`; `transform-origin: 0 0` on #uiBase; `uiScaling === false` clears all; re-run on resize and on `scaleUI`/`uiScaling` setting changes, then previewRect.
- Geometry queries: getBoundingClientRect on #classPreviewCanvas and #instructionHolder; offsetWidth (#instructionHolder → scale k); offsetHeight (#uiBase); ResizeObserver on #classPreviewCanvas, #menuClassContainer, #uiBase.
- Focus/caret/selection: ~30 text-like inputs with caret, placeholder, maxlength (70/36), `::placeholder`/`:placeholder-shown`; selection only inside inputs; `:focus + .slider`; blur() on Escape; focus() on #settSearch; onfocus/onblur on #chatInput; range dragging; select popup; color picker (or swatch); checkbox toggle via wrapping label.
- Hover/active: 125 `:hover`, 16 `:active`, e.g. `.button:hover { scale(.95) }`, `.menuItem:hover { border-left: 10px }` + icon swap; transitions on 43 rules.

## 7. Dynamic behaviour (DOM-ops API surface)

| Operation | krunknative.js | hud.js | Targets |
|---|---:|---:|---|
| style.display | 28 | 46 | menuHolder, menuHider, speakerDisplay, supportMapCreator, gameNameHolder, spectButton, spinUI, respinUI, endUI, instructionHolder, baseLinks, newLeaderDisplay, terminalHolder, windowHolder, loadingBg, consentBlock, bdayBlock, menuFPSDisplay, teamSelector, `.settName`/`.setHed`/`setBod_*`; ~40 HUD ids (inGameUI, aimRecticle, overlay, vignette, topLeftHolder, topRight, bottomLeftHolder, botRightHider, weapDisplay, bottomRight, igXPBar, chatUI, killCardHolder, leaderDisplay, kill/death/streak/scoreCount, fpsDisplay, pingDisplay, killFeed, ammoDisplay, reloadMsg, timerDisplay, …) |
| style.visibility / opacity / transform | 2 / 1 / 6 | – / 10 / – | menuClassContainer; vignette, teamName, igXPBar, chatList, aimRecticle, knCrosshair, knHitmarker, weapon icons; `#uiBase scale()`, `#bubbleContainer scale(1/0)` |
| style width/height/left/top/bottom | 11 / 4 | – | uiBase, classPreviewCanvas, knPreviewHole, chatHolder, voiceDisplay |
| style pointerEvents/overflowY/background/backgroundSize/backgroundColor/color/maxHeight/marginRight | 3 | 5 | instructionHolder, menuWindow, canvas, ammoDisplay (`N% auto`), igXPBarProg, pingIcon, chatList, feed icon |
| style.setProperty(--kn-*) | – | 6 | `--kn-cross-size` per frame; hit-marker vars per animation frame |
| classList toggle/add/remove | 7 | 3 | `html.kn-menu-open`, `html.kn-hud-visible`, `html.kn-preview-bg`, `#uiBase.onMenu/.onGame`, `#instructionHolder.kn-hole`, `#menuWindow.stickyHeader`, `.menuTabNew.tabANew` |
| innerHTML | 10 | 7 | menuWindow (whole windows up to 138 KB), windowHeader, instructions, menuClassSubtext, menuClassNameTag, txtBubble, settHolder; healthValue, healthBar, weapDisplay, curGameInfo, leaderContainerD, chat rows, kill-feed rows |
| insertAdjacentHTML/Element | – | 3 | kill feed |
| textContent / nodeValue | 6 | 10 | menuRegionLabel, menuClassName, inviteButton, accResp, `.plusOrMinus`; ammoVal, ammoMax, timerVal, killsVal, deathsVal, myScoreVal, ingameFPS, pingText |
| createElement/appendChild/removeChild/replaceChild/createTextNode | 3 | 21 | knPreviewHole, knCrosshair(+4), knHitmarker(+4), chat rows, kill-feed rows (cap 8, remove after 4 s) |
| id/className/src writes, img.onerror | 1 | 15 | hudClassImg, challIcon, feed images |
| value read/write, checked | 15 | – | `slid_<key>`, `slid_input_<key>`, accName, settSearch, settings controls |
| getAttribute / querySelector(All) / closest | 6 / 9 / 1 | 2 | onclick/onchange/href/data-ext-href/src; `input[type=range][id^=slid_]`, `input[type=checkbox][onclick]`, `select[onchange]`, `.settName`, `.setHed`, `.plusOrMinus`, `#menuWindow .menuTabNew` |
| focus/blur/scrollTop | 2 | 1 | settSearch, active input, chatList |
| timers | setTimeout 4 | setTimeout 3, setInterval 1, rAF 2 | bubble 2.1 s, invite 1 s, connect timeout 6 s, feed fade 3.5/4 s, hit-marker anim |
| localStorage | knSettings, classindex, knName, consent | – | |

Events: mousedown (#instructionHolder: play / close window; #windowHolder backdrop closes), click (document, intercepts `<a>`), keydown capture (inputs: Escape blurs, Enter on #accName commits; stops propagation), keyup capture, keydown window (Escape closes window / toggles menu), resize (scaleBase), DOMContentLoaded ×3, ResizeObserver ×3, img.onerror ×3.

Bridge events in: menu, state, classes, maps, connect, bridgeReady, hud, killfeed, hit. Calls out: ready, menu, play ×2, previewRect, reconnect, setClass ×2, setMap, setName ×2, setSetting ×4, log. Dynamic ids: `slid_<key>`, `slid_input_<key>`, `setHed_*`/`setBod_*`, `weapItem_N`, `chatMsg_N`, `feed_N`, `gameMapN`, knPreviewHole, knCrosshair, knHitmarker.
