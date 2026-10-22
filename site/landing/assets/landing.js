// RFB landing page: hero frame-time demo, comparison slider, scroll states. No libraries.
(() => {
  'use strict';
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const RED = '#e10600', TEXT = '#f5f5f5', DIM = '#8a8a8a', LINE = '#262626', SURF = '#141414';
  const MONO = '"JetBrains Mono", Consolas, monospace';

  // deterministic noise, so the demo looks the same on every load
  const hash = n => { const x = Math.sin(n * 127.1 + 311.7) * 43758.5453; return x - Math.floor(x); };
  const clamp = (v, a = 0, b = 1) => Math.min(b, Math.max(a, v));
  const lerp = (a, b, p) => a + (b - a) * p;

  // ---------- mobile menu ----------
  const menuBtn = document.querySelector('.nav-menu'), links = document.getElementById('nav-links');
  if (menuBtn && links) {
    menuBtn.addEventListener('click', () => {
      const open = links.classList.toggle('open');
      menuBtn.setAttribute('aria-expanded', String(open));
    });
    links.addEventListener('click', e => { if (e.target.closest('a')) { links.classList.remove('open'); menuBtn.setAttribute('aria-expanded', 'false'); } });
  }

  // ---------- shared chart helpers ----------
  function fitCanvas(cv) {
    const dpr = Math.min(2, window.devicePixelRatio || 1), r = cv.getBoundingClientRect();
    const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
    const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { g, w: r.width, h: r.height };
  }
  // frame-time axis: 0-40 ms, grid at the 120, 60 and 30 fps marks
  function axis(g, w, h, pad) {
    g.fillStyle = SURF; g.fillRect(0, 0, w, h);
    g.font = `12px ${MONO}`; g.textBaseline = 'middle';
    for (const ms of [8.3, 16.7, 33.3]) {
      const y = yOf(ms, h, pad);
      g.strokeStyle = LINE; g.lineWidth = 1; g.beginPath(); g.moveTo(pad.l, y); g.lineTo(w - pad.r, y); g.stroke();
      g.fillStyle = DIM; g.fillText(ms.toFixed(1).replace('.', ',') + ' ms', 12, y);
    }
  }
  const yOf = (ms, h, pad) => pad.t + (1 - clamp(ms / 40)) * (h - pad.t - pad.b);
  function series(g, vals, x0, x1, h, pad, opts = {}) {
    const n = vals.length, dx = (x1 - x0) / (n - 1);
    g.lineWidth = 2; g.lineJoin = 'round';
    for (let i = 1; i < n; i++) {
      const hot = !opts.calm && (vals[i] > 18 || vals[i - 1] > 18);
      g.strokeStyle = hot ? RED : TEXT;
      g.beginPath(); g.moveTo(x0 + (i - 1) * dx, yOf(vals[i - 1], h, pad)); g.lineTo(x0 + i * dx, yOf(vals[i], h, pad)); g.stroke();
    }
    if (!opts.calm) {                      // mark every spike, red square on the peak
      g.fillStyle = RED;
      for (let i = 1; i < n - 1; i++) if (vals[i] > 18 && vals[i] >= vals[i - 1] && vals[i] >= vals[i + 1]) g.fillRect(x0 + i * dx - 4, yOf(vals[i], h, pad) - 4, 8, 8);
    }
  }
  const BASE = 11.1;                       // the same average before and after: the demo shows stutter going away, not a speed-up
  const rough = k => BASE + (hash(k) - 0.5) * 2.6 + (hash(k + 999) > 0.86 ? 9 + hash(k + 77) * 19 : 0);
  const calm = k => BASE + (hash(k + 4242) - 0.5) * 0.7;

  // ---------- hero: rough curve, RESET after 1.5 s, then only a gentle drift ----------
  const heroCv = document.getElementById('hero-chart');
  if (heroCv) {
    const N = 90, RESET_AT = 1.5, pad = { l: 78, r: 18, t: 54, b: 22 };
    let t0 = null, raf = 0, visible = true, lastDraw = 0;
    function draw(t) {
      const { g, w, h } = fitCanvas(heroCv);
      axis(g, w, h, pad);
      const shift = reduce ? 0 : (t < RESET_AT ? t * 14 : RESET_AT * 14 + (t - RESET_AT) * 5);
      const k0 = Math.floor(shift), vals = [];
      let hot = false;
      for (let i = 0; i < N; i++) {
        const k = k0 + i, x = i / (N - 1);
        const b = reduce ? 1 : clamp((t - RESET_AT - x * 0.3) / 0.25);   // the reset wipes from left to right
        const v = lerp(rough(k), calm(k), b);
        if (v > 18) hot = true;
        vals.push(v);
      }
      series(g, vals, pad.l, w - pad.r, h, pad, { calm: !hot });
      g.font = `12px ${MONO}`; g.textBaseline = 'middle'; g.fillStyle = DIM;
      g.fillText('FRAME-TIME', pad.l, 22);
      const label = hot ? 'STUTTER' : 'RESET';                 // status chip top right
      g.font = `600 13px ${MONO}`;
      const tw = g.measureText(label).width + 20, cx = w - pad.r - tw;
      g.fillStyle = hot ? RED : TEXT; g.fillRect(cx, 10, tw, 26);
      g.fillStyle = hot ? '#fff' : '#0a0a0a'; g.fillText(label, cx + 10, 23);
    }
    const loop = now => {
      if (t0 === null) t0 = now;
      const t = (now - t0) / 1000;
      // full rate until the reset has settled, then about 30 fps for the drift
      if (t < RESET_AT + 1 || now - lastDraw > 32) { draw(t); lastDraw = now; }
      if (visible && !document.hidden) raf = requestAnimationFrame(loop); else raf = 0;
    };
    const start = () => { if (!raf && !reduce) raf = requestAnimationFrame(loop); };
    if (reduce) draw(99);
    else {
      new IntersectionObserver(es => { visible = es[0].isIntersecting; if (visible) start(); }).observe(heroCv);
      document.addEventListener('visibilitychange', () => { if (!document.hidden) start(); });
      start();
    }
    new ResizeObserver(() => { if (reduce || !raf) draw(reduce ? 99 : (t0 === null ? 0 : (performance.now() - t0) / 1000)); }).observe(heroCv);
  }

  // ---------- comparison: one curve, split by the slider into "aus" and "an" ----------
  const cmpCv = document.getElementById('cmp-chart'), range = document.getElementById('cmp-range');
  if (cmpCv && range) {
    const N = 160, pad = { l: 78, r: 18, t: 24, b: 22 };
    const off = Array.from({ length: N }, (_, i) => rough(i + 500)), on = Array.from({ length: N }, (_, i) => calm(i + 500));
    function draw() {
      const { g, w, h } = fitCanvas(cmpCv), s = range.value / 100, split = pad.l + s * (w - pad.l - pad.r);
      axis(g, w, h, pad);
      g.save(); g.beginPath(); g.rect(0, 0, split, h); g.clip(); series(g, off, pad.l, w - pad.r, h, pad); g.restore();
      g.save(); g.beginPath(); g.rect(split, 0, w - split, h); g.clip(); series(g, on, pad.l, w - pad.r, h, pad, { calm: true }); g.restore();
      g.fillStyle = TEXT; g.fillRect(split - 1, 0, 2, h);              // divider with a square grip
      g.fillStyle = RED; g.fillRect(split - 12, h / 2 - 12, 24, 24);
      g.strokeStyle = TEXT; g.lineWidth = 2; g.strokeRect(split - 12, h / 2 - 12, 24, 24);
    }
    range.addEventListener('input', draw);
    let drag = false;
    const setFrom = e => {
      const r = cmpCv.getBoundingClientRect(), x = (e.clientX - r.left) / r.width;
      const inner = (x * r.width - pad.l) / (r.width - pad.l - pad.r);
      range.value = Math.round(clamp(inner) * 100); draw();
    };
    cmpCv.addEventListener('pointerdown', e => { drag = true; cmpCv.setPointerCapture(e.pointerId); setFrom(e); });
    cmpCv.addEventListener('pointermove', e => { if (drag) setFrom(e); });
    cmpCv.addEventListener('pointerup', () => { drag = false; });
    cmpCv.addEventListener('pointercancel', () => { drag = false; });
    new ResizeObserver(draw).observe(cmpCv);
    if (document.fonts) document.fonts.ready.then(draw);
  }

  // ---------- reveals and the active module ----------
  if ('IntersectionObserver' in window) {
    const rev = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { e.target.classList.add('in'); rev.unobserve(e.target); } }), { threshold: 0.12 });
    document.querySelectorAll('.reveal').forEach(el => rev.observe(el));
    // the active module is the newest one in the upper half of the screen (sticky ones stay there while they are stacked)
    const mods = [...document.querySelectorAll('.mod')], inTop = new Set();
    const act = new IntersectionObserver(es => {
      es.forEach(e => e.isIntersecting ? inTop.add(e.target) : inTop.delete(e.target));
      const cur = mods.filter(m => inTop.has(m)).pop();
      if (cur) mods.forEach(m => m.classList.toggle('active', m === cur));
    }, { rootMargin: '0px 0px -50% 0px' });
    mods.forEach(m => act.observe(m));
  } else document.querySelectorAll('.reveal').forEach(el => el.classList.add('in'));
})();
