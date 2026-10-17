// Krunker Box Office's service worker (web build): it shows the brain's Web Push messages as notifications.
// Served at /sw.js (the page registers it from client/src/html/web_dom.c; the menu's Notifications switch is
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
