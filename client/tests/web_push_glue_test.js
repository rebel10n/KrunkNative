// The web build's Web Push glue under node, with a stubbed browser (navigator.serviceWorker, PushManager,
// Notification, indexedDB): client/src/html/web_dom.c's EM_JS bodies kn_js_push_subscribe / kn_js_push_current /
// kn_js_push_listen and ui/sw.js's pushsubscriptionchange, read from the sources as they are compiled.
//   node client/tests/web_push_glue_test.js        (repository root; exit 0 = every check passed)
// Checked: a subscription made for another server's VAPID key is let go before subscribing with this one (a Box Office
// subscription is not reused for krunker.io's key, nor the other way); the same key's is reused; current reads without
// changing anything; the worker's WEBPUSH_REQUEST_VAPID_KEY is answered with the key, WEBPUSH_SUBSCRIPTION_LOST /
// _CHANGED reach C as "lost" / "changed", and the queued IndexedDB changes are handed on once and deleted (Krunker
// v10.0.7, deob-10.0.7.js:147015-147060, :148986-149003); the worker posts WEBPUSH_SUBSCRIPTION_CHANGED to an open page,
// queues the change when none is open, and posts WEBPUSH_SUBSCRIPTION_LOST when it cannot subscribe again.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = process.cwd();
let pass = 0, fail = 0;
function check(name, ok, detail) {
    if (ok) pass++;
    else fail++;
    console.log((ok ? 'PASS  ' : 'FAIL  ') + name + (!ok && detail !== undefined ? '  -- ' + JSON.stringify(detail) : ''));
}
const tick = () => new Promise((r) => setTimeout(r, 0));
async function settle() { for (let i = 0; i < 20; i++) await tick(); }

// ---- the EM_JS bodies of web_dom.c
const src = fs.readFileSync(path.join(root, 'client/src/html/web_dom.c'), 'utf8');
function emjs(name) {
    const re = new RegExp('EM_JS\\(\\w+, ' + name + ', \\(([^)]*)\\), \\{\\n([\\s\\S]*?)\\n\\}\\);');
    const m = src.match(re);
    if (!m) throw new Error('no EM_JS ' + name);
    const params = m[1].trim() === 'void' ? [] : m[1].split(',').map((p) => p.trim().split(/[\s*]+/).pop());
    return { params, body: m[2] };
}

// ---- a stubbed browser
function b64urlBytes(s) {
    let b = s.replace(/-/g, '+').replace(/_/g, '/');
    while (b.length % 4) b += '=';
    return Uint8Array.from(Buffer.from(b, 'base64'));
}
function makeSub(key, endpoint, log) {
    return {
        endpoint,
        options: { applicationServerKey: key ? key.buffer : null },
        toJSON() { return { endpoint, expirationTime: null, keys: { p256dh: 'p-' + endpoint, auth: 'a-' + endpoint } }; },
        unsubscribe() { log.push('unsubscribe ' + endpoint); browser.sub = null; return Promise.resolve(true); },
    };
}
const browser = { sub: null, log: [], n: 0, permission: 'granted', listeners: [], db: null };
function fakeIndexedDB() {
    const db = { rows: new Map(), next: 1, objectStoreNames: { contains: (n) => n === 'subscription-changes' && db.made } };
    const req = (fn) => { const r = {}; setTimeout(() => { r.result = fn(); if (r.onsuccess) r.onsuccess(); }, 0); return r; };
    const store = () => ({
        getAll: () => req(() => [...db.rows.values()]),
        delete: (id) => req(() => { db.rows.delete(id); }),
        add: (v) => req(() => { const id = db.next++; db.rows.set(id, Object.assign({ id }, v)); return id; }),
    });
    browser.db = db;
    return {
        open(name, version) {
            const r = {};
            setTimeout(() => {
                if (name !== 'webpush-storage' || version !== 1) { if (r.onerror) r.onerror(); return; }
                const handle = { objectStoreNames: db.objectStoreNames, transaction: () => ({ objectStore: () => store() }), close() {},
                                 createObjectStore: (n, o) => { db.made = n === 'subscription-changes' && o.keyPath === 'id' && o.autoIncrement; } };
                if (!db.made && r.onupgradeneeded) r.onupgradeneeded({ target: { result: handle } });
                r.result = handle;
                if (r.onsuccess) r.onsuccess();
            }, 0);
            return r;
        },
    };
}
const pushManager = {
    getSubscription: () => Promise.resolve(browser.sub),
    subscribe(opts) {
        browser.log.push('subscribe ' + Buffer.from(opts.applicationServerKey).toString('base64'));
        browser.sub = makeSub(opts.applicationServerKey, 'https://push.test/ep' + ++browser.n, browser.log);
        return Promise.resolve(browser.sub);
    },
};
const reg = { pushManager };
const page = {
    window: { isSecureContext: true, PushManager: function () {} },
    navigator: {
        serviceWorker: {
            register: () => Promise.resolve(reg), ready: Promise.resolve(reg), getRegistration: () => Promise.resolve(reg),
            addEventListener: (type, fn) => { if (type === 'message') browser.listeners.push(fn); },
        },
    },
    Notification: { permission: 'granted', requestPermission: () => Promise.resolve(browser.permission) },
    atob: (s) => Buffer.from(s, 'base64').toString('binary'),
    indexedDB: fakeIndexedDB(),
    setTimeout, Promise, Uint8Array, JSON,
    UTF8ToString: (p) => p, stringToNewUTF8: (s) => s,
    done: [], events: [],
    _kn_ui_push_done(req, json) { page.done.push([req, json]); },
    _kn_ui_push_event(fn, user, type, json) { page.events.push([type, json ? JSON.parse(json) : null]); },
};
page.window.knPushListen = undefined;
const ctx = vm.createContext(page);
vm.runInContext('var window = this.window; window.navigator = navigator;', ctx);
function call(name, ...args) {
    const f = emjs(name);
    vm.runInContext('(function (' + f.params.join(',') + ') {\n' + f.body + '\n})', ctx)(...args);
}

