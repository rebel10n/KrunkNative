"use strict";
const process = {}
const require = {}
const SM = (m) => m.replace(/\bthis\.GAME\b/g, "GAME").replace(/V_(\w+)/g, "$1").replace(/\bdelete\b/g, "remove").replace(/of #<Object>/g, "");
const EH = (m) => {throw new Error("RUNTIME ERROR: "+SM(m).replace(/^RUNTIME ERROR: /g, ""))};
const RE = (l,c,m) => {throw new Error(`${l}:${c} ${m}`)};
const RP = (s) => s.replace(/V_(\w+)/g, "$1");
const L = (x) => (x.length||0)
const NE = (x) => !!Object.keys(x||{}).length
const B = (x) => !!x
const N = (x) => (parseInt(x)||0);
const F = (x) => (parseFloat(x)||0);
const S = (x) => (x+"");
const O = (x) => (typeof x==="object"&&!Array.isArray(x)?x:{});
const A = (x,f) => (Array.isArray(x)?(x.forEach((v,i)=>x[i]=A(v,f)),x):f(x));
const HP = (x,p) => x.hasOwnProperty(p);
const BC = (a,i,l,c) => (a.hasOwnProperty(N(i))?N(i):RE(l,c,"Index out of range."));
const FC = (f,p,l,c) => (typeof f==="function"?f:RE(l,c,`Property "${p}" is not an an "action".`));
const DE = (x,i) => (Array.isArray(x)?x.splice(i,1):0);
const OE = (x) => Object.entries(x||{}).map(p=>{return{V_key:RP(p[0]),V_value:p[1]}});
let V_UTILS = this.GAME.V_UTILS;
let V_Math = this.GAME.V_UTILS.V_math;
let LC = 0;
let V_usingMenu = false;
let V_playerSelected = 0;
let V_commandSelected = 0;
let V_playerShifted = 0;
let V_admins = ["Iniquity_Games", "HellFir3", "SITF21", "MinerZapped", "L-U-C-A-07", "Sorable", "QuackyTTV", "MoonJuiceAlt", "west2nv"];
let V_check = false;
function V_render(V_delta) {
try {
LC = 0;
if (V_usingMenu) {
this.GAME.V_OVERLAY.V_drawRect.bind(this)(490, 90, 1055, 620, 0, "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(500, 100, 1035, 600, 0, "#fff", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("Code INIQ", 1000, 130, 0, 30, "center", "#000", 1);
let V_players = this.GAME.V_PLAYERS.V_list.bind(this)();
this.GAME.V_OVERLAY.V_drawRect.bind(this)(545, 145, 945, 495, 0, "#ddd", 1);
for (let V_i = 0; V_i < ((L(V_players)) - V_playerShifted) && V_i < 14; (V_i)++) {
if (LC++ > 1e+6) RE(22,8,"Loop timed out.");
if ((V_i % 2) === 0) {
this.GAME.V_OVERLAY.V_drawRect.bind(this)(550, 150 + (35 * V_i), 935, 30, 0, "#999", 1);
};
if ((V_i % 2) === 1) {
this.GAME.V_OVERLAY.V_drawRect.bind(this)(550, 150 + (35 * V_i), 935, 30, 0, "#666", 1);
};
this.GAME.V_OVERLAY.V_drawText.bind(this)(V_players[BC(V_players,V_i + V_playerShifted,29,34)].V_username, 600, 170 + (35 * V_i), 0, 15, "left", "#000", 1);
if (V_i === (V_playerSelected - V_playerShifted)) {
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1237 + 35 * V_commandSelected, 152 + 35 * (V_playerSelected - V_playerShifted), 36, 26, 0, "#333", 1);
};
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1240, 155 + (35 * V_i), 30, 20, 0, "#80f", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("tpto", 1255, 167 + (35 * V_i), 0, 11, "center", "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1275, 155 + (35 * V_i), 30, 20, 0, "#00f", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("tpme", 1290, 167 + (35 * V_i), 0, 11, "center", "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1310, 155 + (35 * V_i), 30, 20, 0, "#0ff", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("fix", 1325, 167 + (35 * V_i), 0, 11, "center", "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1345, 155 + (35 * V_i), 30, 20, 0, "#0f0", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("kill", 1360, 167 + (35 * V_i), 0, 11, "center", "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1380, 155 + (35 * V_i), 30, 20, 0, "#ff0", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("kick", 1395, 167 + (35 * V_i), 0, 11, "center", "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1415, 155 + (35 * V_i), 30, 20, 0, "#f00", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("ban", 1430, 167 + (35 * V_i), 0, 11, "center", "#000", 1);
this.GAME.V_OVERLAY.V_drawRect.bind(this)(1450, 155 + (35 * V_i), 30, 20, 0, "#000", 1);
this.GAME.V_OVERLAY.V_drawText.bind(this)("+", 1465, 167 + (35 * V_i), 0, 11, "center", "#fff", 1);
};
};
} catch (e) {EH(e.message)};
}
function V_onKeyPress(V_key, V_code) {
try {
LC = 0;
let V_player = this.GAME.V_PLAYERS.V_getSelf.bind(this)();
if (V_key === "o") {
V_check = false;
V_playerSelected = 0;
V_commandSelected = 0;
V_playerShifted = 0;
for (let V_i = 0; V_i < L(V_admins); (V_i)++) {
if (LC++ > 1e+6) RE(59,8,"Loop timed out.");
if (S(V_player.V_username) === V_admins[BC(V_admins,V_i,60,40)]) {
V_check = true;
};
};
if (V_check) {
V_usingMenu = !V_usingMenu;
};
};
if (V_usingMenu) {
let V_players = this.GAME.V_PLAYERS.V_list.bind(this)();
if (V_key === "=") {
if (V_commandSelected < 6) {
(V_commandSelected)++;
} else {
V_commandSelected = 0;
};
};
if (V_key === "-") {
if (V_commandSelected > 0) {
(V_commandSelected)--;
} else {
V_commandSelected = 6;
};
};
if (V_key === "p") {
this.GAME.V_NETWORK.V_send.bind(this)(S(V_commandSelected), {V_playerSelected: V_players[BC(V_players,V_playerSelected,87,71)].V_id});
this.GAME.V_log.bind(this)(V_players[BC(V_players,V_playerSelected,88,21)].V_username);
};
};
} catch (e) {EH(e.message)};
}
function V_onMouseScroll(V_dir) {
try {
LC = 0;
if (V_usingMenu) {
let V_players = this.GAME.V_PLAYERS.V_list.bind(this)();
if (V_dir === 1) {
if (V_playerSelected > 0) {
(V_playerSelected)--;
};
if (V_playerShifted > V_playerSelected) {
(V_playerShifted)--;
};
};
if (V_dir === -1) {
if (V_playerSelected < (L(V_players)) - 1) {
(V_playerSelected)++;
};
if (V_playerShifted <= V_playerSelected - 14) {
(V_playerShifted)++;
};
};
};
} catch (e) {EH(e.message)};
}
return {render: V_render.bind(this), onKeyPress: V_onKeyPress.bind(this), onMouseScroll: V_onMouseScroll.bind(this), start: function(){}, update: function(){}, onPlayerSpawn: function(){}, onPlayerDeath: function(){}, onPlayerUpdate: function(){}, onKeyUp: function(){}, onKeyHeld: function(){}, onControllerPress: function(){}, onControllerUp: function(){}, onControllerHeld: function(){}, onMouseClick: function(){}, onMouseUp: function(){}, onDIVClicked: function(){}, onNetworkMessage: function(){}};