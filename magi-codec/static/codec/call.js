/* Codec call page: /codec/<callId> */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const body = document.body;
  const callId = decodeURIComponent(location.pathname.split('/').filter(Boolean).pop() || '');
  const CHAR_MS = 38;

  let data = null;
  let audio = null;
  let typing = null;       // { finish() }
  let done = false;
  let ending = false;

  function setState(s) { body.dataset.state = s; }

  // ---------------------------------------------------------------- signal bar
  const BARS = 16;
  const signal = $('signal');
  const bars = Array.from({ length: BARS }, () => signal.appendChild(document.createElement('i')));
  let level = 0.25;
  function animateSignal() {
    const st = body.dataset.state;
    const target = st === 'ringing' ? 0.35 + Math.random() * 0.25
      : st === 'talking' ? 0.55 + Math.random() * 0.45
      : st === 'connected' || st === 'replay' ? 0.4 + Math.random() * 0.15 : 0.1;
    level += (target - level) * 0.35;
    bars.forEach((b, i) => {
      const lit = i / BARS < level;
      b.classList.toggle('on', lit);
      const h = 0.2 + 0.8 * ((i + 1) / BARS);
      b.style.transform = `scaleY(${lit ? h : 0.12})`;
    });
  }
  setInterval(animateSignal, 90);

  function tickClock() {
    const d = new Date();
    $('clock').textContent = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  }
  setInterval(tickClock, 1000); tickClock();

  // ---------------------------------------------------------------- typewriter
  function typeOut(title, message) {
    const box = $('text');
    box.replaceChildren();
    const titleEl = document.createElement('span');
    titleEl.className = 'title';
    const msgEl = document.createElement('span');
    const cursor = document.createElement('span');
    cursor.className = 'cursor'; cursor.textContent = ' ';
    box.append(titleEl, msgEl, cursor);

    const parts = [[titleEl, title ? '[ ' + title.toUpperCase() + ' ]' : ''], [msgEl, message || '']];
    let p = 0, i = 0, timer = null, resolveFn;
    const finished = new Promise((r) => { resolveFn = r; });

    function step() {
      while (p < parts.length && i >= parts[p][1].length) { p++; i = 0; }
      if (p >= parts.length) { clearInterval(timer); resolveFn(); return; }
      const [elx, str] = parts[p];
      const ch = str[i++];
      elx.textContent += ch;
      if (ch.trim() && i % 2 === 0) audio && audio.blip();
      box.scrollTop = box.scrollHeight;
    }
    body.classList.add('talking');
    timer = setInterval(step, CHAR_MS);
    return {
      finished: finished.then(() => body.classList.remove('talking')),
      finish() {
        clearInterval(timer);
        parts.forEach(([elx, str]) => { elx.textContent = str; });
        p = parts.length;
        resolveFn();
      },
    };
  }

  // ---------------------------------------------------------------- flow
  async function connect(replay) {
    $('answerBtn').disabled = true;
    if (audio) { await audio.unlock(); audio.stopRing(); }
    if (!replay) {
      fetch('/api/codec/calls/' + encodeURIComponent(callId) + '/answered', { method: 'POST' })
        .catch(() => {});
    }
    setState('connecting');
    const ms = audio ? audio.answer() : 600;
    await new Promise((r) => setTimeout(r, replay ? Math.min(ms, 500) : ms));
    body.classList.add('connected');
    setState('talking');
    $('incoming').textContent = replay ? 'REPLAY' : 'CONNECTED';
    typing = typeOut(data.call.title, data.call.text);
    await typing.finished;
    typing = null;
    done = true;
    setState('connected');
    $('prompt').textContent = '▼ TAP TO END';
  }

  async function hangUp() {
    if (ending) return;
    ending = true;
    const ms = audio ? audio.close() : 700;
    runStatic(ms + 300);
    setState('ending');
    await new Promise((r) => setTimeout(r, ms + 500));
    goHome();
  }

  function goHome() {
    const url = (data && data.dashboard_url) || '/';
    if (history.length > 1 && document.referrer && new URL(document.referrer).origin === location.origin) {
      history.back();
      setTimeout(() => location.replace(url), 400); // if back() did nothing
    } else {
      location.replace(url);
    }
  }

  function runStatic(ms) {
    const c = $('staticCanvas');
    const w = (c.width = 160), h = (c.height = 100);
    const ctx = c.getContext('2d');
    const img = ctx.createImageData(w, h);
    const t0 = performance.now();
    (function frame(t) {
      const k = Math.min(1, (t - t0) / ms);
      for (let i = 0; i < img.data.length; i += 4) {
        const v = Math.random() * 255 * (1 - k * 0.6);
        img.data[i] = v * 0.45; img.data[i + 1] = v; img.data[i + 2] = v * 0.55; img.data[i + 3] = 255 * (1 - k * 0.5);
      }
      ctx.putImageData(img, 0, 0);
      if (k < 1) requestAnimationFrame(frame);
    })(t0);
  }

  // Tap anywhere: skip typing, or end the call once the text is done.
  document.addEventListener('click', (e) => {
    if (e.target.closest('#answerOverlay')) return;
    const st = body.dataset.state;
    if (typing) { typing.finish(); return; }
    if (done && (st === 'connected')) hangUp();
    if (st === 'error') goHome();
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && done) hangUp();
  });

  function showError(msg) {
    setState('error');
    $('incoming').textContent = 'NO SIGNAL';
    $('callerName').textContent = '----';
    $('text').textContent = msg;
    $('prompt').textContent = '▼ TAP TO RETURN';
  }

  async function init() {
    if (!callId) return showError('No call selected.');
    try {
      const r = await fetch('/api/codec/calls/' + encodeURIComponent(callId), { cache: 'no-store' });
      if (r.status === 404) return showError('This call is not in the log (it may have been cleared).');
      if (!r.ok) throw new Error('HTTP ' + r.status);
      data = await r.json();
    } catch (e) {
      return showError('Could not reach Magi: ' + e.message);
    }
    const { call, caller } = data;
    document.title = 'Codec · ' + call.caller_name;
    $('callerName').textContent = call.caller_name;
    $('freq').textContent = call.frequency;
    $('role').textContent = caller.role || '';
    const img = $('portrait');
    img.alt = call.caller_name + ' portrait';
    if (caller.portrait) img.src = caller.portrait;
    img.onerror = () => { img.removeAttribute('src'); };

    audio = window.CodecAudio ? new window.CodecAudio(data.sounds) : null;
    const replay = !!call.answered || new URLSearchParams(location.search).has('replay');

    if (replay) {
      setState('replay');
      $('incoming').textContent = 'REPLAY';
      $('answerBtn').textContent = 'PLAY';
      $('answerHint').textContent = 'Replay this call';
    } else {
      setState('ringing');
      // Works only if the browser already allows audio (often it does not until a tap).
      if (audio) { audio.ready.then(() => audio.startRing()); }
      $('answerHint').textContent = audio && audio.running ? 'Tap to answer' : 'Tap to answer (ring needs a tap to start audio)';
    }
    $('answerBtn').addEventListener('click', () => connect(replay), { once: true });
  }

  init();
})();
