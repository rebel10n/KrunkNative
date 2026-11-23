// Krunker Box Office's service worker (web build): it shows the Web Push messages (the brain's, or krunker.io's for
// a subscription made on a krunker.io account) as notifications.
// Served at /sw.js (the page registers it from client/src/html/web_dom.c; the menu's Notifications panel is
// client/src/ui/web_push.c). Glue only: no game or UI logic lives here.
//
// A push carries JSON {title, text, tag, url}; an empty or unreadable payload still shows a notification.

self.addEventListener('push', function (event) {
    var data = {};
    try {
        data = event.data ? event.data.json() : {};
    } catch (e) {
        data = {};
    }
    var title = (data && data.title) || 'Krunker Box Office';
    var options = {
        body: (data && data.text) || '',
        tag: (data && data.tag) || undefined,
        icon: '/img/pwa/icon-192.png',
        data: { url: (data && typeof data.url === 'string' && data.url.charAt(0) === '/' && data.url.charAt(1) !== '/') ? data.url : '/' }
    };
    event.waitUntil(self.registration.showNotification(title, options));
});

// a click closes the notification and focuses an open window of this origin (or opens the game)
self.addEventListener('notificationclick', function (event) {
    event.notification.close();
    var url = (event.notification.data && event.notification.data.url) || '/';
    event.waitUntil(self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then(function (list) {
        for (var i = 0; i < list.length; i++) {
            if (list[i].url && new URL(list[i].url).origin === self.location.origin && 'focus' in list[i]) {
                return list[i].focus();
            }
        }
        return self.clients.openWindow(url);
    }));
});

// The browser replaced or dropped the subscription (pushsubscriptionchange). Krunker v10.0.7's page takes three
// messages from its worker (deob-10.0.7.js:148986-149003): WEBPUSH_REQUEST_VAPID_KEY (it answers
// WEBPUSH_VAPID_KEY_RESPONSE {vapidKey}), WEBPUSH_SUBSCRIPTION_CHANGED {data: {old_subscription, new_subscription}}
// (it sends /webpush/subscription-change) and WEBPUSH_SUBSCRIPTION_LOST; a change made while no page is open waits
// in IndexedDB "webpush-storage" / "subscription-changes" (keyPath id), which the page reads at load (:147015-147060).
// Krunker's own worker is not in the bundle: this one makes those messages and records.
var vapidWaiters = [];

self.addEventListener('message', function (event) {
    var d = event.data;
    if (!d || d.type !== 'WEBPUSH_VAPID_KEY_RESPONSE') return;
    var waiting = vapidWaiters;
    vapidWaiters = [];
    for (var i = 0; i < waiting.length; i++) waiting[i](typeof d.vapidKey === 'string' ? d.vapidKey : null);
});

function windows() {
    return self.clients.matchAll({ type: 'window', includeUncontrolled: true });
}

// the page's VAPID key (null when no page answers within 5 s)
function vapidFromPage() {
    return windows().then(function (list) {
        if (!list.length) return null;
        return new Promise(function (resolve) {
            vapidWaiters.push(resolve);
            setTimeout(function () { resolve(null); }, 5000);
            list[0].postMessage({ type: 'WEBPUSH_REQUEST_VAPID_KEY' });
        });
    });
}

function keyBytes(key) {
    if (!key) return null;
    if (typeof key !== 'string') return key;
    var b64 = key.replace(/-/g, '+').replace(/_/g, '/');
    while (b64.length % 4) b64 += '=';
    var raw = atob(b64), bytes = new Uint8Array(raw.length);
    for (var i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
    return bytes;
}

function queueChange(change) {
    return new Promise(function (resolve) {
        var open = indexedDB.open('webpush-storage', 1);
        open.onupgradeneeded = function (ev) {
            var db = ev.target.result;
            if (!db.objectStoreNames.contains('subscription-changes')) db.createObjectStore('subscription-changes', { keyPath: 'id', autoIncrement: true });
        };
        open.onsuccess = function () {
            var db = open.result;
            var add = db.transaction(['subscription-changes'], 'readwrite').objectStore('subscription-changes').add(change);
            add.onsuccess = add.onerror = function () { db.close(); resolve(); };
        };
        open.onerror = function () { resolve(); };
    });
}

self.addEventListener('pushsubscriptionchange', function (event) {
    var old = event.oldSubscription || null;
    var made = event.newSubscription ? Promise.resolve(event.newSubscription) : Promise.resolve(
        old && old.options && old.options.applicationServerKey ? old.options.applicationServerKey : vapidFromPage()
    ).then(function (key) {
        if (!key) throw new Error('no key');
        return self.registration.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: keyBytes(key) });
    });
    event.waitUntil(made.then(function (sub) {
        var change = { old_subscription: old ? old.toJSON() : null, new_subscription: sub.toJSON() };
        return windows().then(function (list) {
            if (!list.length) return queueChange(change);
            for (var i = 0; i < list.length; i++) list[i].postMessage({ type: 'WEBPUSH_SUBSCRIPTION_CHANGED', data: change });
        });
    }).catch(function () {
        return windows().then(function (list) {
            for (var i = 0; i < list.length; i++) list[i].postMessage({ type: 'WEBPUSH_SUBSCRIPTION_LOST' });
        });
    }));
});
