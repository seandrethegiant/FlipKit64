/* Codec sounds. Uses your own files from /static/codec/sounds/ when present
   (ring, answer, blip, close), otherwise synthesizes them with Web Audio. */
(function () {
  'use strict';

  class CodecAudio {
    constructor(soundUrls) {
      this.urls = soundUrls || {};
      this.buffers = {};
      this.ctx = null;
      this.ringTimer = null;
      this.ringSource = null;
      try {
        const AC = window.AudioContext || window.webkitAudioContext;
        this.ctx = AC ? new AC() : null;
      } catch (e) { this.ctx = null; }
      if (this.ctx) {
        this.master = this.ctx.createGain();
        this.master.gain.value = 0.6;
        this.master.connect(this.ctx.destination);
      }
      this.ready = this._load();
    }

    get running() { return !!this.ctx && this.ctx.state === 'running'; }

    async unlock() {           // call from a tap
      if (!this.ctx) return false;
      try { await this.ctx.resume(); } catch (e) { /* ignore */ }
      return this.running;
    }

    async _load() {
      if (!this.ctx) return;
      await Promise.all(Object.entries(this.urls).map(async ([slot, url]) => {
        try {
          const r = await fetch(url);
          if (!r.ok) return;
          const data = await r.arrayBuffer();
          this.buffers[slot] = await new Promise((res, rej) => this.ctx.decodeAudioData(data, res, rej));
        } catch (e) { console.warn('codec: could not load sound', slot, e); }
      }));
    }

    _playBuffer(buf, { loop = false, gain = 1 } = {}) {
      const src = this.ctx.createBufferSource();
      const g = this.ctx.createGain();
      g.gain.value = gain;
      src.buffer = buf; src.loop = loop;
      src.connect(g).connect(this.master);
      src.start();
      return src;
    }

    _tone(freq, start, dur, { type = 'square', gain = 0.12, endFreq = null } = {}) {
      const t = this.ctx.currentTime + start;
      const o = this.ctx.createOscillator();
      const g = this.ctx.createGain();
      o.type = type;
      o.frequency.setValueAtTime(freq, t);
      if (endFreq) o.frequency.exponentialRampToValueAtTime(endFreq, t + dur);
      g.gain.setValueAtTime(0, t);
      g.gain.linearRampToValueAtTime(gain, t + 0.005);
      g.gain.setValueAtTime(gain, t + dur - 0.01);
      g.gain.linearRampToValueAtTime(0, t + dur);
      o.connect(g).connect(this.master);
      o.start(t); o.stop(t + dur + 0.02);
    }

    _noise(start, dur, { gain = 0.25, from = 6000, to = 300 } = {}) {
      const ctx = this.ctx, t = ctx.currentTime + start;
      const len = Math.max(1, Math.floor(ctx.sampleRate * dur));
      const buf = ctx.createBuffer(1, len, ctx.sampleRate);
      const d = buf.getChannelData(0);
      for (let i = 0; i < len; i++) d[i] = Math.random() * 2 - 1;
      const src = ctx.createBufferSource();
      const f = ctx.createBiquadFilter();
      const g = ctx.createGain();
      f.type = 'lowpass';
      f.frequency.setValueAtTime(from, t);
      f.frequency.exponentialRampToValueAtTime(to, t + dur);
      g.gain.setValueAtTime(gain, t);
      g.gain.exponentialRampToValueAtTime(0.001, t + dur);
      src.buffer = buf;
      src.connect(f).connect(g).connect(this.master);
      src.start(t); src.stop(t + dur);
    }

    /* Two-tone trill: high/low alternating bursts, then a pause. */
    _ringOnce() {
      for (let i = 0; i < 8; i++) {
        this._tone(i % 2 ? 1046 : 1397, i * 0.07, 0.065, { type: 'square', gain: 0.09 });
      }
      for (let i = 0; i < 8; i++) {
        this._tone(i % 2 ? 1046 : 1397, 0.75 + i * 0.07, 0.065, { type: 'square', gain: 0.09 });
      }
    }

    startRing() {
      if (!this.running || this.ringTimer || this.ringSource) return;
      if (this.buffers.ring) { this.ringSource = this._playBuffer(this.buffers.ring, { loop: true }); return; }
      this._ringOnce();
      this.ringTimer = setInterval(() => this._ringOnce(), 2200);
    }

    stopRing() {
      if (this.ringTimer) { clearInterval(this.ringTimer); this.ringTimer = null; }
      if (this.ringSource) { try { this.ringSource.stop(); } catch (e) { /* ignore */ } this.ringSource = null; }
    }

    /* Ring-to-connect: short last ring, rising sweep, click of the line opening. Returns ms. */
    answer() {
      if (!this.running) return 600;
      if (this.buffers.answer) { this._playBuffer(this.buffers.answer); return Math.min(4000, this.buffers.answer.duration * 1000); }
      for (let i = 0; i < 4; i++) this._tone(i % 2 ? 1046 : 1397, i * 0.06, 0.055, { gain: 0.08 });
      this._noise(0.26, 0.18, { gain: 0.18, from: 8000, to: 1500 });
      this._tone(500, 0.42, 0.28, { type: 'sawtooth', gain: 0.07, endFreq: 1900 });
      this._tone(1900, 0.72, 0.09, { type: 'sine', gain: 0.12 });
      this._tone(2400, 0.84, 0.12, { type: 'sine', gain: 0.1 });
      return 1000;
    }

    blip() {
      if (!this.running) return;
      if (this.buffers.blip) { this._playBuffer(this.buffers.blip, { gain: 0.7 }); return; }
      this._tone(1650 + Math.random() * 250, 0, 0.022, { type: 'square', gain: 0.035 });
    }

    /* Static-out on hang up. Returns ms. */
    close() {
      if (!this.running) return 700;
      if (this.buffers.close) { this._playBuffer(this.buffers.close); return Math.min(3000, this.buffers.close.duration * 1000); }
      this._tone(1900, 0, 0.06, { type: 'sine', gain: 0.1 });
      this._tone(1200, 0.07, 0.08, { type: 'sine', gain: 0.1 });
      this._noise(0.12, 0.6, { gain: 0.3, from: 7000, to: 200 });
      return 800;
    }
  }

  window.CodecAudio = CodecAudio;
})();
