#!/usr/bin/env node
// Krunker v10 menu markup, component CSS and English strings, extracted STATICALLY from the live
// client bundle (the v10.0.2 client script): the bundle is parsed with acorn (an AST only, nothing
// of it is ever evaluated) and the Svelte 5 mount code of the boot components is walked by a small
// symbolic interpreter over a toy DOM. v10's menu is no longer in index.html: the page is a shell
// and the Svelte components (#mainMenuUIMount, ...) build the menu at boot from template strings.
//
// usage: node tools/ui-import/v10_bundle.js <client.js> <out dir>
// writes <out>/mounts.json   {"<target id>": "<html the component mounts>", ...}
//        <out>/svelte.css    every component stylesheet ({hash, code} blocks), bundle order
//        <out>/i18n-en.json  translations.en (parseText of the inline string), key order kept
//        <out>/report.txt    helper identification, mounts, unknowns the interpreter skipped
//
// What the interpreter knows (identified by shape, not by the minified names): template(html, flags)
// factories, first_child / child / sibling(n) navigation, append (anchor.before(node)), set_text,
// set_attribute, toggle_class, if blocks (condition evaluated; unknown state = falsy, so `!state` is
// true: a component's initial, logged-out state), each blocks (literal arrays only), component calls
// with literal props (prop() getters, `get` accessors), template_effect callbacks (run once), and
// component CSS appends. Runtime state, stores, effects, events and awaits evaluate to UNKNOWN.
'use strict';
const fs = require('fs');
const path = require('path');
// acorn 8.16.0 (pinned in net-bridge/package-lock.json, a ts-node dependency: npm ci in net-bridge/); KN_NODE_MODULES overrides
const acorn = require(path.join(process.env.KN_NODE_MODULES || path.join(__dirname, '../../net-bridge/node_modules'), 'acorn'));

const [, , SRC, OUT] = process.argv;
if (!SRC || !OUT) { process.stderr.write('usage: v10_bundle.js <client.js> <out dir>\n'); process.exit(2); }
const code = fs.readFileSync(SRC, 'utf8');
const ast = acorn.parse(code, { ecmaVersion: 'latest', allowReturnOutsideFunction: true });
const src = (n) => code.slice(n.start, n.end);
const report = [];

