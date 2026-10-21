// Browser globals for running the official Krunker v10.0.3 bundle (game.js) in Node.
//
// The bundle expects a page: window, document (the menu is built into it), a WebGL context, storage,
// fetch and WebSocket. This module provides those as small stubs. Every network call is routed to
// loopback: the official hostnames (matchmaker, gapi, coordinator, api) go to the local brain, the bridge's
// name goes to the local v10_bridge, and any other host is refused, so nothing leaves the machine.
// Every frame the client sends or receives is passed to cfg.onFrame.
//
// Nothing here is Krunker code. Each stub says, where it is defined, why the bundle needs it.
import fs from "node:fs";
import path from "node:path";

// Hostnames the client uses, and where they land on the brain (matchmaker routes sit under /mm).
export const OFFICIAL_HOSTS = {
  "matchmaker.krunker.io": "/mm",
  "matchmaker.beta.krunker.io": "/mm",
  "gapi.svc.krunker.io": "",
  "gapi.beta.krunker.io": "",
  "coordinator.krunk.io": "",
  "api.beta.krunker.io": "",
  "api.krunker.io": "",
};

export function isLoopbackHost(host) {
  return host === "127.0.0.1" || host === "krunker-local.test" || host.endsWith(".krunker-local.test");
}

// Accepts any call and any property; calls return another stub. Used for APIs the menu touches but whose
// result does not matter here (audio, workers, broadcast channels, caches).
function anyStub(name) {
  const fn = function () { return anyStub(name + "()"); };
  return new Proxy(fn, {
    get(t, p) {
      if (p === Symbol.toPrimitive) return () => 0;
      if (p === "then") return undefined;
      if (p === "length") return 0;
      if (typeof p === "symbol") return undefined;
      if (p in t) return t[p];
      return anyStub(name + "." + String(p));
    },
    apply() { return anyStub(name + "()"); },
    construct() { return anyStub("new " + name); },
    set() { return true; },
  });
}

// WebGL context. ALL_CAPS names are GL constants (each gets a stable number); the methods that the
// renderer checks come back as success values; everything else is a stub. Nothing is drawn.
function glContext(canvas) {
  const consts = new Map();
  let next = 0x1000;
  const answers = {
    getParameter: () => 0,
    getShaderParameter: () => true,
    getProgramParameter: () => true,
    getExtension: () => null,
    getSupportedExtensions: () => [],
    getContextAttributes: () => ({ antialias: false, alpha: true, depth: true, stencil: false }),
    isContextLost: () => false,
    getError: () => 0,
  };
  return new Proxy({ canvas, drawingBufferWidth: canvas.width, drawingBufferHeight: canvas.height }, {
    get(t, p) {
      if (p in t) return t[p];
      if (typeof p !== "string") return undefined;
      if (/^[A-Z][A-Z0-9_]*$/.test(p)) {
        if (!consts.has(p)) consts.set(p, next++);
        return consts.get(p);
      }
      if (p in answers) return answers[p];
      return anyStub("gl." + p);
    },
  });
}

function addListener(map, type, fn, opts) {
  if (!fn) return;
  const once = typeof opts === "object" && opts !== null && !!opts.once;
  const list = map.get(type) || [];
  if (!list.some((l) => l.fn === fn)) list.push({ fn, once });
  map.set(type, list);
}
function removeListener(map, type, fn) {
  map.set(type, (map.get(type) || []).filter((l) => l.fn !== fn));
}

// Event init: copy the init keys that are plain properties (Event's own getters are read-only).
function eventWith(base, init) {
  for (const [k, v] of Object.entries(init)) {
    if (!(k in base) || Object.getOwnPropertyDescriptor(Object.getPrototypeOf(base), k)?.set) {
      Object.defineProperty(base, k, { value: v, writable: true, configurable: true, enumerable: true });
    }
  }
  return base;
}
export class KeyboardEvent extends Event {
  constructor(type, init = {}) { super(type, { bubbles: true, cancelable: true, ...init }); eventWith(this, init); }
}
export class MouseEvent extends Event {
  constructor(type, init = {}) {
    super(type, { bubbles: true, cancelable: true, ...init });
    eventWith(this, { clientX: 0, clientY: 0, button: 0, buttons: 0, ...init });
  }
}
export class PointerEvent extends MouseEvent {}
export class WheelEvent extends MouseEvent {}
export class UIEvent extends Event {}
export class ErrorEvent extends Event {}

