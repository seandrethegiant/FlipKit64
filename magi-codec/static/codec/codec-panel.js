/* Magi Codec dashboard panel.
   Include once on the dashboard:  <script src="/static/codec/codec-panel.js" defer></script>
   It fills <div id="codec-panel"></div> (or appends one to <body> if missing). */
(function () {
  'use strict';

  function ensureHead() {
    const head = document.head;
    if (!head.querySelector('link[rel="manifest"]')) {
      const l = document.createElement('link');
      l.rel = 'manifest'; l.href = '/manifest.webmanifest'; head.appendChild(l);
    }
    if (!head.querySelector('meta[name="theme-color"]')) {
      const m = document.createElement('meta');
      m.name = 'theme-color'; m.content = '#06140a'; head.appendChild(m);
    }
    if (!head.querySelector('link[data-codec-css]')) {
      const c = document.createElement('link');
      c.rel = 'stylesheet'; c.href = '/static/codec/codec-panel.css'; c.dataset.codecCss = '1';
      head.appendChild(c);
    }
  }

  function el(tag, attrs, ...kids) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') e.className = v; else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else e.setAttribute(k, v);
    }
    for (const k of kids) if (k != null) e.append(k);
    return e;
  }

  function b64ToBytes(s) {
    const pad = '='.repeat((4 - (s.length % 4)) % 4);
    const raw = atob((s + pad).replace(/-/g, '+').replace(/_/g, '/'));
    return Uint8Array.from(raw, (c) => c.charCodeAt(0));
  }
  function bytesEqual(a, b) {
    if (!a || !b) return false;
    a = new Uint8Array(a); b = new Uint8Array(b);
    return a.length === b.length && a.every((v, i) => v === b[i]);
  }

  async function api(path, body) {
    const opt = body === undefined ? {} : {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    };
    const r = await fetch(path, opt);
    let j = {};
    try { j = await r.json(); } catch (e) { /* non-JSON */ }
    if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
    return j;
  }

  const supported = () => 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;
  const installed = () => matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;

  let reg = null;
  const ui = {};

  async function registration() {
    if (reg) return reg;
    reg = await navigator.serviceWorker.register('/sw.js', { scope: '/' });
    await navigator.serviceWorker.ready;
    return reg;
  }

  function setStatus(state, detail) {
    ui.status.dataset.state = state;
    ui.statusText.textContent = {
      enabled: 'ENABLED', off: 'OFF', blocked: 'BLOCKED', unsupported: 'UNAVAILABLE', working: 'WORKING…',
    }[state] || state;
    ui.detail.textContent = detail || '';
    ui.enable.hidden = state === 'enabled' || state === 'unsupported';
    ui.disable.hidden = state !== 'enabled';
    ui.test.disabled = state !== 'enabled';
  }

  async function refreshStatus() {
    ui.install.textContent = installed() ? 'Installed app' : 'Not installed: Chrome menu > Add to Home screen';
    ui.install.dataset.ok = installed() ? '1' : '0';
    if (!window.isSecureContext) {
      return setStatus('unsupported', 'Needs HTTPS. Open Magi at your https://…ts.net address.');
    }
    if (!supported()) return setStatus('unsupported', 'This browser has no Web Push support.');
    try { await registration(); } catch (e) { return setStatus('off', 'Service worker error: ' + e.message); }
    if (Notification.permission === 'denied') {
      return setStatus('blocked', 'Notifications are blocked. Site settings > Notifications > Allow, then reload.');
    }
    try {
      const r = await registration();
      const sub = await r.pushManager.getSubscription();
      if (sub && Notification.permission === 'granted') {
        const s = await api('/api/push/status', { endpoint: sub.endpoint });
        if (s.subscribed) return setStatus('enabled', 'This device will ring for Codec calls.');
        return setStatus('off', 'Magi lost this device\'s subscription. Press Enable again.');
      }
      setStatus('off', 'Press Enable to get calls on this device.');
    } catch (e) {
      setStatus('off', 'Service worker error: ' + e.message);
    }
  }

  async function enable() {
    setStatus('working');
    try {
      const perm = await Notification.requestPermission();
      if (perm !== 'granted') return refreshStatus();
      const r = await registration();
      const { publicKey } = await api('/api/push/public-key');
      const key = b64ToBytes(publicKey);
      let sub = await r.pushManager.getSubscription();
      if (sub && !bytesEqual(sub.options && sub.options.applicationServerKey, key)) {
        await sub.unsubscribe(); sub = null; // Magi's keys changed
      }
      if (!sub) sub = await r.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: key });
      const label = (navigator.userAgentData && navigator.userAgentData.platform) ||
        navigator.platform || 'device';
      await api('/api/push/subscribe', { subscription: sub.toJSON(), label: label + (installed() ? ' (app)' : '') });
    } catch (e) {
      ui.detail.textContent = 'Enable failed: ' + e.message;
      return;
    }
    refreshStatus();
  }

  async function disable() {
    setStatus('working');
    try {
      const r = await registration();
      const sub = await r.pushManager.getSubscription();
      if (sub) {
        await api('/api/push/unsubscribe', { endpoint: sub.endpoint }).catch(() => {});
        await sub.unsubscribe();
      }
    } catch (e) { /* ignore */ }
    refreshStatus();
  }

  async function sendTest() {
    ui.test.disabled = true;
    ui.testResult.textContent = 'Dialing…';
    try {
      const j = await api('/api/push/test', {});
      const p = j.push || {};
      ui.testResult.textContent = `Sent to ${p.sent || 0} device(s)` +
        (p.failed ? `, ${p.failed} failed` : '') + (p.removed ? `, ${p.removed} expired removed` : '') +
        '. Close the app to see it as a notification.';
      loadLog();
    } catch (e) {
      ui.testResult.textContent = 'Test failed: ' + e.message;
    }
    setTimeout(() => { ui.test.disabled = ui.status.dataset.state !== 'enabled'; }, 3000);
  }

  const TYPE_LABEL = { conversion_done: 'Conversion', disk_low: 'Disk low', camera_motion: 'Motion', test: 'Test' };

  async function loadLog() {
    try {
      const { calls } = await api('/api/codec/calls?limit=25');
      ui.log.replaceChildren();
      if (!calls.length) {
        ui.log.append(el('tr', {}, el('td', { colspan: '4', class: 'codec-empty' }, 'No calls yet.')));
        return;
      }
      for (const c of calls) {
        const t = new Date(c.epoch * 1000);
        const when = t.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' +
          t.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        const row = el('tr', { class: 'codec-row', tabindex: '0', title: c.title || '' },
          el('td', {}, when),
          el('td', {}, c.caller_name + ' ', el('span', { class: 'codec-freq' }, c.frequency)),
          el('td', {}, TYPE_LABEL[c.type] || c.type),
          el('td', { class: c.answered ? 'codec-ans' : 'codec-miss' }, c.answered ? 'Answered' : 'Missed'));
        const open = () => { location.href = '/codec/' + encodeURIComponent(c.id); };
        row.addEventListener('click', open);
        row.addEventListener('keydown', (e) => { if (e.key === 'Enter') open(); });
        ui.log.append(row);
      }
    } catch (e) {
      ui.log.replaceChildren(el('tr', {}, el('td', { colspan: '4', class: 'codec-empty' }, 'Call log unavailable: ' + e.message)));
    }
  }

  function build(root) {
    ui.status = el('span', { class: 'codec-status' }, ui.statusText = el('b', {}, '…'));
    ui.detail = el('div', { class: 'codec-detail' });
    ui.install = el('div', { class: 'codec-install' });
    ui.enable = el('button', { class: 'codec-btn', type: 'button', onclick: enable }, 'Enable Codec alerts');
    ui.disable = el('button', { class: 'codec-btn codec-btn-quiet', type: 'button', onclick: disable }, 'Disable on this device');
    ui.test = el('button', { class: 'codec-btn', type: 'button', onclick: sendTest }, 'Send test call');
    ui.testResult = el('div', { class: 'codec-detail' });
    ui.log = el('tbody');
    root.classList.add('codec-panel');
    root.replaceChildren(
      el('div', { class: 'codec-head' }, el('h2', {}, 'Codec alerts'), ui.status),
      ui.detail, ui.install,
      el('div', { class: 'codec-actions' }, ui.enable, ui.disable, ui.test),
      ui.testResult,
      el('p', { class: 'codec-note' },
        'Android plays its normal notification sound for the alert (Web Push cannot set a custom one). ' +
        'The ring plays on the call page.'),
      el('h3', {}, 'Call log'),
      el('table', { class: 'codec-log' },
        el('thead', {}, el('tr', {}, el('th', {}, 'Time'), el('th', {}, 'Caller'), el('th', {}, 'Type'), el('th', {}, 'Status'))),
        ui.log));
  }

  function start() {
    ensureHead();
    let root = document.getElementById('codec-panel');
    if (!root) { root = el('section', { id: 'codec-panel' }); document.body.append(root); }
    build(root);
    refreshStatus();
    loadLog();
    setInterval(loadLog, 30000);
    document.addEventListener('visibilitychange', () => { if (!document.hidden) { refreshStatus(); loadLog(); } });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start); else start();
})();
