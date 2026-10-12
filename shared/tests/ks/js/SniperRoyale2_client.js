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
function V_onNetworkMessage(V_id, V_data) {
try {
LC = 0;
if (V_id === 'customSky') {
this.GAME.V_SCENE.V_setSkyDome.bind(this)('', '', '', {V_texture: A(V_data.V_Id,F)});
};
} catch (e) {EH(e.message)};
}
return {onNetworkMessage: V_onNetworkMessage.bind(this), start: function(){}, update: function(){}, render: function(){}, onPlayerSpawn: function(){}, onPlayerDeath: function(){}, onPlayerUpdate: function(){}, onKeyPress: function(){}, onKeyUp: function(){}, onKeyHeld: function(){}, onControllerPress: function(){}, onControllerUp: function(){}, onControllerHeld: function(){}, onMouseClick: function(){}, onMouseUp: function(){}, onMouseScroll: function(){}, onDIVClicked: function(){}};