// Dispatch to one target: the event goes to the node's own listeners, then up the parent chain (document,
// then window), as the page's delegated listeners expect. Stops at stopPropagation.
function dispatchOn(target, ev) {
  defineTarget(ev, "target", target);
  const path = [];
  for (let n = target; n; n = n.parentNode) path.push(n);
  const doc = target.ownerDocument || target;
  if (path[path.length - 1] === doc && doc.defaultView) path.push(doc.defaultView);
  for (const n of path) {
    defineTarget(ev, "currentTarget", n);
    const map = n._listeners || (n === globalThis ? globalThis.__knWindowListeners : null);
    const prop = n["on" + ev.type];
    if (typeof prop === "function") {
      try { prop.call(n, ev); } catch (e) { reportError(e); }
    }
    for (const l of [...((map && map.get(ev.type)) || [])]) {
      if (l.once && map) removeListener(map, ev.type, l.fn);
      try {
        if (typeof l.fn === "function") l.fn.call(n, ev);
        else if (l.fn && typeof l.fn.handleEvent === "function") l.fn.handleEvent(ev);
      } catch (e) { reportError(e); }
    }
    if (!ev.bubbles || ev.cancelBubble) break;
  }
  return !ev.defaultPrevented;
}
function defineTarget(ev, key, value) {
  Object.defineProperty(ev, key, { value, writable: true, configurable: true, enumerable: true });
}

function reportError(e) {
  if (globalThis.__knErrorSink) globalThis.__knErrorSink(e);
}

// Named access: an element's id is a global, as the browser's window exposes it (the bundle reads ids as bare names).
// A name that is already a real global keeps its value.
function exposeNamed(name, el) {
  if (!name || !/^[A-Za-z_$][\w$]*$/.test(name)) return;
  const cur = Object.getOwnPropertyDescriptor(globalThis, name);
  if (cur && !(cur.value instanceof Node_) && !(cur.configurable && cur.value === undefined)) return;
  Object.defineProperty(globalThis, name, { value: el, writable: true, configurable: true, enumerable: false });
}

// The inline style: declarations as properties, plus the CSSStyleDeclaration methods the page calls.
function makeStyle() {
  const css = {};
  const api = {
    setProperty(k, v) { css[k] = String(v); },
    getPropertyValue(k) { return css[k] ?? ""; },
    removeProperty(k) { const old = css[k] ?? ""; delete css[k]; return old; },
  };
  return new Proxy(css, {
    get(t, k) {
      if (typeof k === "symbol") return undefined;
      if (k in api) return api[k];
      return t[k] ?? "";
    },
    set(t, k, v) { t[k] = String(v); return true; },
  });
}

