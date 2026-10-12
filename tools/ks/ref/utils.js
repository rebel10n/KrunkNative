// GAME.UTILS / GAME.UTILS.math / TIME.getReadable exactly as v7.1.4 builds them
// (REF/src/libs/createKrunkScriptApiContext.js:1458-1580 on top of REF/src/libs/utils.js).
'use strict';
const U = {};
U.isNumber = (p) => p !== null && p !== undefined && typeof p == 'number' && !isNaN(p) && isFinite(p);
U.roundToNearest = (a, b) => (a > 0 ? Math.ceil(a / b) * b : a < 0 ? Math.floor(a / b) * b : parseFloat(a));
U.calcPercent = (a, b) => (a / 100) * b;
U.getDistance = (a, b, c, d) => Math.sqrt((c -= a) * c + (d -= b) * d);
U.getD3D = (a, b, c, d, e, f) => { const x = a - d, y = b - e, z = c - f; return Math.sqrt(x * x + y * y + z * z); };
U.getAngleDst = (a, b) => Math.atan2(Math.sin(b - a), Math.cos(a - b));
U.toRad = (a) => a * (Math.PI / 180);
U.toDeg = (a) => a * (180 / Math.PI);
U.getDir = (a, b, c, d) => Math.atan2(b - d, a - c);
U.getDrs = (a, b, c) => { const x = b.x - a.x, y = b.y + (c || 0) - a.y, z = b.z - a.z; const d = Math.sqrt(x * x + z * z); return [-Math.atan2(-x, z), -Math.atan2(y, d)]; };
U.lerp = (a, b, c) => a + (b - a) * c;
U.truncateText = (a, b, c, d) => (U.isNumber(b) ? (a.length > b ? (c ? a.substring(d || 0, b) : a.substring(d || 0, b) + '...') : a + '') : a + '');
U.replaceText = (a, b, c) => (a + '').replace(b + '', c + '');
U.textContains = (a, b) => !!(a + '').includes(b + '');
U.rgbToHex = (r, g, b) => { let v = (r << 16 | g << 8 | b).toString(16); while (v.length < 6) v = '0' + v; return '#' + v; };
U.isHexColor = (p) => /^#([0-9A-F]{3}){1,2}$/i.test(p);
U.hexToRGB = (p) => p.replace(/^#?([a-f\d])([a-f\d])([a-f\d])$/i, (m, r, g, b) => '#' + r + r + g + g + b + b).substring(1).match(/.{2}/g).map((x) => parseInt(x, 16));
U.hexFromHue = (p) => { const a = p / 60, b = (1 - Math.abs((a % 2) - 1)) * 255, c = Math.floor(a);
  return U.rgbToHex(c < 1 || c > 4 ? 255 : c == 1 || c == 4 ? b : 0, c == 0 || c == 3 ? b : c == 1 || c == 2 ? 255 : 0, c == 0 || c == 1 ? 0 : c == 3 || c == 4 ? 255 : b); };
U.invertHex = (p) => { p = (p + '').replace('#', ''); return '#' + (+('0x1' + p) ^ 16777215).toString(16).substr(1).toUpperCase(); };
U.toLower = (p) => (p || '').toString().toLowerCase();
U.toUpper = (p) => (p || '').toString().toUpperCase();
U.getReadableTime = (ms, b, c, d) => {
  if (!ms && c) return c;
  let s = Math.floor(ms / 1000), m = Math.floor(s / 60); s %= 60; let h = Math.floor(m / 60); m %= 60;
  let dd = Math.floor(h / 24); h %= 24; const y = Math.floor(dd / 365); dd %= 365;
  return (y ? y + 'y ' : '') + (dd ? dd + 'd ' : '') + (h ? h + 'h ' : '') + (d ? (m ? m + 'm ' : '') : (m ? m : 0) + 'm ') + (b ? (d ? (s ? s + 's ' : '') : s + 's') : '');
};
let rand = Math.random;
const math = {
  E: Math.E, PI: Math.PI, PI2: Math.PI * 2,
  sin: (x) => Math.sin(x) || 0, sinh: (x) => Math.sinh(x) || 0, cos: (x) => Math.cos(x) || 0, cosh: (x) => Math.cosh(x) || 0,
  tan: (x) => Math.tan(x) || 0, tanh: (x) => Math.tanh(x) || 0, asin: (x) => Math.asin(x) || 0, asinh: (x) => Math.asinh(x) || 0,
  acos: (x) => Math.acos(x) || 0, acosh: (x) => Math.acosh(x) || 0, atan: (x) => Math.atan(x) || 0, atan2: (a, b) => Math.atan2(a, b) || 0,
  atanh: (x) => Math.atanh(x) || 0, log: (x) => Math.log(x) || 0, exp: (x) => Math.exp(x) || 0, pow: (a, b) => Math.pow(a, b) || 0,
  abs: (x) => Math.abs(x) || 0, round: (x) => Math.round(x) || 0, floor: (x) => Math.floor(x) || 0, ceil: (x) => Math.ceil(x) || 0,
  min: (...a) => Math.min(...a) || 0, max: (...a) => Math.max(...a) || 0, hypot: (...a) => Math.hypot(...a) || 0,
  sqrt: (x) => Math.sqrt(x) || 0, trunc: (x) => Math.trunc(x) || 0, lerp: (a, b, c) => U.lerp(a, b, c),
  toRad: (x) => U.toRad(x) || 0, toDeg: (x) => U.toDeg(x) || 0, calcPerc: (a, b) => U.calcPercent(a, b) || 0,
  roundToNearest: (a, b) => (U.isNumber(a) ? U.roundToNearest(a, b) || 0 : 0),
  roundDecimal: (a, b) => (U.isNumber(a) ? Math.round(a * Math.pow(10, b || 0)) / Math.pow(10, b || 0) : 0),
};
module.exports = {
  math,
  getDist2D: (a, b, c, d) => U.getDistance(a, b, c, d) || 0,
  getDist3D: (a, b, c, d, e, f) => U.getD3D(a, b, c, d, e, f) || 0,
  anglDst: (a, b) => U.getAngleDst(a, b) || 0,
  getDir2D: (a, b, c, d) => U.getDir(a, b, c, d) || 0,
  getDir3D: (a, b, c, d, e, f) => { const v = U.getDrs({ x: a, y: b, z: c }, { x: d, y: e, z: f }); return { x: v[0] + Math.PI / 2, y: v[1] }; },
  randInt: (a, b) => Math.floor(rand() * (b - a + 1)) + a || 0,
  randFloat: (a, b) => rand() * (b - a) + a || 0,
  hexFromHue: (p) => U.hexFromHue(p) || '#ffffff',
  invertHex: (p) => U.invertHex(p) || '#ffffff',
  RGBtoHEX: (r, g, b) => U.rgbToHex(U.isNumber(r) ? r : 0, U.isNumber(g) ? g : 0, U.isNumber(b) ? b : 0),
  HEXtoRGB: (p) => { p = U.isHexColor(p) ? p : '#ffffff'; const v = U.hexToRGB(p); return { r: v[0] || 0, g: v[1] || 0, b: v[2] || 0 }; },
  toLower: (p) => U.toLower(p + '') || '',
  toUpper: (p) => U.toUpper(p + '') || '',
  truncateTxt: (a, b, c, d) => { d = Math.abs(U.isNumber(d) ? d : 0); return U.truncateText(a + '', b, !!c, d) || ''; },
  replaceText: (a, b, c) => U.replaceText(a + '', b + '', c + '') || '',
  textContains: (a, b) => U.textContains(a + '', b + ''),
  getReadable: (ms) => U.getReadableTime(ms) || '',
};