const BO = 'BOxGcOzDr3qcdOGs5eNPUFkNsR5w3HUP2x-jLJ2qvpPBWzTgDTGa7nbMSpbyZOm8RxAjeMh3hQRIDQv3VP1s0aY';
const KIO = 'BIQ4JkKF4R0khgycd7N447m-Uxk39zZBeHlW7-WzGeFNc3OhaZgh7k3_PiyHY5L_OTOoU7SU7KBVooddC8bNdcc';
const b64 = (k) => Buffer.from(b64urlBytes(k)).toString('base64');

(async function main() {
    // 1. subscribe with Box Office's key, then with krunker.io's: the Box Office one is let go first
    call('kn_js_push_subscribe', 'r1', BO);
    await settle();
    check('a first subscribe makes one with the key', page.done.length === 1 && page.done[0][1] && browser.log[0] === 'subscribe ' + b64(BO), browser.log);
    call('kn_js_push_subscribe', 'r2', BO);
    await settle();
    check('the same key: the existing subscription is reused', browser.log.length === 1 && JSON.parse(page.done[1][1]).endpoint === 'https://push.test/ep1',
          browser.log);
    call('kn_js_push_subscribe', 'r3', KIO);
    await settle();
    check("another key (krunker.io's): the old one is let go, then one with the new key",
          browser.log[1] === 'unsubscribe https://push.test/ep1' && browser.log[2] === 'subscribe ' + b64(KIO) &&
          JSON.parse(page.done[2][1]).endpoint === 'https://push.test/ep2', browser.log);
    browser.permission = 'denied';
    call('kn_js_push_subscribe', 'r4', KIO);
    await settle();
    check('permission not granted: no subscription (NULL)', page.done[3][0] === 'r4' && page.done[3][1] === 0, page.done[3]);
    browser.permission = 'granted';

    // 2. current: the subscription's JSON, nothing changed
    const logged = browser.log.length;
    call('kn_js_push_current', 'r5');
    await settle();
    check('current: the subscription as JSON, nothing changed',
          page.done[4][0] === 'r5' && JSON.parse(page.done[4][1]).endpoint === 'https://push.test/ep2' && browser.log.length === logged, page.done[4]);

    // 3. listen: queued changes are handed on once and deleted; the worker's messages
    const idb = page.indexedDB;
    await new Promise((resolve) => { // a change the worker queued earlier (and one without subscriptions: only deleted)
        const o = idb.open('webpush-storage', 1);
        o.onupgradeneeded = (ev) => ev.target.result.createObjectStore('subscription-changes', { keyPath: 'id', autoIncrement: true });
        o.onsuccess = () => {
            const st = o.result.transaction().objectStore();
            st.add({ old_subscription: { endpoint: 'old' }, new_subscription: { endpoint: 'new' } });
            const r = st.add({ note: 'empty' });
            r.onsuccess = resolve;
        };
    });
    call('kn_js_push_listen', KIO, 'fn', 'user');
    await settle();
    check('a queued change reaches C as "changed" {old_subscription, new_subscription}',
          page.events.length === 1 && page.events[0][0] === 'changed' && page.events[0][1].old_subscription.endpoint === 'old' &&
          page.events[0][1].new_subscription.endpoint === 'new', page.events);
    check('every queued record is deleted', browser.db.rows.size === 0, [...browser.db.rows.values()]);
    call('kn_js_push_listen', BO, 'fn', 'user');
    await settle();
    check('listen again: one listener, the queue already empty', browser.listeners.length === 1 && page.events.length === 1, browser.listeners.length);
    const answers = [];
    browser.listeners[0]({ data: { type: 'WEBPUSH_REQUEST_VAPID_KEY' }, source: { postMessage: (m) => answers.push(m) } });
    check('WEBPUSH_REQUEST_VAPID_KEY: answered with the current key', answers.length === 1 && answers[0].type === 'WEBPUSH_VAPID_KEY_RESPONSE' &&
          answers[0].vapidKey === BO, answers);
    browser.listeners[0]({ data: { type: 'WEBPUSH_SUBSCRIPTION_LOST' } });
    browser.listeners[0]({ data: { type: 'WEBPUSH_SUBSCRIPTION_CHANGED', data: { old_subscription: null, new_subscription: { endpoint: 'n2' } } } });
    browser.listeners[0]({ data: { type: 'SOMETHING_ELSE' } });
    check('WEBPUSH_SUBSCRIPTION_LOST / _CHANGED reach C; other messages do not',
          page.events.length === 3 && page.events[1][0] === 'lost' && page.events[2][0] === 'changed' &&
          page.events[2][1].new_subscription.endpoint === 'n2', page.events);

    // 4. ui/sw.js: pushsubscriptionchange
    const handlers = {};
    const posted = [];
    let windows = [{ postMessage: (m) => posted.push(m) }];
    const sw = {
        addEventListener: (type, fn) => { handlers[type] = fn; },
        registration: { pushManager, showNotification: () => Promise.resolve() },
        clients: { matchAll: () => Promise.resolve(windows), openWindow: () => Promise.resolve() },
        location: { origin: 'https://kn.test' },
    };
    sw.self = sw;
    const swctx = vm.createContext(Object.assign(sw, { setTimeout, Promise, Uint8Array, JSON, atob: page.atob, indexedDB: idb, URL }));
    vm.runInContext(fs.readFileSync(path.join(root, 'ui/sw.js'), 'utf8'), swctx);
    const fire = async (ev) => {
        let p = null;
        ev.waitUntil = (x) => { p = x; };
        handlers.pushsubscriptionchange(ev);
        await p;
        await settle();
    };
    const oldSub = makeSub(b64urlBytes(KIO), 'https://push.test/gone', browser.log);
    await fire({ oldSubscription: oldSub, newSubscription: null });
    check("sw: no new subscription: one with the old one's key, then WEBPUSH_SUBSCRIPTION_CHANGED to the page",
          posted.length === 1 && posted[0].type === 'WEBPUSH_SUBSCRIPTION_CHANGED' && posted[0].data.old_subscription.endpoint === 'https://push.test/gone' &&
          posted[0].data.new_subscription.endpoint === browser.sub.endpoint && browser.log[browser.log.length - 1] === 'subscribe ' + b64(KIO), posted);
    windows = [];
    await fire({ oldSubscription: oldSub, newSubscription: makeSub(null, 'https://push.test/fresh', browser.log) });
    check('sw: no page open: the change is queued in IndexedDB (webpush-storage / subscription-changes)',
          browser.db.rows.size === 1 && [...browser.db.rows.values()][0].new_subscription.endpoint === 'https://push.test/fresh', [...browser.db.rows.values()]);
    const keyAsk = [];
    windows = [{ postMessage: (m) => { keyAsk.push(m); if (m.type === 'WEBPUSH_REQUEST_VAPID_KEY') setTimeout(() => handlers.message({ data: { type: 'WEBPUSH_VAPID_KEY_RESPONSE', vapidKey: KIO } }), 0); } }];
    await fire({ oldSubscription: makeSub(null, 'https://push.test/nokey', browser.log), newSubscription: null });
    check("sw: an old subscription without its key: the page's (WEBPUSH_REQUEST_VAPID_KEY) is used",
          keyAsk[0] && keyAsk[0].type === 'WEBPUSH_REQUEST_VAPID_KEY' && keyAsk[1] && keyAsk[1].type === 'WEBPUSH_SUBSCRIPTION_CHANGED', keyAsk);
    const lost = [];
    windows = [{ postMessage: (m) => { lost.push(m); if (m.type === 'WEBPUSH_REQUEST_VAPID_KEY') setTimeout(() => handlers.message({ data: { type: 'WEBPUSH_VAPID_KEY_RESPONSE', vapidKey: null } }), 0); } }];
    await fire({ oldSubscription: null, newSubscription: null });
    check('sw: no key anywhere: WEBPUSH_SUBSCRIPTION_LOST', lost.some((m) => m.type === 'WEBPUSH_SUBSCRIPTION_LOST'), lost);

    console.log(pass + '/' + (pass + fail) + ' passed');
    process.exit(fail ? 1 : 0);
})().catch((e) => {
    console.log('FAIL  ' + (e && e.stack || e));
    process.exit(1);
});