// ------------------------------------------------------------------ AST index
const decls = new Map(); // name -> [node] (FunctionDeclaration, or VariableDeclarator init)
const cssBlocks = [];     // {hash, code} object literals, in source order
let i18nText = null;
const mountCalls = [];   // CallExpression mount(Component, {target, props})
const keyOf = (p) => (p.key.type === 'Identifier' ? p.key.name : p.key.value);
function walk(node, fn) {
  const stack = [node];
  while (stack.length) {
    const n = stack.pop();
    if (!n || typeof n.type !== 'string') continue;
    fn(n);
    for (const k in n) {
      if (k === 'type' || k === 'start' || k === 'end') continue;
      const v = n[k];
      if (Array.isArray(v)) { for (let i = v.length - 1; i >= 0; i--) if (v[i] && typeof v[i].type === 'string') stack.push(v[i]); }
      else if (v && typeof v.type === 'string') stack.push(v);
    }
  }
}
const add = (name, node) => { if (!decls.has(name)) decls.set(name, []); decls.get(name).push(node); };
walk(ast, (n) => {
  if (n.type === 'FunctionDeclaration' && n.id) add(n.id.name, n);
  else if (n.type === 'VariableDeclarator' && n.id.type === 'Identifier' && n.init) add(n.id.name, n.init);
  else if (n.type === 'ObjectExpression' && n.properties.length === 2) {
    const ks = n.properties.map((p) => p.type === 'Property' && keyOf(p));
    const v = n.properties.map((p) => p.value);
    if (ks[0] === 'hash' && ks[1] === 'code' && v.every((x) => x.type === 'Literal' && typeof x.value === 'string')) cssBlocks.push({ hash: v[0].value, code: v[1].value });
  } else if (n.type === 'AssignmentExpression' && n.left.type === 'MemberExpression' && n.right.type === 'CallExpression') {
    const l = n.left;
    const prop = l.property.type === 'Literal' ? l.property.value : (!l.computed && l.property.name);
    const obj = l.object;
    const objProp = obj.type === 'MemberExpression' && (obj.property.type === 'Literal' ? obj.property.value : obj.property.name);
    const a = n.right.arguments[0];
    if (prop === 'en' && objProp === 'translations' && a && a.type === 'Literal' && typeof a.value === 'string') i18nText = a.value;
  } else if (n.type === 'CallExpression' && n.arguments.length === 2 && n.arguments[0].type === 'Identifier' && n.arguments[1].type === 'ObjectExpression') {
    if (n.arguments[1].properties.some((p) => p.type === 'Property' && keyOf(p) === 'target')) mountCalls.push(n);
  }
});
// ------------------------------------------------------------------ toy DOM + template parser
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr']);
class TNode {
  constructor(kind, name) { this.kind = kind; this.name = name; this.attrs = []; this.children = []; this.parent = null; this.data = ''; this.ev = {}; }
  get firstChild() { return this.children[0] || null; }
  get nextSibling() { if (!this.parent) return null; const c = this.parent.children; return c[c.indexOf(this) + 1] || null; }
  clone() {
    const n = new TNode(this.kind, this.name);
    n.attrs = this.attrs.map((a) => [a[0], a[1]]); n.data = this.data;
    for (const c of this.children) { const k = c.clone(); k.parent = n; n.children.push(k); }
    return n;
  }
  getAttr(k) { const a = this.attrs.find((x) => x[0] === k); return a ? a[1] : null; }
  setAttr(k, v) { const a = this.attrs.find((x) => x[0] === k); if (v == null) { this.attrs = this.attrs.filter((x) => x[0] !== k); return; } if (a) a[1] = v; else this.attrs.push([k, v]); }
}
function insertBefore(anchor, node) {
  const p = anchor.parent; if (!p) return;
  const nodes = node.kind === 'frag' ? node.children.slice() : [node];
  for (const n of nodes) {
    if (n.parent) n.parent.children.splice(n.parent.children.indexOf(n), 1);
    n.parent = p; p.children.splice(p.children.indexOf(anchor), 0, n);
  }
}
function parseHtml(html) {
  const root = new TNode('frag'); let cur = root; let i = 0;
  const RAW = new Set(['style', 'script', 'textarea', 'title']);
  while (i < html.length) {
    if (html.startsWith('<!--', i)) { const j = html.indexOf('-->', i + 4); const c = new TNode('comment'); c.data = html.slice(i + 4, j < 0 ? html.length : j); c.parent = cur; cur.children.push(c); i = j < 0 ? html.length : j + 3; continue; }
    if (html.startsWith('<!>', i)) { const c = new TNode('comment'); c.parent = cur; cur.children.push(c); i += 3; continue; }
    if (html.startsWith('</', i)) {
      const j = html.indexOf('>', i); const name = html.slice(i + 2, j).trim().toLowerCase();
      let n = cur; while (n && n !== root && n.name !== name) n = n.parent;
      if (n && n !== root) cur = n.parent;
      i = j + 1; continue;
    }
    if (html[i] === '<' && /[A-Za-z]/.test(html[i + 1] || '')) {
      const m = /^<([A-Za-z][\w:-]*)/.exec(html.slice(i, i + 64)); const el = new TNode('el', m[1].toLowerCase());
      i += m[0].length;
      const AT = /\s*([^\s=>\/"']+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/y;
      for (;;) {
        AT.lastIndex = i;
        if (/^\s*\/?>/.test(html.slice(i, i + 64))) break;
        const a = AT.exec(html); if (!a) { i++; continue; }
        el.attrs.push([a[1], a[2] ?? a[3] ?? a[4] ?? '']); i = AT.lastIndex;
      }
      const close = /^\s*(\/?)>/.exec(html.slice(i, i + 64)); i += close[0].length;
      el.parent = cur; cur.children.push(el);
      if (RAW.has(el.name)) { const j = html.toLowerCase().indexOf('</' + el.name, i); const t = new TNode('text'); t.data = html.slice(i, j); t.parent = el; el.children.push(t); i = html.indexOf('>', j) + 1; continue; }
      if (!VOID.has(el.name) && !close[1]) cur = el;
      continue;
    }
    const j = html.indexOf('<', i + 1); const t = new TNode('text'); t.data = html.slice(i, j < 0 ? html.length : j);
    t.parent = cur; cur.children.push(t); i = j < 0 ? html.length : j;
  }
  return root;
}
const escText = (v) => v.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
const escAttr = (v) => String(v).replace(/&(?![#\w]+;)/g, '&amp;').replace(/"/g, '&quot;');
function serialize(n) {
  if (n.kind === 'text') return n.data;
  if (n.kind === 'comment') return n.data ? '<!--' + n.data + '-->' : '';
  const inner = n.children.map(serialize).join('');
  if (n.kind === 'frag') return inner;
  const at = n.attrs.map(([k, v]) => (v === '' && k !== 'class' && k !== 'style' && k !== 'src' && k !== 'alt' ? ' ' + k : ' ' + k + '="' + escAttr(v) + '"')).join('');
  return '<' + n.name + at + '>' + (VOID.has(n.name) ? '' : inner + '</' + n.name + '>');
}
// ------------------------------------------------------------------ static scopes
const parentOf = new Map();
walk(ast, (n) => { for (const k in n) { const v = n[k]; if (k === 'type') continue; if (Array.isArray(v)) v.forEach((c) => c && typeof c.type === 'string' && parentOf.set(c, n)); else if (v && typeof v.type === 'string') parentOf.set(v, n); } });
const isFn = (n) => n.type === 'FunctionDeclaration' || n.type === 'FunctionExpression' || n.type === 'ArrowFunctionExpression' || n.type === 'Program';
const scopeDecls = new Map(); // fn node -> Map(name -> decl node)
function enclosingFn(n) { let p = parentOf.get(n); while (p && !isFn(p)) p = parentOf.get(p); return p || ast; }
function declsOf(fn) {
  let m = scopeDecls.get(fn);
  if (m) return m;
  m = new Map();
  scopeDecls.set(fn, m);
  const body = fn.type === 'Program' ? fn : fn.body;
  const stack = [body];
  while (stack.length) {
    const n = stack.pop();
    if (!n || typeof n.type !== 'string') continue;
    if (n !== body && isFn(n)) { if (n.type === 'FunctionDeclaration' && n.id && !m.has(n.id.name)) m.set(n.id.name, n); continue; }
    if (n.type === 'VariableDeclarator' && n.id.type === 'Identifier' && !m.has(n.id.name)) m.set(n.id.name, n);
    for (const k in n) { if (k === 'type') continue; const v = n[k]; if (Array.isArray(v)) v.forEach((c) => c && stack.push(c)); else if (v && typeof v.type === 'string') stack.push(v); }
  }
  return m;
}

// ------------------------------------------------------------------ values
const UNKNOWN = Object.freeze({ unknown: true });
const isU = (v) => v === UNKNOWN;
class Closure { constructor(node, env, self) { this.node = node; this.env = env; this.self = self; } }
class Native { constructor(name, fn) { this.name = name; this.fn = fn; } }
class Env {
  constructor(fn, parent, executed) { this.fn = fn; this.parent = parent; this.vars = new Map(); this.executed = executed; }
}
const staticEnvs = new Map();
function staticEnv(fn) {
  let e = staticEnvs.get(fn);
  if (!e) { e = new Env(fn, fn.type === 'Program' ? null : staticEnv(enclosingFn(fn)), false); staticEnvs.set(fn, e); }
  return e;
}
class InterpError extends Error {}
class ReturnSig { constructor(v) { this.v = v; } }
const BREAK = { brk: true }, CONTINUE = { cont: true };
let steps = 0;
const STEP_LIMIT = 4e6;
const tick = () => { if (++steps > STEP_LIMIT) throw new InterpError('step budget exhausted'); };
const special = new Map(); // FunctionDeclaration node -> Native (runtime helpers identified by shape)
const unknownCalls = new Map();
// ------------------------------------------------------------------ Svelte runtime helpers by fingerprint
class Sig { constructor(v, fn) { this.v = v; this.fn = fn || null; } }
const ID = '[\\p{L}\\p{N}_$]+'; // minified names are non-ASCII (iíìîìiï)
const getterName = (prop) => { const m = new RegExp('(' + ID + ') = ' + ID + '\\(' + ID + ', [\'"]' + prop + '[\'"]\\)\\[[\'"]get[\'"]\\]', 'u').exec(code); return m && m[1]; };
const FC = getterName('firstChild'), NS = getterName('nextSibling');
const fnDecls = []; walk(ast, (n) => { if (n.type === 'FunctionDeclaration' && n.id) fnDecls.push(n); });
const firstChildOf = (n) => (n instanceof TNode ? n.firstChild : UNKNOWN);
const nextSiblingOf = (n) => (n instanceof TNode ? (n.nextSibling || null) : UNKNOWN);
const callFn = (f, args) => call(f, UNKNOWN, args || []);
const roles = {};
function role(name, re, fn, extra) {
  const hits = fnDecls.filter((d) => { const s = src(d); return re.every((r) => r.test(s)) && (!extra || extra(s, d)); });
  roles[name] = hits.map((h) => h.id.name);
  for (const h of hits) special.set(h, new Native(name, fn));
  return hits;
}
const tmplCache = new Map();
function template(html, flags, svg) {
  const f = Number(isU(flags) || flags === undefined ? 0 : flags);
  return new Native('template-instance', () => {
    let frag = tmplCache.get(html + '\u0000' + svg);
    if (!frag) {
      const body = html.startsWith('<!>') ? '<!>' + html : html;
      frag = parseHtml(svg ? '<svg>' + body + '</svg>' : body);
      if (svg) { const s = frag.children[0]; frag = new TNode('frag'); for (const c of s.children.slice()) { c.parent = frag; frag.children.push(c); } }
      tmplCache.set(html + '\u0000' + svg, frag);
    }
    const c = frag.clone();
    return (f & 1) ? c : c.firstChild;
  });
}
if (FC) role('first-child-getter', [new RegExp('return ' + FC + '\\[["\']call["\']\\]\\(')], (n) => firstChildOf(n));
if (NS) role('next-sibling-getter', [new RegExp('return ' + NS + '\\[["\']call["\']\\]\\(')], (n) => nextSiblingOf(n));
role('template', [/startsWith["']\]\(["']<!>["']\)/, /importNode/], (h, f) => template(String(h), f, false));
role('template-svg', [/startsWith["']\]\(["']<!>["']\)/, /["']svg["']/], (h, f) => template(String(h), f, true), (s) => !/importNode/.test(s));
role('first-child', [/instanceof Comment/], (n) => { const c = firstChildOf(n); return c instanceof TNode && c.kind === 'comment' && c.data === '' ? nextSiblingOf(c) : c; }, (s) => s.length < 300);
const NEXT_SIB = special.size ? [...special].find(([, v]) => v.name === 'next-sibling-getter') : null;
role('sibling', [/for \(var [^;]*arguments/, new RegExp('= ' + (NEXT_SIB ? NEXT_SIB[0].id.name.replace(/[$]/g, '\\$') : 'NONE') + '\\(', 'u')], (n, count) => { let k = isU(count) || count === undefined ? 1 : count; let x = n; while (k-- > 0 && x instanceof TNode) x = nextSiblingOf(x); return x; }, (s) => s.length < 400);
role('append', [/\[["']before["']\]\(/], (anchor, node) => { if (anchor instanceof TNode && node instanceof TNode) insertBefore(anchor, node); }, (s) => s.length < 200);
role('push-context', [/["']p["']: \S+,\s*["']c["']: null,\s*["']e["']: null/], () => undefined);
role('pop-context', [/\[["']e["']\];\s*if \(null !== /], (x) => x, (s) => s.length < 900);
role('event', [/["']capture["']: /, /["']passive["']: /], () => undefined, (s) => s.length < 1500);
role('comment-fragment', [/createDocumentFragment/, /createComment/], () => { const f = new TNode('frag'); for (const k of ['comment', 'text']) { const c = new TNode(k); c.parent = f; f.children.push(c); } return f; });
role('text-node', [/createTextNode/], (t) => { const n = new TNode('text'); n.data = isU(t) || t === undefined ? '' : String(t); return n; }, (s) => s.length < 300);
role('set-text', [/__t/, /nodeValue/], (n, v) => { if (n instanceof TNode && !isU(v) && !(v instanceof Sig)) n.data = escText(v == null ? '' : String(v)); });
role('set-attribute', [/__attributes/, /["']loading["']/], (n, k, v) => { if (n instanceof TNode && typeof k === 'string' && !isU(v)) n.setAttr(k, v == null || v === false ? null : String(v)); });
role('toggle-class', [/classList/, /contains/, /["']add["']/, /["']remove["']/], (n, c, on) => { if (n instanceof TNode && typeof c === 'string' && truthy(on)) { const cls = (n.getAttr('class') || '').split(/\s+/).filter(Boolean); if (!cls.includes(c)) cls.push(c); n.setAttr('class', cls.join(' ')); } }, (s) => s.length < 400);
role('set-style', [/__styles/, /setProperty/], (n, k, v) => { if (n instanceof TNode && typeof k === 'string' && !isU(v) && v != null) n.setAttr('style', ((n.getAttr('style') || '') + ';' + k + ':' + v).replace(/^;/, '')); });
role('if', [/([\p{L}\p{N}_$]+) !== \(\1 = !![\p{L}\p{N}_$]+\(\)\)/u], (anchor, cond, yes, no) => { const c = truthy(callFn(cond)); if (c) callFn(yes, [anchor]); else if (no && !isU(no)) callFn(no, [anchor]); });
role('each', [/["']items["']: new Map\(\)/], (anchor, flags, items, key, render, fallback) => {
  const arr = callFn(items);
  if (Array.isArray(arr) && arr.length) arr.slice(0, 200).forEach((it, i) => callFn(render, [anchor, new Sig(it), new Sig(i)]));
  else if (fallback && !isU(fallback)) callFn(fallback, [anchor]);
});
role('untrack', [/try \{\s*[\p{L}\p{N}_$]+ = null;\s*return [\p{L}\p{N}_$]+\(\);/u], (f) => callFn(f));
role('legacy-pre-effect', [/["']ran["']: !(?:1|0x1)/], (a, b) => { callFn(a); callFn(b); });
role('proxy', [/state_descriptors_fixed/], (v) => v);
role('get', [/state_unsafe_local_read/], (s) => (s instanceof Sig ? (s.fn ? callFn(s.fn) : s.v) : UNKNOWN));
role('set', [/state_unsafe_mutation/], (s, v) => { if (s instanceof Sig) { s.v = v; s.fn = null; } return v; });
role('derived', [/["']children["']: null/, /["']fn["']:/], (f) => new Sig(undefined, f));
role('prop', [/props_invalid_value/], (props, name, flags, fallback) => new Native('prop-getter', () => {
  const v = props && typeof props === 'object' && !isU(props) && !(props instanceof TNode) ? getMember(props, name) : UNKNOWN;
  if (v !== undefined) return v;
  return (Number(flags) & 16) && fallback instanceof Closure ? callFn(fallback) : fallback;
}));
const usedCss = [];
role('append-styles', [/createElement["']\]\(["']style["']\)/, /["']hash["']\]/], (anchor, css) => { if (css && typeof css === 'object' && typeof css.hash === 'string' && !usedCss.includes(css.hash)) usedCss.push(css.hash); }, (s) => s.length < 800);
const CE = fnDecls.filter((d) => /["']nodes_start["']: null/.test(src(d)) && /["']teardown["']: null/.test(src(d))).map((d) => d.id.name);
roles['create-effect'] = CE;
// create_effect(flags, fn, sync): run fn once (template_effect / render_effect / block bodies)
for (const d of fnDecls) if (CE.includes(d.id.name)) special.set(d, new Native('create-effect', (flags, fn) => { if (fn instanceof Closure || fn instanceof Native) callFn(fn); }));
const sourceCreators = fnDecls.filter((d) => /["']reactions["']: null/.test(src(d)) && /["']version["']: 0/.test(src(d)) && src(d).length < 300).map((d) => d.id.name);
roles['source'] = sourceCreators;
for (const d of fnDecls) if (sourceCreators.includes(d.id.name)) special.set(d, new Native('source', (v) => new Sig(v)));
// effect wrappers and state wrappers: one `return F(...)` deep, to a fixpoint
let changed = true;
const effectNames = new Set(CE), stateNames = new Set(sourceCreators);
while (changed) {
  changed = false;
  for (const d of fnDecls) {
    if (special.has(d) && !stateNames.has(d.id.name)) continue;
    const s = src(d);
    if (s.length > 700) continue;
    const callees = [...s.matchAll(/(?<![\p{L}\p{N}_$.])([\p{L}\p{N}_$]+)\(/gu)].map((m) => m[1]);
    if (!effectNames.has(d.id.name) && callees.some((c) => effectNames.has(c)) && /return [\p{L}\p{N}_$]+\(/u.test(s) && !/state_unsafe/.test(s)) {
      effectNames.add(d.id.name); changed = true;
      special.set(d, new Native('effect', (...a) => { const f = a.find((x) => x instanceof Closure || x instanceof Native); if (f) callFn(f); }));
    } else if (!stateNames.has(d.id.name) && !effectNames.has(d.id.name) && callees.some((c) => stateNames.has(c)) && /^function [\p{L}\p{N}_$]+\([\p{L}\p{N}_$]+[,)]/u.test(s) && /return [\p{L}\p{N}_$]+\(/u.test(s) && !/state_unsafe|throw/.test(s)) {
      stateNames.add(d.id.name); changed = true;
      special.set(d, new Native('state', (v) => new Sig(v)));
    }
  }
}
roles['effect'] = [...effectNames]; roles['state'] = [...stateNames];
// the runtime: every function declared between the first and last identified helper
const CORE = ['template', 'first-child', 'append', 'set-text', 'if', 'each', 'get', 'set', 'prop', 'create-effect'];
const helperNodes = [...special].filter(([, v]) => CORE.includes(v.name)).map(([k]) => k);
const RT_LO = Math.min(...helperNodes.map((n) => n.start)), RT_HI = Math.max(...helperNodes.map((n) => n.end));
const isRuntime = (node) => node.start >= RT_LO && node.end <= RT_HI;
// ------------------------------------------------------------------ the evaluator (whitelist: no host access)
function truthy(v) { return !(isU(v) || !v); }
const SAFE_GLOBALS = {
  Math: { min: Math.min, max: Math.max, ceil: Math.ceil, floor: Math.floor, round: Math.round, abs: Math.abs, PI: Math.PI, sin: Math.sin, cos: Math.cos },
  undefined: undefined, NaN: NaN, Infinity: Infinity,
  String: new Native('String', (v) => (isU(v) ? UNKNOWN : String(v))),
  Number: new Native('Number', (v) => (isU(v) ? UNKNOWN : Number(v))),
  Boolean: new Native('Boolean', (v) => truthy(v)),
  Array: { isArray: Array.isArray },
  Object: { keys: (o) => (o && typeof o === 'object' && !isU(o) ? Object.keys(o) : UNKNOWN), entries: (o) => (o && typeof o === 'object' && !isU(o) ? Object.entries(o) : UNKNOWN) },
};
const PURE_STR = new Set(['concat', 'toLowerCase', 'toUpperCase', 'startsWith', 'endsWith', 'includes', 'split', 'trim', 'slice', 'substring', 'indexOf', 'padStart', 'repeat', 'charAt', 'toFixed', 'toString', 'replace']);
const PURE_ARR = new Set(['slice', 'concat', 'join', 'includes', 'indexOf']);
function getMember(obj, key) {
  if (isU(obj) || obj == null || isU(key)) return UNKNOWN;
  if (obj instanceof TNode) {
    if (key === 'firstChild') return firstChildOf(obj);
    if (key === 'nextSibling') return nextSiblingOf(obj);
    return UNKNOWN;
  }
  if (typeof obj === 'string') {
    if (key === 'length') return obj.length;
    if (typeof key === 'number') return obj[key];
    if (PURE_STR.has(key)) return new Native('str.' + key, (...a) => (a.some((x) => isU(x) || (x && typeof x === 'object')) ? UNKNOWN : String.prototype[key].apply(obj, a)));
    return UNKNOWN;
  }
  if (typeof obj === 'number') return key === 'toFixed' || key === 'toString' ? new Native('num', (...a) => Number.prototype[key].apply(obj, a)) : UNKNOWN;
  if (Array.isArray(obj)) {
    if (key === 'length') return obj.length;
    if (typeof key === 'number' || /^\d+$/.test(key)) return obj[key];
    if (PURE_ARR.has(key)) return new Native('arr.' + key, (...a) => Array.prototype[key].apply(obj, a));
    if (key === 'push') return new Native('arr.push', (...a) => obj.push(...a));
    if (key === 'map' || key === 'filter' || key === 'forEach' || key === 'some' || key === 'every' || key === 'find' || key === 'findIndex') {
      return new Native('arr.' + key, (f) => Array.prototype[key].call(obj.slice(0, 500), (x, i) => { const r = callFn(f, [x, i]); return key === 'map' ? r : truthy(r); }));
    }
    return UNKNOWN;
  }
  if (obj instanceof Sig || obj instanceof Closure || obj instanceof Native) return UNKNOWN;
  if (typeof obj === 'object') {
    const d = Object.getOwnPropertyDescriptor(obj, key);
    if (!d) return Object.prototype.hasOwnProperty.call(SAFE_GLOBALS, '') ? UNKNOWN : (obj === SAFE_GLOBALS.Math || obj === SAFE_GLOBALS.Array || obj === SAFE_GLOBALS.Object ? UNKNOWN : undefined);
    if (d.get) return d.get();
    if (typeof d.value === 'function') return new Native('pure', (...a) => (a.some(isU) ? UNKNOWN : d.value(...a)));
    return d.value;
  }
  return UNKNOWN;
}
function setMember(obj, key, v) {
  if (obj instanceof TNode) {
    if (key === 'textContent' && !isU(v)) { obj.children = []; const t = new TNode('text'); t.data = escText(String(v ?? '')); t.parent = obj; obj.children.push(t); }
    else if (key === 'innerHTML' && typeof v === 'string') { obj.children = []; for (const c of parseHtml(v).children) { c.parent = obj; obj.children.push(c); } }
    else if (key === 'className' && typeof v === 'string') obj.setAttr('class', v);
    else if ((key === 'src' || key === 'id' || key === 'alt' || key === 'title') && typeof v === 'string') obj.setAttr(key, v);
    return;
  }
  if (obj && typeof obj === 'object' && !isU(obj) && !(obj instanceof Sig || obj instanceof Closure || obj instanceof Native) && !Object.isFrozen(obj) && !isU(key)) {
    if (Object.values(SAFE_GLOBALS).includes(obj)) return;
    obj[key] = v;
  }
}
function lookup(name, env, node) {
  for (let e = env; e; e = e.parent) {
    if (e.vars.has(name)) return e.vars.get(name);
    const d = declsOf(e.fn).get(name);
    if (!d) continue;
    if (d.type === 'FunctionDeclaration') { const c = new Closure(d, e); e.vars.set(name, c); return c; }
    if (e.executed) return undefined; // declared here, not reached yet (hoisted var)
    // module / outer static scope: evaluate simple initialisers once
    let v = UNKNOWN;
    const init = d.init;
    if (init && (init.type === 'Literal' || init.type === 'ObjectExpression' || init.type === 'ArrayExpression' || init.type === 'FunctionExpression' || init.type === 'ArrowFunctionExpression' || init.type === 'CallExpression' || init.type === 'Identifier')) {
      e.vars.set(name, UNKNOWN); // cycles
      try { v = evalExpr(init, e); } catch (err) { if (!(err instanceof InterpError)) throw err; v = UNKNOWN; }
    }
    e.vars.set(name, v);
    return v;
  }
  if (Object.prototype.hasOwnProperty.call(SAFE_GLOBALS, name)) return SAFE_GLOBALS[name];
  return UNKNOWN;
}
function assign(name, v, env) {
  for (let e = env; e; e = e.parent) {
    if (e.vars.has(name) || declsOf(e.fn).has(name)) { e.vars.set(name, v); return; }
  }
}
function bindPattern(p, v, env) {
  if (p.type === 'Identifier') env.vars.set(p.name, v);
  else if (p.type === 'AssignmentPattern') bindPattern(p.left, v === undefined ? evalExpr(p.right, env) : v, env);
  else if (p.type === 'RestElement') bindPattern(p.argument, Array.isArray(v) ? v : UNKNOWN, env);
  else if (p.type === 'ObjectPattern') for (const q of p.properties) {
    if (q.type === 'RestElement') bindPattern(q.argument, UNKNOWN, env);
    else bindPattern(q.value, getMember(v, q.computed ? evalExpr(q.key, env) : keyOf(q)), env);
  }
  else if (p.type === 'ArrayPattern') p.elements.forEach((q, i) => q && bindPattern(q, Array.isArray(v) ? (q.type === 'RestElement' ? v.slice(i) : v[i]) : UNKNOWN, env));
}
let depth = 0;
function call(f, self, args) {
  tick();
  if (f instanceof Native) return f.fn(...args);
  if (!(f instanceof Closure)) return UNKNOWN;
  const n = f.node;
  const sp = special.get(n);
  if (sp) return sp.fn(...args);
  if (isRuntime(n) && src(n).length > 250) { unknownCalls.set(n.id ? n.id.name : '?', (unknownCalls.get(n.id ? n.id.name : '?') || 0) + 1); return UNKNOWN; }
  if (n.async || n.generator) return UNKNOWN;
  if (depth > 120) return UNKNOWN;
  const env = new Env(n, f.env, true);
  env.vars.set('arguments', args);
  n.params.forEach((p, i) => bindPattern(p, p.type === 'RestElement' ? args.slice(i) : args[i], env));
  depth++;
  try {
    if (n.body.type !== 'BlockStatement') return evalExpr(n.body, env);
    const r = execBlock(n.body.body, env);
    return r instanceof ReturnSig ? r.v : undefined;
  } catch (err) {
    if (err instanceof InterpError && err.message !== 'step budget exhausted') return UNKNOWN;
    throw err;
  } finally { depth--; }
}
function execBlock(stmts, env) {
  for (const s of stmts) {
    const r = exec(s, env);
    if (r) return r;
  }
  return null;
}
function exec(s, env) {
  tick();
  switch (s.type) {
    case 'ExpressionStatement': evalExpr(s.expression, env); return null;
    case 'VariableDeclaration':
      for (const d of s.declarations) bindPattern(d.id, d.init ? evalExpr(d.init, env) : undefined, env);
      return null;
    case 'FunctionDeclaration': env.vars.set(s.id.name, new Closure(s, env)); return null;
    case 'ReturnStatement': return new ReturnSig(s.argument ? evalExpr(s.argument, env) : undefined);
    case 'BlockStatement': return execBlock(s.body, env);
    case 'IfStatement': {
      const c = evalExpr(s.test, env);
      if (truthy(c)) return exec(s.consequent, env);
      return s.alternate ? exec(s.alternate, env) : null;
    }
    case 'TryStatement': {
      try { return exec(s.block, env); } catch (err) {
        if (!(err instanceof InterpError) || err.message === 'step budget exhausted') throw err;
        return s.handler ? exec(s.handler.body, env) : null;
      } finally { if (s.finalizer) exec(s.finalizer, env); }
    }
    case 'ForStatement': {
      if (s.init) s.init.type === 'VariableDeclaration' ? exec(s.init, env) : evalExpr(s.init, env);
      for (let k = 0; k < 500; k++) {
        if (s.test) { const t = evalExpr(s.test, env); if (isU(t)) break; if (!t) break; }
        const r = exec(s.body, env);
        if (r === BREAK) break;
        if (r && r !== CONTINUE) return r;
        if (s.update) evalExpr(s.update, env);
      }
      return null;
    }
    case 'ForOfStatement': case 'ForInStatement': {
      const it = evalExpr(s.right, env);
      const list = Array.isArray(it) ? (s.type === 'ForOfStatement' ? it : it.map((_, i) => String(i))) : (it && typeof it === 'object' && !isU(it) && s.type === 'ForInStatement' ? Object.keys(it) : []);
      for (const v of list.slice(0, 500)) {
        const p = s.left.type === 'VariableDeclaration' ? s.left.declarations[0].id : s.left;
        if (p.type === 'Identifier' && s.left.type !== 'VariableDeclaration') assign(p.name, v, env); else bindPattern(p, v, env);
        const r = exec(s.body, env);
        if (r === BREAK) break;
        if (r && r !== CONTINUE) return r;
      }
      return null;
    }
    case 'BreakStatement': return BREAK;
    case 'ContinueStatement': return CONTINUE;
    case 'EmptyStatement': return null;
    case 'ThrowStatement': throw new InterpError('throw');
    case 'LabeledStatement': return exec(s.body, env);
    case 'SwitchStatement': {
      const d = evalExpr(s.discriminant, env);
      if (isU(d)) return null;
      let on = false;
      for (const c of s.cases) {
        if (!on && (c.test === null || evalExpr(c.test, env) === d)) on = true;
        if (on) { const r = execBlock(c.consequent, env); if (r === BREAK) return null; if (r) return r; }
      }
      return null;
    }
    default: return null; // while/do loops, classes: skipped (runtime only)
  }
}
const ARITH = {
  '+': (a, b) => a + b, '-': (a, b) => a - b, '*': (a, b) => a * b, '/': (a, b) => a / b, '%': (a, b) => a % b,
  '===': (a, b) => a === b, '!==': (a, b) => a !== b, '==': (a, b) => a == b, '!=': (a, b) => a != b, // eslint-disable-line eqeqeq
  '<': (a, b) => a < b, '>': (a, b) => a > b, '<=': (a, b) => a <= b, '>=': (a, b) => a >= b,
  '&': (a, b) => a & b, '|': (a, b) => a | b, '^': (a, b) => a ^ b, '<<': (a, b) => a << b, '>>': (a, b) => a >> b, '>>>': (a, b) => a >>> b,
};
const prim = (v) => v === null || v === undefined || ['string', 'number', 'boolean'].includes(typeof v);
function evalExpr(e, env) {
  tick();
  switch (e.type) {
    case 'Literal': return e.regex ? UNKNOWN : e.value;
    case 'Identifier': return e.name === 'undefined' ? undefined : lookup(e.name, env, e);
    case 'TemplateLiteral': {
      let s = e.quasis[0].value.cooked;
      for (let i = 0; i < e.expressions.length; i++) { const v = evalExpr(e.expressions[i], env); if (!prim(v)) return UNKNOWN; s += String(v ?? '') + e.quasis[i + 1].value.cooked; }
      return s;
    }
    case 'ArrayExpression': {
      const out = [];
      for (const x of e.elements) { if (!x) out.push(undefined); else if (x.type === 'SpreadElement') { const v = evalExpr(x.argument, env); if (Array.isArray(v)) out.push(...v); else return UNKNOWN; } else out.push(evalExpr(x, env)); }
      return out;
    }
    case 'ObjectExpression': {
      const o = {};
      for (const p of e.properties) {
        if (p.type === 'SpreadElement') { const v = evalExpr(p.argument, env); if (v && typeof v === 'object' && !isU(v)) Object.assign(o, v); continue; }
        const k = p.computed ? evalExpr(p.key, env) : keyOf(p);
        if (isU(k)) continue;
        if (p.kind === 'get') { const c = new Closure(p.value, env); Object.defineProperty(o, k, { get: () => call(c, o, []), enumerable: true, configurable: true }); }
        else if (p.kind === 'set') continue;
        else o[k] = evalExpr(p.value, env);
      }
      return o;
    }
    case 'FunctionExpression': case 'ArrowFunctionExpression': return new Closure(e, env);
    case 'SequenceExpression': { let v; for (const x of e.expressions) v = evalExpr(x, env); return v; }
    case 'ParenthesizedExpression': return evalExpr(e.expression, env);
    case 'UnaryExpression': {
      if (e.operator === 'typeof') { const v = evalExpr(e.argument, env); return isU(v) ? UNKNOWN : v instanceof Closure || v instanceof Native ? 'function' : v === null ? 'object' : typeof v; }
      if (e.operator === 'void') { evalExpr(e.argument, env); return undefined; }
      if (e.operator === 'delete') return true;
      const v = evalExpr(e.argument, env);
      if (e.operator === '!') return isU(v) ? true : !v; // unknown state reads as falsy: !state is the initial branch
      if (!prim(v)) return UNKNOWN;
      return e.operator === '-' ? -v : e.operator === '+' ? +v : e.operator === '~' ? ~v : UNKNOWN;
    }
    case 'LogicalExpression': {
      const a = evalExpr(e.left, env);
      if (e.operator === '&&') return truthy(a) ? evalExpr(e.right, env) : a;
      if (e.operator === '||') return truthy(a) ? a : evalExpr(e.right, env);
      return a === null || a === undefined || isU(a) ? evalExpr(e.right, env) : a; // ??
    }
    case 'BinaryExpression': {
      const a = evalExpr(e.left, env), b = evalExpr(e.right, env);
      if (e.operator === 'in' || e.operator === 'instanceof') return UNKNOWN;
      if (!prim(a) || !prim(b)) return e.operator === '===' || e.operator === '==' ? (isU(a) || isU(b) ? UNKNOWN : a === b) : e.operator === '!==' || e.operator === '!=' ? (isU(a) || isU(b) ? UNKNOWN : a !== b) : UNKNOWN;
      return ARITH[e.operator] ? ARITH[e.operator](a, b) : UNKNOWN;
    }
    case 'ConditionalExpression': { const t = evalExpr(e.test, env); return truthy(t) ? evalExpr(e.consequent, env) : evalExpr(e.alternate, env); }
    case 'AssignmentExpression': {
      let v = evalExpr(e.right, env);
      if (e.operator !== '=') {
        const cur = e.left.type === 'Identifier' ? lookup(e.left.name, env) : evalExpr(e.left, env);
        if (e.operator === '||=') { if (truthy(cur)) return cur; } else if (e.operator === '&&=') { if (!truthy(cur)) return cur; } else if (e.operator === '??=') { if (cur != null && !isU(cur)) return cur; } else {
          const op = e.operator.slice(0, -1); v = prim(cur) && prim(v) && ARITH[op] ? ARITH[op](cur, v) : UNKNOWN;
        }
      }
      if (e.left.type === 'Identifier') assign(e.left.name, v, env);
      else if (e.left.type === 'MemberExpression') setMember(evalExpr(e.left.object, env), e.left.computed ? evalExpr(e.left.property, env) : e.left.property.name, v);
      return v;
    }
    case 'UpdateExpression': {
      if (e.argument.type !== 'Identifier') return UNKNOWN;
      const cur = lookup(e.argument.name, env); const n = typeof cur === 'number' ? cur + (e.operator === '++' ? 1 : -1) : UNKNOWN;
      assign(e.argument.name, n, env); return e.prefix ? n : cur;
    }
    case 'MemberExpression': case 'ChainExpression': {
      const m = e.type === 'ChainExpression' ? e.expression : e;
      if (m.type === 'CallExpression') return evalExpr(m, env);
      const o = evalExpr(m.object, env);
      if (m.optional && (o === null || o === undefined)) return undefined;
      return getMember(o, m.computed ? evalExpr(m.property, env) : m.property.name);
    }
    case 'CallExpression': {
      let self = UNKNOWN, f;
      const c = e.callee;
      if (c.type === 'MemberExpression') {
        self = evalExpr(c.object, env);
        const k = c.computed ? evalExpr(c.property, env) : c.property.name;
        if ((k === 'call' || k === 'apply') && (self instanceof Closure || self instanceof Native)) {
          const a = e.arguments.map((x) => evalExpr(x, env));
          return call(self, a[0], k === 'call' ? a.slice(1) : (Array.isArray(a[1]) ? a[1] : []));
        }
        f = getMember(self, k);
      } else f = evalExpr(c, env);
      if (e.optional && (f === null || f === undefined)) return undefined;
      const args = [];
      for (const x of e.arguments) { if (x.type === 'SpreadElement') { const v = evalExpr(x.argument, env); if (Array.isArray(v)) args.push(...v); } else args.push(evalExpr(x, env)); }
      return call(f, self, args);
    }
    case 'NewExpression': case 'AwaitExpression': case 'YieldExpression': case 'ClassExpression': case 'TaggedTemplateExpression': case 'ThisExpression': case 'MetaProperty': case 'ImportExpression':
      return UNKNOWN;
    default: return UNKNOWN;
  }
}
// ------------------------------------------------------------------ driver
function render(compClosure, props, label) {
  const host = new TNode('el', 'div');
  const anchor = new TNode('text'); anchor.parent = host; host.children.push(anchor);
  steps = 0;
  try { call(compClosure, UNKNOWN, [anchor, props || {}]); } catch (err) {
    if (!(err instanceof InterpError)) throw err;
    report.push('render ' + label + ': ' + err.message);
  }
  host.children = host.children.filter((c) => c !== anchor);
  return serialize(host).replace(/^<div>|<\/div>$/g, '');
}
function targetId(obj) {
  const t = obj.properties.find((p) => p.type === 'Property' && keyOf(p) === 'target');
  if (!t) return null;
  const s = src(t.value);
  const m = /getElementById["']?\]?\(\s*["']([\w-]+)["']\s*\)/.exec(s);
  if (m) return m[1];
  if (t.value.type === 'Identifier') return t.value.name; // a global element id (window.<id>)
  return null;
}
const mounts = {};
const mountOrder = [];
for (const c of mountCalls) {
  const id = targetId(c.arguments[1]);
  if (!id || mounts[id]) continue;
  const env = staticEnv(enclosingFn(c));
  const comp = lookup(c.arguments[0].name, env);
  if (!(comp instanceof Closure)) { report.push('mount #' + id + ': component ' + c.arguments[0].name + ' not resolved'); continue; }
  const propsNode = c.arguments[1].properties.find((p) => p.type === 'Property' && keyOf(p) === 'props');
  let props = {};
  if (propsNode) { try { props = evalExpr(propsNode.value, env); } catch (err) { if (!(err instanceof InterpError)) throw err; } }
  if (!props || typeof props !== 'object' || isU(props)) props = {};
  const html = render(comp, props, '#' + id);
  mounts[id] = html;
  mountOrder.push(id);
  report.push('mount #' + id + ' <- ' + comp.node.id.name + ' (' + html.length + ' bytes, ' + steps + ' steps)');
}
fs.mkdirSync(OUT, { recursive: true });
fs.writeFileSync(path.join(OUT, 'mounts.json'), JSON.stringify(mounts, null, 1) + '\n');
// every component stylesheet; the ones the rendered mounts use first
const seen = new Set();
const ordered = usedCss.map((h) => cssBlocks.find((b) => b.hash === h)).filter(Boolean).concat(cssBlocks);
let css = '/* Krunker v10 Svelte component stylesheets, extracted from the client bundle by tools/ui-import/v10_bundle.js */\n';
for (const b of ordered) { if (seen.has(b.hash)) continue; seen.add(b.hash); css += '/* ' + b.hash + ' */\n' + b.code.trim() + '\n'; }
fs.writeFileSync(path.join(OUT, 'svelte.css'), css);
// translations.en: parseText = lines "key value..." split at the first space
const i18n = {};
if (i18nText) for (const line of i18nText.split('\n')) { if (!line.trim()) continue; const k = line.split(' ')[0]; i18n[k] = line.split(' ').slice(1).join(' '); }
fs.writeFileSync(path.join(OUT, 'i18n-en.json'), JSON.stringify(i18n, null, 1) + '\n');
report.unshift('helpers: ' + Object.entries(roles).map(([k, v]) => k + '=' + v.length).join(' '),
  'css blocks: ' + cssBlocks.length + ' (' + usedCss.length + ' used by the mounts)', 'i18n en keys: ' + Object.keys(i18n).length);
report.push('runtime helpers skipped (unknown): ' + [...unknownCalls.entries()].sort((a, b) => b[1] - a[1]).slice(0, 30).map(([k, v]) => k + 'x' + v).join(' '));
fs.writeFileSync(path.join(OUT, 'report.txt'), report.join('\n') + '\n');
process.stdout.write(report.slice(0, 3 + mountOrder.length).join('\n') + '\n');
