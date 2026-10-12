// Reference harness for the KrunkScript runtime conformance tests (doc/v7/krunkscript.md 6.7):
// each tools/ks/ref/<name>.js is the hand-written v7 lowering (section 1.16) of
// shared/tests/ks/run/<name>.krnk; running it with Node prints the expected <name>.out.
//   node tools/ks/ref/<name>.js > shared/tests/ks/run/<name>.out
// Keys are written un-prefixed (the V_ prefix is only observable in GAME.log, which strips it).
'use strict';
const out = [];
const sanitize = (x) => {
  if (typeof x === 'object') {
    if (Array.isArray(x)) return x.map(sanitize);
    const o = {};
    for (const k in x) o[k] = sanitize(x[k]); // sanitize(null) -> {}
    return o;
  }
  if (typeof x === 'function') return Symbol('action');
  return x;
};
const logStringify = (x) => (typeof x === 'object' ? JSON.stringify(x, null, 2) : x + '');
const SM = (m) => m.replace(/\bthis\.GAME\b/g, 'GAME').replace(/V_(\w+)/g, '$1').replace(/\bdelete\b/g, 'remove').replace(/of #<Object>/g, '');
const EH = (m) => { throw new Error('RUNTIME ERROR: ' + SM(m).replace(/^RUNTIME ERROR: /g, '')); };
const RE = (l, c, m) => { throw new Error(`${l}:${c} ${m}`); };
const L = (x) => (x.length || 0);
const NE = (x) => !!Object.keys(x || {}).length;
const B = (x) => !!x;
const N = (x) => (parseInt(x) || 0);
const F = (x) => (parseFloat(x) || 0);
const S = (x) => (x + '');
const O = (x) => (typeof x === 'object' && !Array.isArray(x) ? x : {});
const A = (x, f) => (Array.isArray(x) ? (x.forEach((v, i) => (x[i] = A(v, f))), x) : f(x));
const HP = (x, p) => x.hasOwnProperty(p);
const BC = (a, i, l, c) => (a.hasOwnProperty(N(i)) ? N(i) : RE(l, c, 'Index out of range.'));
const FC = (f, p, l, c) => (typeof f === 'function' ? f : RE(l, c, `Property "${p}" is not an an "action".`));
const DE = (x, i) => (Array.isArray(x) ? x.splice(i, 1) : 0);
const OE = (x) => Object.entries(x || {}).map((p) => ({ key: p[0], value: p[1] }));
let LC = 0;
const GAME = {
  log: (...args) => {
    try { out.push(args.map(sanitize).map(logStringify).join(' ')); }
    catch (e) { throw new Error('Internal Error "log"'); }
  },
};
// hook(fn, ...args): one host entry (LC reset, error contained and printed with "!")
const hook = (fn, ...args) => {
  try { LC = 0; fn(...args); } catch (e) {
    try { EH(e.message); } catch (e2) { out.push('!' + e2.message); }
  }
};
const loop = (l, c) => { if (LC++ > 1e6) RE(l, c, 'Loop timed out.'); };
const done = () => process.stdout.write(out.map((l) => l + '\n').join(''));
module.exports = { out, sanitize, logStringify, SM, EH, RE, L, NE, B, N, F, S, O, A, HP, BC, FC, DE, OE, GAME, hook, loop, done };
