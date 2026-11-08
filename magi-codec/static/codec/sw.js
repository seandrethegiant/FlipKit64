/* Magi service worker: Codec call push notifications.
   Served at /sw.js (scope "/"). No offline caching. */
'use strict';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('push', (event) => {
  let d = {};
  try { d = event.data ? event.data.json() : {}; } catch (e) { d = { body: event.data && event.data.text() }; }
  const callId = d.callId || '';
  const url = d.url || (callId ? '/codec/' + callId : '/');
  event.waitUntil(self.registration.showNotification(d.title || 'INCOMING CALL', {
    body: d.body || 'Codec signal',
    tag: callId || 'codec',
    renotify: true,
    requireInteraction: true,
    vibrate: d.vibrate || [700, 150, 200, 150, 700],
    icon: d.icon || '/static/codec/icons/icon-192.png',
    badge: d.badge || '/static/codec/icons/badge-96.png',
    timestamp: d.ts || Date.now(),
    data: { url, callId },
    actions: [{ action: 'answer', title: 'ANSWER' }],
  }));
});

self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  const target = new URL((event.notification.data && event.notification.data.url) || '/', self.location.origin).href;
  event.waitUntil((async () => {
    const wins = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    for (const c of wins) {
      if (new URL(c.url).origin !== self.location.origin) continue;
      try {
        const focused = await c.focus();
        await (focused || c).navigate(target);
        return;
      } catch (e) { /* uncontrolled tab: fall through to openWindow */ }
    }
    await self.clients.openWindow(target);
  })());
});