// A DOM node: the subset the bundle uses (tree, attributes, classes, style, events, selectors).
class Node_ {
  constructor(doc) {
    this.ownerDocument = doc;
    this.parentNode = null;
    this.childNodes = [];
    this.style = makeStyle();
    this.dataset = {};
    this.attributes = new Map();
    this._listeners = new Map();
  }
  get parentElement() { return this.parentNode && this.parentNode.nodeType === 1 ? this.parentNode : null; }
  get children() { return this.childNodes.filter((c) => c.nodeType === 1); }
  get firstChild() { return this.childNodes[0] || null; }
  get nextSibling() { const p = this.parentNode; return p ? p.childNodes[p.childNodes.indexOf(this) + 1] || null : null; }
  get previousSibling() { const p = this.parentNode; return p ? p.childNodes[p.childNodes.indexOf(this) - 1] || null : null; }
  get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; }
  get previousElementSibling() { let n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; }
  get childElementCount() { return this.children.length; }
  get isConnected() { for (let n = this; n; n = n.parentNode) if (n.nodeType === 9) return true; return false; }
  get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
  cloneNode(deep) {
    const doc = this.ownerDocument;
    const c = this.nodeType === 3 ? doc.createTextNode(this.data) : new this.constructor(doc, this.localName);
    if (this.nodeType === 1) for (const [k, v] of this.attributes) c.attributes.set(k, v);
    if (deep) for (const child of this.childNodes) c.appendChild(child.cloneNode(true));
    return c;
  }
  hasChildNodes() { return this.childNodes.length > 0; }
  appendChild(c) { return this.insertBefore(c, null); }
  insertBefore(c, ref) {
    if (c.parentNode) c.parentNode.removeChild(c);
    const i = ref ? this.childNodes.indexOf(ref) : -1;
    if (i < 0) this.childNodes.push(c); else this.childNodes.splice(i, 0, c);
    c.parentNode = this;
    return c;
  }
  removeChild(c) {
    const i = this.childNodes.indexOf(c);
    if (i >= 0) { this.childNodes.splice(i, 1); c.parentNode = null; }
    return c;
  }
  replaceChild(n, old) { this.insertBefore(n, old); this.removeChild(old); return old; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  contains(n) { for (let c = n; c; c = c.parentNode) if (c === this) return true; return false; }
  get textContent() { return this.childNodes.map((c) => (c.nodeType === 3 ? c.data : c.textContent)).join(""); }
  set textContent(v) {
    for (const c of this.childNodes) c.parentNode = null;
    this.childNodes = [];
    if (v !== "" && v != null) this.appendChild(this.ownerDocument.createTextNode(String(v)));
  }
  get innerText() { return this.textContent; }
  set innerText(v) { this.textContent = v; }
  get innerHTML() { return this.textContent; }
  set innerHTML(v) { this.textContent = ""; parseInto(this.content || this, String(v)); }
  get id() { return this.attributes.get("id") || ""; }
  set id(v) { this.attributes.set("id", String(v)); }
  get className() { return this.attributes.get("class") || ""; }
  set className(v) { this.attributes.set("class", String(v)); }
  get classList() {
    const el = this;
    const list = () => el.className.split(/\s+/).filter(Boolean);
    const set = (a) => { el.className = [...new Set(a)].join(" "); };
    return {
      add: (...c) => set([...list(), ...c]),
      remove: (...c) => set(list().filter((x) => !c.includes(x))),
      toggle(c, force) {
        const has = list().includes(c);
        const on = force === undefined ? !has : !!force;
        if (on && !has) set([...list(), c]);
        if (!on && has) set(list().filter((x) => x !== c));
        return on;
      },
      contains: (c) => list().includes(c),
      item: (i) => list()[i] ?? null,
      get length() { return list().length; },
      [Symbol.iterator]: function* () { yield* list(); },
    };
  }
  setAttribute(k, v) {
    this.attributes.set(String(k), String(v));
    if (String(k) === "id") exposeNamed(String(v), this);
  }
  getAttribute(k) { return this.attributes.has(k) ? this.attributes.get(k) : null; }
  hasAttribute(k) { return this.attributes.has(k); }
  removeAttribute(k) { this.attributes.delete(k); }
  getBoundingClientRect() { return { x: 0, y: 0, top: 0, left: 0, width: 0, height: 0, right: 0, bottom: 0 }; }
  get offsetWidth() { return 0; }
  get offsetHeight() { return 0; }
  get clientWidth() { return 0; }
  get clientHeight() { return 0; }
  get offsetParent() { return null; }
  focus() {}
  blur() {}
  click() { this.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true })); }
  addEventListener(type, fn, opts) { addListener(this._listeners, type, fn, opts); }
  removeEventListener(type, fn) { removeListener(this._listeners, type, fn); }
  dispatchEvent(ev) { return dispatchOn(this, ev); }
  matches(sel) { return sel.split(",").some((s) => matchesCompound(this, s.trim().split(/\s+/).pop())); }
  closest(sel) { for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (n.matches(sel)) return n; return null; }
  querySelectorAll(sel) {
    const out = [];
    const groups = sel.split(",").map((s) => s.trim().split(/\s+/));
    const walk = (n) => {
      for (const c of n.childNodes) {
        if (c.nodeType !== 1) continue;
        if (groups.some((g) => matchChain(c, g, this))) out.push(c);
        walk(c);
      }
    };
    walk(this);
    return out;
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  getElementsByTagName(t) { return this.querySelectorAll(t); }
  getElementsByClassName(c) { return this.querySelectorAll("." + c); }
}

class Element_ extends Node_ {
  constructor(doc, tag) {
    super(doc);
    this.nodeType = 1;
    this.tagName = String(tag).toUpperCase();
    this.localName = String(tag).toLowerCase();
    this.namespaceURI = "http://www.w3.org/1999/xhtml";
    if (this.localName === "template") this.content = new Element_(doc, "fragment");
  }
  get nodeName() { return this.tagName; }
}

class Text_ extends Node_ {
  constructor(doc, data) { super(doc); this.nodeType = 3; this.data = String(data); }
  get nodeName() { return "#text"; }
  get textContent() { return this.data; }
  set textContent(v) { this.data = String(v); }
  get nodeValue() { return this.data; }
  set nodeValue(v) { this.data = String(v); }
}

