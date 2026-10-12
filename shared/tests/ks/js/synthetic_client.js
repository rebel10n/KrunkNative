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
let V_count = 0;
let V_grid = [[1, 2], [3]];
let V_o = {V_a: 1, "V_b": "x", V_f: V_helper.bind(this)};
let V_label = "n" + S(V_count);
function V_helper() {
return V_count * 2;
}
function V_update(V_delta) {
try {
LC = 0;
let V_list = this.GAME.V_PLAYERS.V_list.bind(this)();
while (V_count < 3) {
if (LC++ > 1e+6) RE(14,4,"Loop timed out.");
(V_count)++;
};
if (V_count === 1) {
V_count = 2;
} else if (V_count !== 3) {
V_count -= 1;
} else {
V_count = V_helper.bind(this)();
};
V_grid[BC(V_grid,0,22,8)].push(V_count);
DE(V_grid, 1);
delete V_o.V_a;
V_o.V_c += 2;
for (let V_e of OE(V_o)) {
if (LC++ > 1e+6) RE(26,4,"Loop timed out.");
this.GAME.V_log.bind(this)(V_e.V_key, HP(V_o, "V_b"), NE(V_list), V_Math.V_floor.bind(this)(1.5) ** 2);
};
FC(V_o.V_f, "f", 30, 6)(1, V_delta > 2 ? "a" : 'b\'c');
this.GAME.V_log.bind(this)(-(-V_delta), L(V_grid[BC(V_grid,0,31,20)]), A(V_grid,F), B(V_count) && !V_count, V_label);
for (let V_i = 0; V_i < 2; V_i += 1) {
if (LC++ > 1e+6) RE(32,4,"Loop timed out.");
if (V_i === 1) {
break;
};
continue;
};
} catch (e) {EH(e.message)};
}
return {update: V_update.bind(this), start: function(){}};