// Simple selectors: tag, #id, .class, [attr] / [attr=value], * and a descendant chain of them.
function matchesCompound(el, compound) {
  if (!el || el.nodeType !== 1) return false;
  const re = /([.#]?[A-Za-z0-9_-]+)|\[([^\]]+)\]/g;
  let m;
  let any = false;
  while ((m = re.exec(compound))) {
    any = true;
    if (m[2]) {
      const [k, v] = m[2].split("=");
      const have = el.getAttribute(k.trim());
      if (have === null) return false;
      if (v !== undefined && have !== v.trim().replace(/^["']|["']$/g, "")) return false;
    } else if (m[1].startsWith("#")) { if (el.id !== m[1].slice(1)) return false; }
    else if (m[1].startsWith(".")) { if (!el.classList.contains(m[1].slice(1))) return false; }
    else if (m[1] !== "*" && el.localName !== m[1].toLowerCase()) return false;
  }
  return any || compound === "*";
}

// "a b c": the last part matches el, the others match ancestors below root in order.
function matchChain(el, parts, root) {
  if (!matchesCompound(el, parts[parts.length - 1])) return false;
  let i = parts.length - 2;
  for (let n = el.parentNode; n && i >= 0 && n !== root.parentNode; n = n.parentNode) {
    if (n.nodeType === 1 && matchesCompound(n, parts[i])) i--;
  }
  return i < 0;
}

// Comment node: nodeType 8 (Svelte puts anchors in comments).
class Comment_ extends Node_ {
  constructor(doc, data) { super(doc); this.nodeType = 8; this.data = String(data ?? ""); }
  get nodeName() { return "#comment"; }
  get textContent() { return this.data; }
  set textContent(v) { this.data = String(v); }
  get nodeValue() { return this.data; }
  set nodeValue(v) { this.data = String(v); }
}

// Range: positions are kept, the fragment helpers parse HTML (the bundle builds its icons this way).
class Range_ {
  constructor(doc) { this.doc = doc; this.startContainer = doc; this.endContainer = doc; this.collapsed = true; this.commonAncestorContainer = doc; }
  setStart(n) { this.startContainer = n; }
  setEnd(n) { this.endContainer = n; }
  setStartBefore(n) { this.startContainer = n.parentNode || n; }
  setStartAfter(n) { this.startContainer = n.parentNode || n; }
  setEndBefore(n) { this.endContainer = n.parentNode || n; }
  setEndAfter(n) { this.endContainer = n.parentNode || n; }
  selectNode(n) { this.startContainer = this.endContainer = n.parentNode || n; }
  selectNodeContents(n) { this.startContainer = this.endContainer = n; }
  collapse() {}
  cloneRange() { const r = new Range_(this.doc); r.startContainer = this.startContainer; r.endContainer = this.endContainer; return r; }
  deleteContents() {}
  cloneContents() { return this.doc.createDocumentFragment(); }
  insertNode(n) { if (this.startContainer && this.startContainer.appendChild) this.startContainer.appendChild(n); }
  createContextualFragment(html) {
    const f = this.doc.createDocumentFragment();
    parseInto(f, String(html));
    return f;
  }
  getBoundingClientRect() { return { x: 0, y: 0, top: 0, left: 0, width: 0, height: 0, right: 0, bottom: 0 }; }
  getClientRects() { return []; }
}

class Canvas_ extends Element_ {
  constructor(doc) { super(doc, "canvas"); this.width = 1280; this.height = 720; this._ctx = null; }
  getContext(kind) {
    if (!this._ctx) this._ctx = /webgl|experimental/.test(kind) ? glContext(this) : anyStub("ctx2d");
    return this._ctx;
  }
  toDataURL() { return "data:image/png;base64,"; }
  toBlob(cb) { cb(null); }
  captureStream() { return anyStub("stream"); }
}

// Image: setting src fetches it (page files, loopback) and fires load or error, as a browser does for an img.
// The bundle waits on these loads for its textures, so they must fire.
class ImageEl_ extends Canvas_ {
  constructor(width, height) {
    super(globalThis.document);
    this.complete = false;
    this.naturalWidth = 0;
    this.naturalHeight = 0;
    this._src = "";
    if (width !== undefined) this.width = width;
    if (height !== undefined) this.height = height;
  }
  get src() { return this._src; }
  set src(v) {
    this._src = String(v);
    setTimeout(() => this._load(), 0);
  }
  async _load() {
    try {
      const res = await globalThis.fetch(this._src);
      const buf = Buffer.from(await res.arrayBuffer());
      if (!res.ok) throw new Error(`image ${res.status}`);
      if (buf.length >= 24 && buf.toString("ascii", 1, 4) === "PNG") {
        this.naturalWidth = buf.readUInt32BE(16);
        this.naturalHeight = buf.readUInt32BE(20);
      }
      this.complete = true;
      this.dispatchEvent(new Event("load"));
    } catch (e) {
      this.complete = true;
      this.dispatchEvent(new Event("error"));
    }
  }
}

class Document_ extends Node_ {
  constructor(win, loc) {
    super(null);
    this.ownerDocument = this;
    this.nodeType = 9;
    this.nodeName = "#document";
    this.defaultView = win;
    this.location = loc;
    this.URL = loc.href;
    this.domain = loc.hostname;
    this.referrer = "";
    this.readyState = "complete";
    this.visibilityState = "visible";
    this.hidden = false;
    this.title = "Krunker";
    this.cookie = "";
    this.pointerLockElement = null;
    this.activeElement = null;
    this.documentElement = this.createElement("html");
    this.head = this.createElement("head");
    this.body = this.createElement("body");
    this.appendChild(this.documentElement);
    this.documentElement.appendChild(this.head);
    this.documentElement.appendChild(this.body);
    this.fonts = { ready: Promise.resolve(), add() {}, load: () => Promise.resolve([]), check: () => true, addEventListener() {} };
  }
  createElement(tag) { return String(tag).toLowerCase() === "canvas" ? new Canvas_(this) : new Element_(this, tag); }
  createElementNS(_ns, tag) { return this.createElement(tag); }
  createTextNode(data) { return new Text_(this, data); }
  createComment(data) { return new Comment_(this, data); }
  createRange() { return new Range_(this); }
  getSelection() { return anyStub("Selection"); }
  importNode(n, deep) { return n.cloneNode(!!deep); }
  createDocumentFragment() { const f = new Element_(this, "fragment"); f.nodeType = 11; return f; }
  createEvent(kind) { return kind === "KeyboardEvent" ? new KeyboardEvent("init") : new MouseEvent("init"); }
  getElementById(id) { return this.querySelector("#" + id); }
  exitPointerLock() {}
  requestPointerLock() {}
  hasFocus() { return true; }
  elementFromPoint() { return this.body; }
  execCommand() { return false; }
}

// Event target for the page objects that are not DOM nodes (FileReader, Image, timers of the page).
class Target_ {
  constructor() { this._listeners = new Map(); }
  addEventListener(type, fn, opts) { addListener(this._listeners, type, fn, opts); }
  removeEventListener(type, fn) { removeListener(this._listeners, type, fn); }
  dispatchEvent(ev) { return dispatchOn(this, ev); }
}

// FileReader: reads a Blob (what the bundle passes it) and fires load / loadend, or error.
class FileReader_ extends Target_ {
  constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
  readAsArrayBuffer(b) { this._read(b, "arrayBuffer"); }
  readAsText(b) { this._read(b, "text"); }
  readAsDataURL(b) { this._read(b, "dataURL"); }
  abort() {}
  async _read(b, how) {
    this.readyState = 1;
    try {
      const buf = Buffer.from(await b.arrayBuffer());
      this.result = how === "arrayBuffer" ? buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length)
        : how === "text" ? buf.toString("utf8")
          : `data:${b.type || "application/octet-stream"};base64,${buf.toString("base64")}`;
      this.readyState = 2;
      this.dispatchEvent(new Event("load"));
    } catch (e) {
      this.error = e;
      this.readyState = 2;
      this.dispatchEvent(new Event("error"));
    }
    this.dispatchEvent(new Event("loadend"));
  }
}

class Storage_ {
  constructor() { this._m = new Map(); }
  getItem(k) { return this._m.has(String(k)) ? this._m.get(String(k)) : null; }
  setItem(k, v) { this._m.set(String(k), String(v)); }
  removeItem(k) { this._m.delete(String(k)); }
  clear() { this._m.clear(); }
  key(i) { return [...this._m.keys()][i] ?? null; }
  get length() { return this._m.size; }
}

// Route one URL. Returns { kind: brain | bridge | page | loopback, url, host } or throws for anything else.
function route(rawUrl, cfg) {
  const u = new URL(String(rawUrl), cfg.pageUrl);
  if (Object.hasOwn(OFFICIAL_HOSTS, u.hostname)) {
    const prefix = OFFICIAL_HOSTS[u.hostname];
    const p = u.pathname.startsWith(prefix + "/") ? u.pathname : prefix + u.pathname;
    return { kind: "brain", url: new URL(p + u.search, cfg.brainUrl).toString(), host: u.hostname };
  }
  if (u.hostname === cfg.bridgeHost) {
    return { kind: "bridge", url: `ws://127.0.0.1:${cfg.bridgePort}${u.pathname}${u.search}`, host: u.hostname };
  }
  if (u.hostname === new URL(cfg.pageUrl).hostname) return { kind: "page", url: u.toString(), host: u.hostname, path: u.pathname };
  if (!isLoopbackHost(u.hostname)) throw new TypeError(`krunker-local: refused ${u.hostname} (not loopback)`);
  return { kind: "loopback", url: u.toString(), host: u.hostname };
}

// The page's own files (game.js, static/, img/...): the official tree (read-only) first, then the KrunkNative
// assets/ and ui/ trees (the first root with the file wins, as the harness's fillPaths does).
function pageFile(rel, cfg) {
  const roots = [cfg.krunkerSrc, ...cfg.fillRoots.map((r) => path.join(cfg.knRepo, r))];
  for (const root of roots) {
    const file = path.join(root, rel);
    if (!file.startsWith(root + path.sep)) return null;
    if (fs.existsSync(file) && fs.statSync(file).isFile()) return file;
  }
  return null;
}

const MIME = {
  ".html": "text/html", ".js": "application/javascript", ".css": "text/css", ".json": "application/json",
  ".png": "image/png", ".jpg": "image/jpeg", ".svg": "image/svg+xml", ".wasm": "application/wasm",
  ".woff2": "font/woff2", ".woff": "font/woff", ".ttf": "font/ttf", ".mp3": "audio/mpeg", ".ogg": "audio/ogg",
};

// A constructor whose instances and prototype members are stubs: the bundle patches prototypes
// (AudioParam.prototype.x = new Proxy(AudioParam.prototype.x, ...)), so every member must exist.
function stubClass(name) {
  function Stub() { return anyStub(name); }
  Stub.prototype = new Proxy({}, {
    get(t, p) {
      if (p in t) return t[p];
      if (typeof p === "symbol" || p === "constructor" || p === "then") return undefined;
      return anyStub(name + ".prototype." + String(p));
    },
    set(t, p, v) { t[p] = v; return true; },
  });
  return Stub;
}

// Browser classes the bundle references by name (instanceof, prototype patches, constructors).
const BROWSER_CLASSES = ["CanvasRenderingContext2D", "WebGLRenderingContext", "WebGL2RenderingContext", "WebGLTexture",
  "WebGLBuffer", "WebGLProgram", "WebGLShader", "WebGLFramebuffer", "WebGLRenderbuffer", "WebGLUniformLocation",
  "WebGLVertexArrayObject", "WebGLSampler", "WebGLSync", "WebGLQuery", "WebGLTransformFeedback", "OffscreenCanvas",
  "ImageData", "Path2D", "DOMParser", "XMLHttpRequest", "SVGElement", "HTMLScriptElement", "HTMLMediaElement",
  "HTMLBodyElement", "HTMLDivElement", "HTMLButtonElement", "HTMLAnchorElement", "HTMLSpanElement", "HTMLFormElement",
  "HTMLSelectElement", "HTMLTextAreaElement", "HTMLLinkElement", "HTMLStyleElement", "HTMLMetaElement",
  "HTMLHeadElement", "HTMLHtmlElement", "Gamepad", "DataTransfer", "DragEvent", "TouchEvent", "Touch", "FocusEvent",
  "InputEvent", "CompositionEvent", "DOMPoint", "DOMMatrix", "DOMRect", "HTMLCollection", "NodeList", "MediaSource",
  "SourceBuffer", "RTCPeerConnection", "IDBDatabase", "IDBObjectStore", "ServiceWorkerRegistration",
  "AudioParam", "AudioNode", "AudioBuffer", "AudioBufferSourceNode", "AudioDestinationNode", "AudioListener",
  "AudioWorkletNode", "AnalyserNode", "BiquadFilterNode", "ConvolverNode", "DelayNode", "GainNode", "OscillatorNode",
  "PannerNode", "StereoPannerNode", "ScriptProcessorNode", "MediaStreamAudioSourceNode", "MediaElementAudioSourceNode"];

// Installs the page globals. cfg: { pageUrl, brainUrl, bridgeHost, bridgePort, krunkerSrc, knRepo, fillRoots,
// onFrame(dir, data, host), onSocket(kind, host, code), onRequest(info), onError(e), rewriteSeek(buf) }.
export function installPage(cfg) {
  const win = globalThis;
  const loc = new URL(cfg.pageUrl);
  globalThis.__knErrorSink = cfg.onError || (() => {});
  globalThis.__knWindowListeners = new Map();
  const doc = new Document_(win, loc);
  const nav = {
    userAgent: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
    appName: "Netscape", appCodeName: "Mozilla", product: "Gecko", productSub: "20030107",
    appVersion: "5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36",
    platform: "Linux x86_64", language: "en-US", languages: ["en-US", "en"], onLine: true, cookieEnabled: true,
    maxTouchPoints: 0, hardwareConcurrency: 8, deviceMemory: 8, vendor: "Google Inc.", webdriver: false,
    getGamepads: () => [], clipboard: anyStub("clipboard"), sendBeacon: () => true,
    permissions: { query: () => Promise.resolve({ state: "prompt" }) },
    connection: { effectiveType: "4g", downlink: 10, rtt: 50, addEventListener() {} },
  };
  const set = (k, v) => Object.defineProperty(globalThis, k, { value: v, writable: true, configurable: true, enumerable: true });
  const stubs = {
    window: win, self: win, top: win, parent: win, frames: win,
    document: doc, navigator: nav, location: loc,
    history: { pushState() {}, replaceState() {}, back() {}, length: 1, state: null },
    screen: { width: 1280, height: 720, availWidth: 1280, availHeight: 720, colorDepth: 24, pixelDepth: 24 },
    innerWidth: 1280, innerHeight: 720, outerWidth: 1280, outerHeight: 720, devicePixelRatio: 1,
    scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0,
    localStorage: new Storage_(), sessionStorage: new Storage_(),
    Element: Element_, HTMLElement: Element_, HTMLCanvasElement: Canvas_, Node: Node_, Text: Text_, Comment: Comment_, Document: Document_,
    Range: Range_,
    Event, CustomEvent, KeyboardEvent, MouseEvent, PointerEvent, WheelEvent, UIEvent, ErrorEvent,
    getComputedStyle: () => new Proxy({}, { get: (t, k) => (typeof k === "string" ? "" : undefined) }),
    matchMedia: (q) => ({ matches: false, media: q, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} }),
    requestAnimationFrame: (f) => setTimeout(() => f(performance.now()), 16),
    cancelAnimationFrame: (id) => clearTimeout(id),
    requestIdleCallback: (f) => setTimeout(() => f({ didTimeout: false, timeRemaining: () => 10 }), 1),
    cancelIdleCallback: (id) => clearTimeout(id),
    AudioContext: class { constructor() { return anyStub("AudioContext"); } },
    webkitAudioContext: class { constructor() { return anyStub("AudioContext"); } },
    Audio: class { constructor() { return anyStub("Audio"); } },
    Image: ImageEl_,
    MutationObserver: class { observe() {} disconnect() {} takeRecords() { return []; } },
    ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
    IntersectionObserver: class { observe() {} unobserve() {} disconnect() {} },
    FileReader: FileReader_,
    Worker: class { constructor() { return anyStub("Worker"); } },
    BroadcastChannel: class { constructor() { return anyStub("BroadcastChannel"); } },
    indexedDB: anyStub("indexedDB"), caches: anyStub("caches"),
    Notification: anyStub("Notification"), speechSynthesis: anyStub("speechSynthesis"),
    gtag: () => {}, dataLayer: [],
    getSelection: () => doc.getSelection(),
    alert: () => {}, confirm: () => false, prompt: () => null, print: () => {},
    open: () => null, close: () => {}, focus: () => {}, blur: () => {}, scrollTo: () => {}, scrollBy: () => {},
  };
  for (const [k, v] of Object.entries(stubs)) if (v !== undefined) set(k, v);
  // URL resolves a relative string against the document, as the browser does (Node needs an explicit base).
  const NativeURL = globalThis.URL;
  set("URL", class URL extends NativeURL {
    constructor(u, base) {
      const relative = typeof u === "string" && !/^[a-z][a-z0-9+.-]*:/i.test(u);
      super(u, base ?? (relative ? loc.href : undefined));
    }
  });
  // Constructors the page or bundle names but never needs to work here (WebGL objects, DOM element classes).
  // Each is a class whose instances are stubs; a real Node global of the same name is kept.
  for (const name of BROWSER_CLASSES) {
    if (typeof globalThis[name] === "undefined") set(name, stubClass(name));
  }
  // The window's own listeners (the page's global keydown / mousemove handlers) and the window's dispatch.
  set("addEventListener", (type, fn, opts) => addListener(globalThis.__knWindowListeners, type, fn, opts));
  set("removeEventListener", (type, fn) => removeListener(globalThis.__knWindowListeners, type, fn));
  set("dispatchEvent", (ev) => dispatchOn(globalThis, ev));

  // fetch: page files from the trees, the official hosts and loopback through route(), anything else refused.
  // A seek answer is rewritten by cfg.rewriteSeek so the client connects to the bridge.
  const nativeFetch = globalThis.fetch;
  set("fetch", async (input, init) => {
    const raw = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
    const r = route(raw, cfg);
    const method = (init && init.method) || "GET";
    if (r.kind === "page") {
      const rel = r.path === "/" ? "index.html" : decodeURIComponent(r.path.slice(1));
      const file = pageFile(rel, cfg);
      cfg.onRequest && cfg.onRequest({ method, url: raw, status: file ? 200 : 404 });
      if (!file) return new Response("404 " + r.path, { status: 404, headers: { "content-type": "text/plain" } });
      return new Response(fs.readFileSync(file), { status: 200, headers: { "content-type": MIME[path.extname(file)] || "application/octet-stream" } });
    }
    if (r.kind !== "brain" && r.kind !== "loopback") throw new TypeError(`krunker-local: ${r.kind} is not reached by fetch`);
    const res = await nativeFetch(r.url, init);
    let body = Buffer.from(await res.arrayBuffer());
    if (r.kind === "brain" && /\/seek-game(\?|$)/.test(r.url) && res.status === 200 && cfg.rewriteSeek) body = cfg.rewriteSeek(body);
    cfg.onRequest && cfg.onRequest({ method, url: raw, status: res.status, host: r.host });
    const headers = new Headers(res.headers);
    headers.delete("content-length");
    return new Response(body, { status: res.status, headers });
  });

  // WebSocket: the bridge's name and loopback only. Every frame in or out goes to cfg.onFrame.
  const NativeWS = globalThis.WebSocket;
  class RoutedWebSocket extends NativeWS {
    constructor(url, protocols) {
      const r = route(url, cfg);
      if (r.kind !== "bridge" && r.kind !== "loopback") throw new TypeError(`krunker-local: refused WebSocket ${r.host}`);
      super(r.url, protocols);
      this._knHost = r.host;
      this.addEventListener("message", (e) => cfg.onFrame && cfg.onFrame("in", e.data, this._knHost));
      this.addEventListener("open", () => cfg.onSocket && cfg.onSocket("open", this._knHost));
      this.addEventListener("close", (e) => cfg.onSocket && cfg.onSocket("close", this._knHost, e.code));
      this.addEventListener("error", () => cfg.onSocket && cfg.onSocket("error", this._knHost));
    }
    send(data) {
      cfg.onFrame && cfg.onFrame("out", data, this._knHost);
      return super.send(data);
    }
  }
  set("WebSocket", RoutedWebSocket);
  return { document: doc, navigator: nav, location: loc };
}

// A small HTML parser for the page markup (index.html, and innerHTML). It builds the tree above; the html, head
// and body tags map onto the document's own elements. Raw-text elements (script, style) keep their text.
const VOID = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"]);
const RAW = new Set(["script", "style", "textarea", "title"]);
const ENT = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", nbsp: " " };
const decode = (s) => s.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (m, e) => (e[0] === "#" ? String.fromCodePoint(e[1] === "x" || e[1] === "X" ? parseInt(e.slice(2), 16) : Number(e.slice(1))) : ENT[e] ?? m));

// Parses html into container (an element or the document). Returns the script elements in document order.
export function parseInto(container, html) {
  const doc = container.ownerDocument || container;
  const scripts = [];
  const stack = [container];
  const top = () => stack[stack.length - 1];
  const named = (name) => (name === "html" ? doc.documentElement : name === "head" ? doc.head : name === "body" ? doc.body : null);
  const attrRe = /\s+([^\s"'>/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?/y;
  let i = 0;
  const into = () => (top().content && top() !== container ? top().content : top());
  const text = (s) => { if (s) into().appendChild(doc.createTextNode(decode(s))); };
  while (i < html.length) {
    const lt = html.indexOf("<", i);
    if (lt < 0) { text(html.slice(i)); break; }
    if (lt > i) text(html.slice(i, lt));
    i = lt;
    if (html.startsWith("<!--", i)) { const e = html.indexOf("-->", i + 4); i = e < 0 ? html.length : e + 3; continue; }
    if (html[i + 1] === "!" || html[i + 1] === "?") { const e = html.indexOf(">", i); i = e < 0 ? html.length : e + 1; continue; }
    if (html[i + 1] === "/") {
      const m = /<\/([a-zA-Z][^\s/>]*)\s*>/y; m.lastIndex = i;
      const r = m.exec(html);
      if (!r) { text("<"); i++; continue; }
      const name = r[1].toLowerCase();
      for (let k = stack.length - 1; k > 0; k--) {
        if (stack[k].localName === name || (named(name) === stack[k])) { stack.length = k; break; }
      }
      i = m.lastIndex;
      continue;
    }
    const tm = /<([a-zA-Z][^\s/>]*)/y; tm.lastIndex = i;
    const t = tm.exec(html);
    if (!t) { text("<"); i++; continue; }
    const name = t[1].toLowerCase();
    i = tm.lastIndex;
    const attrs = [];
    attrRe.lastIndex = i;
    let a;
    while ((a = attrRe.exec(html))) {
      attrs.push([a[1], decode(a[2] ?? a[3] ?? a[4] ?? "")]);
      attrRe.lastIndex = a.index + a[0].length;
      i = attrRe.lastIndex;
    }
    while (i < html.length && /\s/.test(html[i])) i++;
    const selfClose = html[i] === "/";
    const gt = html.indexOf(">", i);
    i = gt < 0 ? html.length : gt + 1;
    let el = named(name);
    if (!el) {
      el = doc.createElement(name);
      into().appendChild(el);
    }
    for (const [k, v] of attrs) el.setAttribute(k, v);
    if (name === "script") scripts.push(el);
    if (VOID.has(name) || selfClose) continue;
    if (RAW.has(name)) {
      const close = html.toLowerCase().indexOf("</" + name, i);
      const body = close < 0 ? html.slice(i) : html.slice(i, close);
      if (body) el.appendChild(doc.createTextNode(body));
      i = close < 0 ? html.length : html.indexOf(">", close) + 1;
      continue;
    }
    stack.push(el);
  }
  return scripts;
}

// Walks the document for the first element whose own text matches re (the deepest one), for the menu's buttons.
export function findByText(doc, re) {
  const hits = [];
  const walk = (n) => {
    for (const c of n.childNodes) {
      if (c.nodeType !== 1) continue;
      if (re.test((c.textContent || "").trim())) hits.push(c);
      walk(c);
    }
  };
  walk(doc);
  return hits.find((x) => !x.children.some((c) => re.test((c.textContent || "").trim()))) || hits[0] || null;
}
