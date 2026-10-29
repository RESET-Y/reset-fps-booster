/* RESET website motion. One frame clock drives everything: smooth scroll, the hero light trails,
   the 60 to 120 scene, the marquee and the small things. Every number it shows is measured in the
   visitor's own browser. With reduced motion it stays still and shows the end states. No libraries. */
(() => {
  "use strict";
  const doc = document.documentElement;
  const reduce = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const fine = matchMedia("(hover: hover) and (pointer: fine)").matches;
  const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
  const lerp = (a, b, k) => a + (b - a) * k;
  const ease = k => 1 - Math.pow(1 - k, 3);
  const $ = id => document.getElementById(id);
  const lang = () => (doc.lang === "de" ? "de" : "en");
  const TX = {
    en: { measuring: "Measuring your display", boost: "Click the track: boost", display: "Your display", page: "This page", frame: "Frame", live: "Live", ready: "Ready", skip: "Skip", hz: "Hz" },
    de: { measuring: "Miss dein Display", boost: "Klick auf die Strecke: Boost", display: "Dein Display", page: "Diese Seite", frame: "Frame", live: "Live", ready: "Bereit", skip: "Überspringen", hz: "Hz" }
  };
  const t = k => TX[lang()][k];

  // ---------------------------------------------------------------- frame clock and real frame times
  const tasks = new Set();
  const frames = [];                 // the last frame intervals in ms, as the browser delivered them
  let last = performance.now();
  let displayHz = 0;
  function loop(now) {
    const raw = now - last;
    last = now;
    if (raw > 0 && raw < 250 && !document.hidden) { frames.push(raw); if (frames.length > 240) frames.shift(); }
    const dt = Math.min(0.05, raw / 1000);
    for (const f of tasks) f(dt, now);
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
  const RATES = [60, 75, 90, 100, 120, 144, 165, 170, 180, 200, 240, 280, 300, 360, 500];
  function measuredFps(n) {
    const s = frames.slice(-n);
    if (s.length < 8) return 0;
    const sorted = [...s].sort((a, b) => a - b);
    return 1000 / sorted[sorted.length >> 1];
  }
  function snapHz(f) {
    let best = Math.round(f);
    for (const r of RATES) if (Math.abs(f - r) / r < 0.035) best = r;
    return best;
  }
  // the display's rate, or 0 when the frames were too slow or too uneven to say (a busy or throttled page)
  function credibleHz(list) {
    if (list.length < 40) return 0;
    const s = [...list.slice(-90)].sort((a, b) => a - b);
    const med = s[s.length >> 1], p10 = s[Math.floor(s.length * 0.1)], p90 = s[Math.floor(s.length * 0.9)];
    const f = 1000 / med;
    if (f < 48 || p90 > med * 1.35 || p10 < med * 0.7) return 0;
    return snapHz(f);
  }

  // ---------------------------------------------------------------- scroll: one smooth source of truth
  let sTarget = scrollY, sCurrent = scrollY, sActive = false, sAnchor = null;
  const smooth = !reduce && fine;
  const maxScroll = () => doc.scrollHeight - innerHeight;
  if (smooth) doc.style.scrollBehavior = "auto";
  function canScrollInside(el, dy) {
    for (; el && el !== document.body && el !== doc; el = el.parentElement) {
      if (el.scrollHeight <= el.clientHeight + 1) continue;
      const oy = getComputedStyle(el).overflowY;
      if (oy !== "auto" && oy !== "scroll") continue;
      if (dy < 0 ? el.scrollTop > 0 : el.scrollTop + el.clientHeight < el.scrollHeight - 1) return true;
    }
    return false;
  }
  function locked() {
    return document.body.style.overflow === "hidden" || doc.classList.contains("intro-pending") || !!$("intro") || ($("hdr") && $("hdr").classList.contains("open"));
  }
  if (smooth) {
    addEventListener("wheel", e => {
      if (e.ctrlKey || e.defaultPrevented) return;
      let dy = e.deltaY;
      if (e.deltaMode === 1) dy *= 40; else if (e.deltaMode === 2) dy *= innerHeight;
      if (Math.abs(e.deltaX) > Math.abs(dy)) return;
      if (canScrollInside(e.target, dy)) return;
      e.preventDefault();
      if (locked()) return;
      if (!sActive) sCurrent = sTarget = scrollY;
      sAnchor = null;
      sTarget = clamp(sTarget + dy, 0, maxScroll());
      sActive = true;
    }, { passive: false });
    const stop = () => { sActive = false; sAnchor = null; sCurrent = sTarget = scrollY; };
    addEventListener("pointerdown", e => { if (e.target === doc) stop(); }, { passive: true });
    addEventListener("keydown", e => { if (/^(Page|Arrow|Home|End| )/.test(e.key) || e.key === " ") stop(); });
    addEventListener("scroll", () => { if (!sActive) sCurrent = sTarget = scrollY; }, { passive: true });
    tasks.add(dt => {
      if (!sActive) return;
      sCurrent = lerp(sCurrent, sTarget, 1 - Math.exp(-dt * 9.5));
      if (Math.abs(sTarget - sCurrent) < 0.5) {
        sCurrent = sTarget; sActive = false;
        // content that loaded during the glide moved the target: settle on it once more
        if (sAnchor) { const y = anchorY(sAnchor); sAnchor = null; if (Math.abs(y - sCurrent) > 2) { sTarget = y; sActive = true; } }
      }
      scrollTo(0, sCurrent);
    });
  }
  function anchorY(el) {
    const pad = parseFloat(getComputedStyle(doc).scrollPaddingTop) || 0;
    return clamp(el.getBoundingClientRect().top + scrollY - pad, 0, maxScroll());
  }
  function scrollToY(y) {
    y = clamp(y, 0, maxScroll());
    if (smooth) { if (!sActive) sCurrent = scrollY; sTarget = y; sActive = true; }
    else scrollTo({ top: y, behavior: reduce ? "auto" : "smooth" });
  }
  window.RFBScroll = { to: scrollToY };
  // in-page links glide instead of jumping
  document.addEventListener("click", e => {
    const a = e.target.closest && e.target.closest('a[href^="#"]');
    if (!a || e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey) return;
    const id = a.getAttribute("href").slice(1);
    const el = id === "top" ? null : id && document.getElementById(id);
    if (id !== "top" && !el) return;
    e.preventDefault();
    // closing the menu happens in the page script; wait one frame so the lock is gone
    requestAnimationFrame(() => { scrollToY(el ? anchorY(el) : 0); sAnchor = smooth ? el : null; });
    if (history.pushState) history.pushState(null, "", "#" + id);
  });

  // scroll velocity, in px per second, smoothed: the marquee and the trails both feel it
  let vel = 0, prevY = scrollY;
  tasks.add(dt => {
    const y = scrollY;
    const v = dt > 0 ? (y - prevY) / dt : 0;
    prevY = y;
    vel = lerp(vel, clamp(v, -6000, 6000), 1 - Math.exp(-dt * 6));
  });

  // ---------------------------------------------------------------- header: glass once scrolled, out of the way going down
  const hdr = $("hdr");
  if (hdr) {
    let lastY = scrollY;
    tasks.add(() => {
      const y = scrollY;
      hdr.classList.toggle("scrolled", y > 24);
      if (!reduce) {
        if (y > 260 && y > lastY + 3) hdr.classList.add("hide");
        else if (y < lastY - 3 || y < 260) hdr.classList.remove("hide");
      }
      lastY = y;
    });
    hdr.querySelectorAll(".mnav a").forEach((a, i) => a.style.setProperty("--i", i));
  }

  // ---------------------------------------------------------------- intro: an honest measurement, then the plate
  function runIntro(done) {
    let seen = false;
    try { seen = sessionStorage.getItem("rfb-intro") === "1"; sessionStorage.setItem("rfb-intro", "1"); } catch { seen = true; }
    if (seen || reduce) { doc.classList.remove("intro-pending"); done(); return; }
    const el = document.createElement("div");
    el.id = "intro";
    el.setAttribute("aria-hidden", "true");
    el.innerHTML = '<div class="stripe"></div><div class="intro-core"><div class="intro-lbl"></div><div class="intro-num"><b>0</b><small>FPS</small></div><canvas class="intro-graph"></canvas></div>'
      + '<div class="intro-plate"><span></span></div><div class="intro-skip"></div><div class="stripe bottom"></div>';
    document.body.appendChild(el);
    doc.classList.remove("intro-pending");
    document.body.style.overflow = "hidden";
    const lbl = el.querySelector(".intro-lbl"), num = el.querySelector(".intro-num b"), unit = el.querySelector(".intro-num small");
    const cv = el.querySelector(".intro-graph"), g = cv.getContext("2d");
    el.querySelector(".intro-plate span").textContent = t("ready");
    el.querySelector(".intro-skip").textContent = t("skip");
    lbl.textContent = t("measuring");
    const start = performance.now(), mine = [];
    let lastF = start, finished = false, slammed = false;
    const dpr = Math.min(2, devicePixelRatio || 1);
    function size() { cv.width = cv.clientWidth * dpr; cv.height = cv.clientHeight * dpr; }
    size();
    function tick(dtS, now) {
      const raw = now - lastF; lastF = now;
      if (raw > 0 && raw < 250) mine.push(raw);
      const k = (now - start) / 1000;
      el.style.setProperty("--glow", Math.min(1, k * 1.2).toFixed(3));
      if (!slammed) {
        const f = mine.length >= 8 ? measuredFps(Math.min(60, mine.length)) : 0;
        num.textContent = f ? f.toFixed(0) : "";
        // the frame-time bars: one per real frame, height is its interval
        g.clearRect(0, 0, cv.width, cv.height);
        const n = Math.min(mine.length, 90), w = cv.width / 90;
        for (let i = 0; i < n; i++) {
          const ms = mine[mine.length - n + i];
          const h = clamp(ms / 33.3, 0.04, 1) * cv.height;
          g.fillStyle = ms > 20 ? "#ff2d3a" : "rgba(255,255,255,.55)";
          g.fillRect((90 - n + i) * w, cv.height - h, Math.max(1, w - 2 * dpr), h);
        }
      }
      if (!slammed && k > 1.15 && mine.length > 30) {
        slammed = true;
        displayHz = credibleHz(mine);
        if (displayHz) {
          num.textContent = displayHz;
          unit.textContent = t("hz");
          lbl.textContent = t("display");
          const tag = document.createElement("small");
          tag.textContent = t("display") + " · " + displayHz + " Hz";
          el.querySelector(".intro-plate span").appendChild(tag);
        }
        el.classList.add("slam");
        setTimeout(finish, 900);
      }
    }
    tasks.add(tick);
    function finish() {
      if (finished) return;
      finished = true;
      tasks.delete(tick);
      el.classList.add("out");
      document.body.style.overflow = "";
      done();
      setTimeout(() => el.remove(), 950);
    }
    el.addEventListener("click", finish);
    addEventListener("keydown", finish, { once: true });
    setTimeout(finish, 3400);
  }

  // ---------------------------------------------------------------- the track: light trails on a night circuit (WebGL)
  // One shader for the hero and the 60/120 comparison. Left of uSplit it runs on its own clock (the comparison
  // feeds it a 60-step clock there), the pointer lights the floor, a click boosts, fast scrolling warps.
  const FRAG = `
precision highp float;
uniform vec2 uRes;
uniform float uTravel;
uniform float uTravelL;
uniform float uSplit;
uniform float uTime;
uniform vec2 uMouse;
uniform vec2 uMouseL;
uniform float uBoost;
uniform float uVpX;
uniform vec3 uLight;
uniform float uWarp;
uniform float uBurst;
float h1(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float h2(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float n1(float x) { float i = floor(x); float f = fract(x); return mix(h1(i), h1(i + 1.0), f * f * (3.0 - 2.0 * f)); }
float ridge(float x) {
  float v = 0.0; float a = 0.55; float fr = 1.0;
  for (int o = 0; o < 4; o++) { v += a * (1.0 - abs(n1(x * fr) * 2.0 - 1.0)); fr *= 2.07; a *= 0.5; }
  return v;
}
void main() {
  vec2 fc = gl_FragCoord.xy;
  float H = uRes.y;
  float trv = fc.x < uSplit ? uTravelL : uTravel;
  vec2 mo = fc.x < uSplit ? uMouseL : uMouse;
  vec2 vp = vec2(uRes.x * (uVpX + mo.x * 0.035), H * (0.40 + mo.y * 0.025));
  vec2 p = (fc - vp) / H;
  vec3 col = vec3(0.0);
  float ay = abs(p.y);
  // sky: an ember band that rises off the horizon, and a hot line on it
  col += vec3(0.20, 0.012, 0.02) * exp(-ay * 2.4) * 0.75;
  col += vec3(0.95, 0.06, 0.09) * exp(-ay * 10.0) * (0.38 * (0.6 + 0.4 * uBoost) + 0.5 * uBurst);
  col += vec3(1.0, 0.45, 0.42) * exp(-ay * H * 0.22) * (0.32 * uBoost + 0.6 * uBurst);
  col *= 1.0 - smoothstep(0.0, 1.4, abs(p.x) * 0.55) * 0.45;
  // warp: streaks fly out of the vanishing point when you scroll hard or boost
  if (uWarp > 0.01) {
    float a = atan(p.y, p.x);
    float r = length(p);
    float k = a * 70.0;
    float id = floor(k);
    float hs = h1(id + 3.0);
    float seg = fract(r * (1.2 + hs * 1.6) - uTravel * (0.5 + hs) * 0.9 + hs * 9.0);
    float body = smoothstep(0.0, 0.05, seg) * (1.0 - smoothstep(0.05, 0.45, seg));
    float dpx = abs(fract(k) - 0.5) / 70.0 * r * H;
    float line = 1.0 - smoothstep(0.4, 1.4, dpx);
    float st = step(0.8, hs) * body * line * smoothstep(0.04, 0.5, r);
    col += mix(vec3(1.0, 0.12, 0.16), vec3(1.0, 0.9, 0.88), step(0.94, hs)) * st * uWarp * 0.9;
  }
  if (p.y < 0.0) {
    float dy = -p.y;
    float f = 1.35;
    float z = f / dy;
    float X = p.x / dy;
    float pxX = 1.0 / (dy * H);
    float pxZ = f / (dy * dy * H);
    float fog = exp(-z * 0.05);
    float zz = z + trv;
    // the pointer is a lamp held over the track
    vec2 ld = (fc - uLight.xy) / H;
    float spot = exp(-dot(ld, ld) * 16.0) * uLight.z;
    // the track: a grid that rolls toward you, fading into the dark
    float sx = 1.25;
    float gx = abs(fract(X / sx + 0.5) - 0.5) * sx;
    float lx = 1.0 - smoothstep(0.0, pxX * 1.5, gx);
    float sz = 2.6;
    float gz = abs(fract(zz / sz + 0.5) - 0.5) * sz;
    float lz = 1.0 - smoothstep(0.0, pxZ * 1.5, gz);
    float road = 1.0 - smoothstep(5.0, 13.0, abs(X));
    col += vec3(0.9, 0.05, 0.08) * (lx * 0.42 + lz * 0.3) * fog * road * (0.6 + spot * 2.6);
    col += vec3(0.9, 0.08, 0.1) * spot * 0.22;
    // the floor catches the horizon's glow
    col += vec3(0.55, 0.02, 0.04) * exp(-dy * 7.0) * (0.45 + uBurst * 0.6);
    // light trails: lanes of streaks rushing at you, red tails, white heads
    float lw = 0.42;
    float li = floor(X / lw);
    float lxl = X - (li + 0.5) * lw;
    float seed = h1(li + 17.0);
    float on = step(0.26, seed) * (1.0 - smoothstep(4.5, 9.0, abs(X)));
    float v = 3.0 + seed * 9.0;
    float period = 12.0 + h1(li * 3.1) * 22.0;
    float ph = fract((zz + trv * v) / period + seed * 5.3);
    float len = 0.22 + h1(li * 7.7) * 0.3 + uBurst * 0.25;
    float along = ph < len ? pow(1.0 - ph / len, 2.2) * smoothstep(0.0, 0.012 + pxZ / period * 2.0, ph) : 0.0;
    float tw = 0.026;
    float w = max(tw, pxX * 0.9);
    float across = exp(-(lxl * lxl) / (w * w)) * (tw / w);
    float halo = exp(-(lxl * lxl) / (w * w * 40.0)) * 0.09;
    float near = smoothstep(0.6, 2.4, z);
    float tr = (across + halo) * along * on * fog * near * (0.35 + 0.65 * uBoost + uBurst * 0.8) * (1.0 + spot * 1.5);
    float white = step(0.78, seed);
    vec3 tc = mix(vec3(1.0, 0.08, 0.12), vec3(1.0, 0.92, 0.9), white * 0.85);
    tc = mix(tc, vec3(1.0, 0.85, 0.82), smoothstep(0.85, 1.0, along) * 0.6);
    col += tc * tr * 2.4;
  }
  // wireframe hills either side of the track: five ridges that roll toward you, the nearest in front
  float off = trv * 0.045;
  float fo = fract(off);
  float io = floor(off);
  for (int i = 0; i < 5; i++) {
    float fk = (float(i) + fo) / 5.0;
    float z = 40.0 * pow(6.5 / 40.0, fk);
    float fade = smoothstep(0.0, 0.3, fk) * (1.0 - smoothstep(0.7, 1.0, fk));
    float s = 1.0 / z;
    float wx = p.x * z;
    float id = float(i) - io;
    float side = smoothstep(5.5, 10.0, abs(wx));
    float hgt = ridge(wx * 0.2 + id * 7.31) * 2.9 * side;
    float base = -1.35 * s;
    float top = base + hgt * s;
    if (p.y > base && p.y < top && hgt > 0.05) {
      float px = z / H;
      // what lies behind is covered by the hill
      col = mix(col, col * 0.18 + vec3(0.05, 0.004, 0.008), fade);
      float gx = abs(fract(wx * 0.8) - 0.5) * 2.0 / 0.8;
      // lines that follow the hill: offsets of its own crest, and verticals, like a mesh laid over it
      float below = (top - p.y) * z;
      float gy = abs(fract(below * 1.6) - 0.5) * 2.0 / 1.6;
      float lx = 1.0 - smoothstep(0.0, px * 2.4, gx);
      float ly = 1.0 - smoothstep(0.0, px * 2.4, gy);
      float crest = 1.0 - smoothstep(0.0, px * 2.2, below);
      float glow = 0.35 + 0.65 * exp(-z * 0.03);
      float depthFade = exp(-below * 0.55);
      vec3 wire = vec3(0.95, 0.07, 0.11) * (lx * 0.75 + ly * 0.9) * (0.35 + 0.65 * depthFade) + vec3(1.0, 0.6, 0.6) * crest * 1.4;
      col += wire * fade * glow * (0.7 + 0.55 * uBoost) * (1.0 + uBurst * 0.8);
    }
  }
  // tone, vignette and a little dither against banding
  col = 1.0 - exp(-col * 1.5);
  vec2 q = fc / uRes;
  col *= 0.35 + 0.65 * pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.22);
  col += (h2(fc + uTime) - 0.5) / 255.0;
  gl_FragColor = vec4(col, 1.0);
}`;
  // a canvas with the track program on it; returns null without WebGL
  function makeTrack(cv) {
    let gl = null;
    try { gl = cv.getContext("webgl", { antialias: false, alpha: false, depth: false, stencil: false, powerPreference: "high-performance" }); } catch { gl = null; }
    if (!gl) return null;
    const sh = (type, src) => { const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); return gl.getShaderParameter(s, gl.COMPILE_STATUS) ? s : null; };
    const vs = sh(gl.VERTEX_SHADER, "attribute vec2 a;void main(){gl_Position=vec4(a,0.,1.);}");
    const fs = sh(gl.FRAGMENT_SHADER, FRAG);
    if (!vs || !fs) return null;
    const pr = gl.createProgram();
    gl.attachShader(pr, vs); gl.attachShader(pr, fs); gl.linkProgram(pr);
    if (!gl.getProgramParameter(pr, gl.LINK_STATUS)) return null;
    gl.useProgram(pr);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(pr, "a");
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    const U = {};
    ["uRes", "uTravel", "uTravelL", "uSplit", "uTime", "uMouse", "uMouseL", "uBoost", "uVpX", "uLight", "uWarp", "uBurst"].forEach(n => { U[n] = gl.getUniformLocation(pr, n); });
    const track = { scale: Math.min(1.25, devicePixelRatio || 1) * 0.8, slow: 0 };
    track.resize = () => {
      const w = Math.max(1, Math.round(cv.clientWidth * track.scale)), h = Math.max(1, Math.round(cv.clientHeight * track.scale));
      if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; gl.viewport(0, 0, w, h); }
    };
    // s: { travel, travelL, split (0..1 of width, -1 for none), time, mx, my, boost, vpx, lx, ly, li (light in 0..1 of the canvas), warp, burst }
    track.draw = (s, dt) => {
      track.resize();
      const W = cv.width, Hh = cv.height;
      gl.uniform2f(U.uRes, W, Hh);
      gl.uniform1f(U.uTravel, s.travel);
      gl.uniform1f(U.uTravelL, s.travelL == null ? s.travel : s.travelL);
      gl.uniform1f(U.uSplit, s.split == null || s.split < 0 ? -1 : s.split * W);
      gl.uniform1f(U.uTime, (s.time || 0) % 100);
      gl.uniform2f(U.uMouse, s.mx || 0, s.my || 0);
      gl.uniform2f(U.uMouseL, s.mxL == null ? (s.mx || 0) : s.mxL, s.my || 0);
      gl.uniform1f(U.uBoost, s.boost == null ? 1 : s.boost);
      gl.uniform1f(U.uVpX, s.vpx == null ? 0.5 : s.vpx);
      gl.uniform3f(U.uLight, (s.lx || 0) * W, (1 - (s.ly || 0)) * Hh, s.li || 0);
      gl.uniform1f(U.uWarp, s.warp || 0);
      gl.uniform1f(U.uBurst, s.burst || 0);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      // a slow machine gets a lighter picture instead of a choppy one
      if (dt != null) {
        if (dt > 0.028) track.slow++; else track.slow = Math.max(0, track.slow - 1);
        if (track.slow > 40 && track.scale > 0.45) { track.scale *= 0.8; track.slow = 0; }
      }
    };
    addEventListener("resize", track.resize);
    return track;
  }

  function heroGL() {
    const cv = $("heroGl"), hero = $("hero");
    if (!cv || !hero) return null;
    const track = makeTrack(cv);
    if (!track) { cv.classList.add("nogl"); return null; }
    let visible = true;
    new IntersectionObserver(es => { visible = es[0].isIntersecting; }).observe(hero);
    const s = { travel: 0, time: 0, boost: 0, vpx: 0.585, mx: 0, my: 0, lx: 0.5, ly: 0.8, li: 0, warp: 0, burst: 0, split: -1 };
    let boostTarget = 0, tmx = 0, tmy = 0, tlx = 0.5, tly = 0.8, tli = 0;
    if (fine) {
      hero.addEventListener("pointermove", e => {
        const r = hero.getBoundingClientRect();
        tmx = (e.clientX - r.left) / r.width * 2 - 1;
        tmy = -((e.clientY - r.top) / r.height * 2 - 1);
        tlx = (e.clientX - r.left) / r.width;
        tly = (e.clientY - r.top) / r.height;
        tli = 1;
      }, { passive: true });
      hero.addEventListener("pointerleave", () => { tli = 0; });
      // a click on the track (not on a button or the readout) floors it
      hero.addEventListener("pointerdown", e => {
        if (e.button !== 0 || e.target.closest("a, button, .hud, input")) return;
        s.burst = 1;
      });
    }
    function draw(dt) {
      if (!visible || document.hidden) return;
      const k = 1 - Math.exp(-dt * 3);
      s.boost = lerp(s.boost, boostTarget, 1 - Math.exp(-dt * 1.6));
      s.mx = lerp(s.mx, tmx, k); s.my = lerp(s.my, tmy, k);
      const kl = 1 - Math.exp(-dt * 8);
      s.lx = lerp(s.lx, tlx, kl); s.ly = lerp(s.ly, tly, kl); s.li = lerp(s.li, tli, 1 - Math.exp(-dt * 4));
      s.burst = Math.max(0, s.burst - dt * 0.8);
      const scrollWarp = Math.min(1, Math.abs(vel) / 2600);
      s.warp = lerp(s.warp, Math.max(scrollWarp, s.burst), 1 - Math.exp(-dt * 6));
      s.travel += dt * (0.5 + 2.6 * s.boost) * (1 + Math.min(2.5, Math.abs(vel) / 1200) + s.burst * 3.5);
      s.time += dt;
      track.draw(s, dt);
    }
    if (reduce) { s.boost = 1; s.travel = 3; requestAnimationFrame(() => track.draw(s)); }
    else tasks.add(draw);
    return { launch() { boostTarget = 1; } };
  }

  // ---------------------------------------------------------------- 60 or 120: drag the line, slow it down
  // The left of the line is shown 60 times a second, the right 120 times. Our own track, rendered live, until
  // a real FrameBoost recording is set in data-clip; then the same split runs on that video, the left side
  // keeping every second frame only.
  function compare() {
    const stage = $("cmpStage");
    if (!stage) return;
    const cv = $("cmpGl"), range = stage.querySelector(".cmp-range"), hint = $("cmpHint"), src = $("cmpSrc");
    const rates = [...document.querySelectorAll("#cmpRate [data-rate]")];
    const clip = stage.dataset.clip;
    let split = 0.5, rate = 1, visible = false, userRate = false;
    new IntersectionObserver(es => { visible = es[0].isIntersecting; }).observe(stage);
    function setSplit(v) {
      split = clamp(v, 0.04, 0.96);
      stage.style.setProperty("--split", (split * 100).toFixed(2) + "%");
      range.value = Math.round(split * 100);
    }
    setSplit(0.5);
    range.addEventListener("input", () => setSplit(range.value / 100));
    let dragging = false;
    const fromEvent = e => { const r = stage.getBoundingClientRect(); setSplit((e.clientX - r.left) / r.width); };
    stage.addEventListener("pointerdown", e => { if (e.button !== 0) return; dragging = true; stage.setPointerCapture(e.pointerId); fromEvent(e); stage.classList.add("drag"); });
    stage.addEventListener("pointermove", e => { if (dragging) fromEvent(e); });
    const end = () => { dragging = false; stage.classList.remove("drag"); };
    stage.addEventListener("pointerup", end);
    stage.addEventListener("pointercancel", end);
    function setRate(r, byUser) {
      rate = r;
      if (byUser) userRate = true;
      rates.forEach(b => b.setAttribute("aria-pressed", String(+b.dataset.rate === r)));
      if (video) video.playbackRate = r;
    }
    rates.forEach(b => b.addEventListener("click", () => setRate(+b.dataset.rate, true)));
    // the words under it follow the language and what was measured
    const TXH = {
      en: { fast: hz => `Your display runs at ${hz} Hz: both sides play at their real speed.`,
            slow: hz => `Your display shows at most ${hz} pictures a second, so in real time both sides look alike here. Slow motion shows the extra frames.`,
            none: "Slow motion shows the extra frames on any display.",
            demo: "Shown: our own track, rendered live in your browser. Left 60, right 120 pictures a second.",
            clip: "Shown: a real FrameBoost recording from Apex Legends at 120 FPS. Left, every second frame is left out." },
      de: { fast: hz => `Dein Display läuft mit ${hz} Hz: Beide Seiten laufen in echter Geschwindigkeit.`,
            slow: hz => `Dein Display zeigt höchstens ${hz} Bilder pro Sekunde, in Echtzeit sehen beide Seiten hier gleich aus. In Zeitlupe siehst du die zusätzlichen Frames.`,
            none: "In Zeitlupe siehst du die zusätzlichen Frames auf jedem Display.",
            demo: "Zu sehen: unsere eigene Strecke, live in deinem Browser gerendert. Links 60, rechts 120 Bilder pro Sekunde.",
            clip: "Zu sehen: eine echte FrameBoost-Aufnahme aus Apex Legends mit 120 FPS. Links fehlt jedes zweite Bild." }
    };
    let shownHz = -1;
    function words() {
      const x = TXH[lang()];
      hint.textContent = displayHz ? (displayHz >= 110 ? x.fast(displayHz) : x.slow(displayHz)) : x.none;
      src.textContent = video ? x.clip : x.demo;
    }
    new MutationObserver(words).observe(doc, { attributes: true, attributeFilter: ["lang"] });
    tasks.add(() => {
      if (displayHz === shownHz) return;
      shownHz = displayHz;
      // on a 60 Hz screen, slow motion is the only way to see it: start there unless the visitor chose
      if (displayHz && displayHz < 110 && !userRate) setRate(0.25, false);
      words();
    });

    // ---- a real recording
    let video = null;
    if (clip) {
      video = document.createElement("video");
      Object.assign(video, { muted: true, loop: true, playsInline: true, preload: "auto", src: clip });
      video.setAttribute("muted", "");
      const c2 = document.createElement("canvas");
      c2.className = "cmp-video";
      cv.replaceWith(c2);
      const g = c2.getContext("2d"), hold = document.createElement("canvas"), hg = hold.getContext("2d");
      let fps = +(stage.dataset.clipFps || 120);
      function paint() {
        const vw = video.videoWidth, vh = video.videoHeight;
        if (!vw) return;
        if (c2.width !== vw) { c2.width = hold.width = vw; c2.height = hold.height = vh; }
        const sx = Math.round(vw * split);
        g.drawImage(hold, 0, 0, sx, vh, 0, 0, sx, vh);
        g.drawImage(video, sx, 0, vw - sx, vh, sx, 0, vw - sx, vh);
      }
      function onFrame(now, meta) {
        const idx = Math.round(meta.mediaTime * fps);
        if (idx % 2 === 0 || !hold.width) { if (!hold.width) { hold.width = video.videoWidth; hold.height = video.videoHeight; } hg.drawImage(video, 0, 0); }
        paint();
        video.requestVideoFrameCallback(onFrame);
      }
      if ("requestVideoFrameCallback" in HTMLVideoElement.prototype) video.requestVideoFrameCallback(onFrame);
      else { let lastIdx = -1; tasks.add(() => { const idx = Math.round(video.currentTime * fps); if (idx !== lastIdx) { lastIdx = idx; if (idx % 2 === 0 || !hold.width) { hold.width = video.videoWidth; hold.height = video.videoHeight; hg.drawImage(video, 0, 0); } paint(); } }); }
      range.addEventListener("input", paint);
      tasks.add(() => {
        if (visible && video.paused && !reduce) video.play().catch(() => {});
        else if (!visible && !video.paused) video.pause();
      });
      words();
      return;
    }

    // ---- our own track, live: two clocks, one stepping 60 times a second, one 120 times
    const track = makeTrack(cv);
    if (!track) { stage.classList.add("nogl"); words(); return; }
    let t = 0;
    const s = { travel: 0, travelL: 0, boost: 1, vpx: 0.5, li: 0, warp: 0, burst: 0 };
    function draw(dt) {
      if (!visible || document.hidden) return;
      t += dt * rate;
      // quick lateral sway as well as speed, so missing frames are easy to see
      s.travel = Math.floor(t * 120) / 120 * 3.2;
      s.travelL = Math.floor(t * 60) / 60 * 3.2;
      s.mx = Math.sin(Math.floor(t * 120) / 120 * 2.6) * 0.9;
      s.mxL = Math.sin(Math.floor(t * 60) / 60 * 2.6) * 0.9;
      s.split = split;
      s.time = t;
      track.draw(s, dt);
    }
    if (reduce) { s.travel = s.travelL = 2; requestAnimationFrame(() => track.draw(s)); }
    else tasks.add(draw);
    words();
  }

  // the live readout under the hero: the visitor's display and this page's own frame rate
  function heroHud() {
    const box = $("hudLive");
    if (!box) return;
    box.innerHTML = '<span class="live"><span class="dot red"></span><span data-k="live"></span></span>'
      + '<span><span data-k="display"></span> <b data-v="hz">–</b></span>'
      + '<span><span data-k="page"></span> <b data-v="fps">–</b></span>'
      + '<span><span data-k="frame"></span> <b data-v="ms">–</b></span><canvas width="264" height="52"></canvas>'
      + (fine && !reduce ? '<span class="boost-hint" data-k="boost"></span>' : "");
    const keys = box.querySelectorAll("[data-k]"), cv = box.querySelector("canvas"), g = cv.getContext("2d");
    const val = k => box.querySelector(`[data-v="${k}"]`);
    function labels() { keys.forEach(n => { n.textContent = t(n.dataset.k); }); }
    labels();
    new MutationObserver(labels).observe(doc, { attributes: true, attributeFilter: ["lang"] });
    let acc = 0;
    tasks.add(dt => {
      acc += dt;
      if (acc < 0.25) return;
      acc = 0;
      const f = measuredFps(90);
      if (!f) return;
      if (!displayHz && frames.length > 90) displayHz = credibleHz(frames);
      val("hz").textContent = displayHz ? displayHz + " HZ" : "–";
      val("fps").textContent = f.toFixed(0) + " FPS";
      val("ms").textContent = (1000 / f).toFixed(1) + " MS";
      const n = Math.min(frames.length, 66), w = cv.width / 66;
      g.clearRect(0, 0, cv.width, cv.height);
      for (let i = 0; i < n; i++) {
        const ms = frames[frames.length - n + i];
        const h = clamp(ms / 33.3, 0.06, 1) * cv.height;
        g.fillStyle = ms > 1000 / Math.max(30, (displayHz || 60)) * 1.6 ? "#ff2d3a" : "rgba(255,255,255,.5)";
        g.fillRect((66 - n + i) * w, cv.height - h, Math.max(1, w - 1), h);
      }
    });
  }

  // the readout panel leans toward the pointer
  function heroTilt() {
    const hero = $("hero"), hud = hero && hero.querySelector(".hud");
    if (!hud || !fine || reduce) return;
    let rx = 0, ry = 0, tx = 0, ty = 0;
    hero.addEventListener("pointermove", e => {
      const r = hero.getBoundingClientRect();
      ty = ((e.clientX - r.left) / r.width - 0.5) * 12;
      tx = -((e.clientY - r.top) / r.height - 0.5) * 9;
    }, { passive: true });
    hero.addEventListener("pointerleave", () => { tx = ty = 0; });
    tasks.add(dt => {
      const k = 1 - Math.exp(-dt * 5);
      rx = lerp(rx, tx, k); ry = lerp(ry, ty, k);
      hud.style.setProperty("--rx", rx.toFixed(2) + "deg");
      hud.style.setProperty("--ry", ry.toFixed(2) + "deg");
    });
  }

  // the hero copy drifts and fades as it scrolls away
  function heroDrift() {
    const hero = $("hero");
    if (!hero || reduce) return;
    const title = hero.querySelector(".hero-title"), gl = $("heroGl");
    tasks.add(() => {
      const y = scrollY, h = hero.offsetHeight;
      if (y > h * 1.2) return;
      const k = clamp(y / h, 0, 1);
      if (title) { title.style.transform = `translate3d(0, ${(y * 0.28).toFixed(1)}px, 0)`; title.style.opacity = (1 - k * 1.3).toFixed(3); }
      if (gl) gl.style.transform = `translate3d(0, ${(y * 0.45).toFixed(1)}px, 0) scale(${(1 + k * 0.08).toFixed(4)})`;
    });
  }

  // ---------------------------------------------------------------- headings, reveals, stagger
  function reveals() {
    // siblings in a set arrive one after the other
    document.querySelectorAll(".grid, .plans, .stats, .mods, .dl-grid, .rv-list, .modes, .hs-track").forEach(set => {
      [...set.children].filter(c => c.classList.contains("rv")).forEach((c, i) => c.style.setProperty("--i", i));
    });
    const heads = [...document.querySelectorAll("main h2.wipe")];
    if (reduce) { heads.forEach(h => h.classList.add("in")); return; }
    const io = new IntersectionObserver(es => es.forEach(e => {
      if (!e.isIntersecting) return;
      e.target.classList.add("in");
      io.unobserve(e.target);
    }), { threshold: 0.4, rootMargin: "0px 0px -8% 0px" });
    heads.forEach(h => io.observe(h));
    const wm = document.querySelector(".wordmark");
    if (wm) {
      [...wm.children].forEach((s, i) => s.style.setProperty("--i", i));
      new IntersectionObserver((es, o) => es.forEach(e => { if (e.isIntersecting) { wm.classList.add("in"); o.disconnect(); } }), { threshold: 0.25 }).observe(wm);
    }
  }

  // ---------------------------------------------------------------- 60 to 120, frame by frame, as you scroll
  function fpsScene() {
    const sec = $("fps");
    if (!sec) return;
    const num = $("fpsNum"), film = sec.querySelector(".fps-film"), steps = [...sec.querySelectorAll(".fps-steps li")];
    if (reduce) { num.textContent = "120"; sec.classList.add("hot"); steps.forEach(s => s.classList.add("on")); return; }
    sec.classList.add("fps-live");
    let shown = -1;
    tasks.add(() => {
      const r = sec.getBoundingClientRect();
      const span = sec.offsetHeight - innerHeight;
      if (r.bottom < -50 || r.top > innerHeight + 50) return;
      const p = clamp(-r.top / span, 0, 1);
      sec.style.setProperty("--p", p.toFixed(4));
      const g = ease(clamp((p - 0.3) / 0.42, 0, 1));
      film.style.setProperty("--g", g.toFixed(4));
      const n = Math.round(60 + 60 * g);
      if (n !== shown) { num.textContent = n; shown = n; }
      sec.classList.toggle("hot", g > 0.985);
      const step = p < 0.28 ? 0 : p < 0.74 ? 1 : 2;
      steps.forEach((s, i) => s.classList.toggle("on", i === step));
    });
  }

  // ---------------------------------------------------------------- marquee
  function marquee() {
    const tk = document.querySelector(".ticker"), row = tk && tk.querySelector(".ticker-row");
    if (!row || reduce) return;
    tk.classList.add("driven");
    let x = 0, dir = 1, on = true;
    new IntersectionObserver(es => { on = es[0].isIntersecting; }).observe(tk);
    tasks.add(dt => {
      if (!on) return;
      if (Math.abs(vel) > 30) dir = vel > 0 ? 1 : -1;
      const half = row.scrollWidth / 2;
      if (!half) return;
      x -= dir * (70 + Math.min(1600, Math.abs(vel)) * 0.45) * dt;
      if (x <= -half) x += half;
      if (x > 0) x -= half;
      row.style.transform = `translate3d(${x.toFixed(1)}px,0,0) skewX(${clamp(-vel * 0.0035, -9, 9).toFixed(2)}deg)`;
    });
  }

  // ---------------------------------------------------------------- pointer: magnetic buttons, lit surfaces, the glow
  function pointer() {
    if (!fine || reduce) return;
    document.querySelectorAll(".btn").forEach(b => {
      b.addEventListener("pointermove", e => {
        const r = b.getBoundingClientRect();
        b.style.setProperty("--mx", (((e.clientX - r.left) / r.width - 0.5) * 14).toFixed(1) + "px");
        b.style.setProperty("--my", (((e.clientY - r.top) / r.height - 0.5) * 10).toFixed(1) + "px");
      });
      b.addEventListener("pointerleave", () => { b.style.setProperty("--mx", "0px"); b.style.setProperty("--my", "0px"); });
    });
    const SURF = ".card,.mod,.strip-box,.rv-card,.pit,.term,.panel,.rv-form";
    document.addEventListener("pointermove", e => {
      const el = e.target.closest && e.target.closest(SURF);
      if (!el) return;
      const r = el.getBoundingClientRect();
      el.style.setProperty("--sx", (e.clientX - r.left).toFixed(0) + "px");
      el.style.setProperty("--sy", (e.clientY - r.top).toFixed(0) + "px");
    }, { passive: true });
    const glow = document.createElement("div");
    glow.className = "cursor-glow";
    glow.setAttribute("aria-hidden", "true");
    document.body.appendChild(glow);
    let gx = innerWidth / 2, gy = innerHeight / 2, tx = gx, ty = gy;
    addEventListener("pointermove", e => { tx = e.clientX; ty = e.clientY; glow.classList.add("on"); }, { passive: true });
    document.addEventListener("pointerleave", () => glow.classList.remove("on"));
    tasks.add(dt => {
      const k = 1 - Math.exp(-dt * 7);
      gx = lerp(gx, tx, k); gy = lerp(gy, ty, k);
      glow.style.transform = `translate3d(${gx.toFixed(1)}px,${gy.toFixed(1)}px,0)`;
    });
    // the Discord band's light follows along
    const band = document.querySelector(".band-glow");
    if (band) band.parentElement.addEventListener("pointermove", e => {
      const r = band.parentElement.getBoundingClientRect();
      band.style.setProperty("--bx", ((e.clientX - r.left) / r.width * 100).toFixed(1) + "%");
    }, { passive: true });
  }

  // ---------------------------------------------------------------- headings split red and white with the scroll speed
  function splitHeads() {
    if (reduce) return;
    const seen = new Set();
    const io = new IntersectionObserver(es => es.forEach(e => {
      if (e.isIntersecting) seen.add(e.target);
      else { seen.delete(e.target); e.target.style.textShadow = ""; }
    }));
    document.querySelectorAll(".hero h1, main h2:not(.sr-only), .fps-num, .wordmark").forEach(h => io.observe(h));
    let last = 0;
    tasks.add(() => {
      const v = clamp(vel / 420, -8, 8);
      const d = Math.abs(v) < 0.2 ? 0 : v;
      if (Math.abs(d - last) < 0.15) return;
      last = d;
      const ts = d ? `${d.toFixed(1)}px 0 0 rgba(255,45,58,.8), ${(-d * 0.7).toFixed(1)}px 0 0 rgba(255,255,255,.5)` : "";
      seen.forEach(h => { h.style.textShadow = ts; });
    });
  }

  // ---------------------------------------------------------------- section labels decode into place as they appear
  function decodeLabels() {
    if (reduce) return;
    const glyphs = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/#%";
    const io = new IntersectionObserver(es => es.forEach(e => {
      if (!e.isIntersecting) return;
      io.unobserve(e.target);
      const node = [...e.target.childNodes].find(n => n.nodeType === 3 && n.nodeValue.trim());
      if (!node) return;
      const final = node.nodeValue, startLang = doc.lang, t0 = performance.now(), dur = 650;
      (function f(now) {
        if (doc.lang !== startLang) return;          // the language switch wrote the new words: leave them
        const k = Math.min(1, (now - t0) / dur), shown = Math.floor(final.length * k);
        let out = final.slice(0, shown);
        for (let i = shown; i < final.length; i++) out += /\s/.test(final[i]) ? final[i] : glyphs[(Math.random() * glyphs.length) | 0];
        node.nodeValue = k < 1 ? out : final;
        if (k < 1) requestAnimationFrame(f);
      })(t0);
    }), { threshold: 1 });
    document.querySelectorAll("main .eyebrow").forEach(el => io.observe(el));
  }

  // ---------------------------------------------------------------- new in 1.5: the cards run sideways while you scroll
  function sideways() {
    const sec = $("new");
    if (!sec || !sec.classList.contains("hs") || reduce) return;
    const track = sec.querySelector(".hs-track"), bar = sec.querySelector(".hs-prog i");
    let live = false, dist = 0;
    function layout() {
      const want = innerWidth > 960;
      if (want !== live) {
        live = want;
        sec.classList.toggle("hs-live", live);
        if (!live) { sec.style.height = ""; track.style.transform = ""; }
      }
      if (!live) return;
      dist = Math.max(0, track.scrollWidth - innerWidth);
      sec.style.height = (innerHeight + dist) + "px";
    }
    layout();
    addEventListener("resize", layout);
    if (document.fonts) document.fonts.ready.then(layout);
    tasks.add(() => {
      if (!live) return;
      const r = sec.getBoundingClientRect();
      if (r.bottom < 0 || r.top > innerHeight) return;
      const p = clamp(-r.top / Math.max(1, sec.offsetHeight - innerHeight), 0, 1);
      track.style.transform = `translate3d(${(-p * dist).toFixed(1)}px,0,0)`;
      if (bar) bar.style.transform = `scaleX(${p.toFixed(4)})`;
    });
    // the big cards lean toward the pointer
    if (fine) sec.querySelectorAll(".card").forEach(c => {
      c.addEventListener("pointermove", e => {
        const r = c.getBoundingClientRect();
        c.style.setProperty("--cry", (((e.clientX - r.left) / r.width - 0.5) * 10).toFixed(2) + "deg");
        c.style.setProperty("--crx", (-((e.clientY - r.top) / r.height - 0.5) * 8).toFixed(2) + "deg");
      });
      c.addEventListener("pointerleave", () => { c.style.setProperty("--cry", "0deg"); c.style.setProperty("--crx", "0deg"); });
    });
  }

  // ---------------------------------------------------------------- the app, rendered from itself
  // Wide screens: the window stays while you scroll, tilts flat, then changes page with the livery plate.
  // Narrow screens and reduced motion: the list switches the page, and it moves on by itself while in view.
  function showcase() {
    const sec = $("app"), win = $("showWin"), list = $("showList");
    if (!sec || !win || !list) return;
    const imgs = [...win.querySelectorAll("img[data-shot]")], items = [...list.children];
    const n = imgs.length;
    function sources() {
      const l = lang();
      imgs.forEach(im => {
        const k = im.dataset.shot;
        im.srcset = `assets/app/${k}-${l}-1280.webp 1280w, assets/app/${k}-${l}.webp 2560w`;
        im.src = `assets/app/${k}-${l}-1280.webp`;
      });
    }
    sources();
    new MutationObserver(sources).observe(doc, { attributes: true, attributeFilter: ["lang"] });
    let step = 0;
    function show(i, plate) {
      if (i === step) return;
      step = i;
      imgs.forEach((im, j) => im.classList.toggle("on", j === i));
      items.forEach((li, j) => li.classList.toggle("on", j === i));
      if (plate && !reduce) { win.classList.remove("swap"); void win.offsetWidth; win.classList.add("swap"); }
    }
    let live = false, visible = false;
    new IntersectionObserver(es => { visible = es[0].isIntersecting; }).observe(sec);
    function layout() {
      const want = innerWidth > 960 && !reduce;
      if (want !== live) { live = want; sec.classList.toggle("show-live", live); if (!live) { sec.style.height = ""; win.style.removeProperty("--tx"); win.style.removeProperty("--ty"); win.style.removeProperty("--ts"); } }
      if (live) sec.style.height = Math.round(innerHeight * (1.25 + n * 0.62)) + "px";
    }
    layout();
    addEventListener("resize", layout);
    // the pointer turns the window a little
    let mx = 0, my = 0, cx = 0, cy = 0;
    if (fine) sec.addEventListener("pointermove", e => { mx = e.clientX / innerWidth - 0.5; my = e.clientY / innerHeight - 0.5; }, { passive: true });
    const span = () => Math.max(1, sec.offsetHeight - innerHeight);
    items.forEach((li, i) => li.addEventListener("click", () => {
      if (live) scrollToY(sec.getBoundingClientRect().top + scrollY + span() * (0.14 + (i + 0.5) / n * 0.86));
      else { show(i, true); last = performance.now(); }
    }));
    let last = performance.now();
    tasks.add((dt, now) => {
      if (!visible) return;
      if (!live) {
        // narrow screens: on by itself every few seconds
        if (!reduce && now - last > 4600) { last = now; show((step + 1) % n, true); }
        items[step].style.setProperty("--f", Math.min(1, (now - last) / 4600).toFixed(3));
        return;
      }
      const p = clamp(-sec.getBoundingClientRect().top / span(), 0, 1);
      const tilt = 1 - ease(clamp(p / 0.14, 0, 1));
      const k = 1 - Math.exp(-dt * 5);
      cx = lerp(cx, mx, k); cy = lerp(cy, my, k);
      win.style.setProperty("--tx", (14 * tilt - cy * 5).toFixed(2) + "deg");
      win.style.setProperty("--ty", (-22 * tilt + cx * 7).toFixed(2) + "deg");
      win.style.setProperty("--ts", (0.86 + 0.14 * (1 - tilt)).toFixed(4));
      const q = clamp((p - 0.14) / 0.86, 0, 0.9999) * n;
      show(Math.floor(q), true);
      items[step].style.setProperty("--f", (q - Math.floor(q)).toFixed(3));
    });
  }

  // ---------------------------------------------------------------- the LED wall
  // A dot-matrix board in the livery red. It runs a line of text, and when this visitor's display rate
  // was measured cleanly, that rate is part of the line. Dim dots twinkle the way a real board does.
  function ledWall() {
    const cv = $("ledWall");
    if (!cv) return;
    const g = cv.getContext("2d");
    const ROWS = 23;
    const TXL = {
      en: hz => [hz ? hz + " HZ MEASURED" : null, "NO PLACEBO", "RESET FPS BOOSTER", "60 IN  120 OUT", "NOTHING INJECTED"],
      de: hz => [hz ? hz + " HZ GEMESSEN" : null, "KEIN PLACEBO", "RESET FPS BOOSTER", "60 REIN  120 RAUS", "NICHTS INJIZIERT"]
    };
    let mask = null, maskW = 1, builtFor = "";
    function buildMask() {
      const words = TXL[lang()](displayHz).filter(Boolean).join("    /    ") + "    /    ";
      const off = document.createElement("canvas"), og = off.getContext("2d");
      og.font = "800 18px 'Saira Condensed', 'Arial Narrow', sans-serif"; og.letterSpacing = "2px";
      maskW = Math.ceil(og.measureText(words).width) + 2;
      off.width = maskW; off.height = ROWS;
      og.font = "800 18px 'Saira Condensed', 'Arial Narrow', sans-serif"; og.letterSpacing = "2px";
      og.fillStyle = "#fff"; og.textBaseline = "middle";
      og.fillText(words, 0, ROWS / 2 + 1);
      const d = og.getImageData(0, 0, maskW, ROWS).data;
      mask = new Uint8Array(maskW * ROWS);
      for (let i = 0; i < mask.length; i++) mask[i] = d[i * 4 + 3] > 96 ? 1 : 0;
      builtFor = lang() + "|" + displayHz;
    }
    // one glowing dot, drawn once, stamped where the board is lit
    const dot = document.createElement("canvas"), dg = dot.getContext("2d");
    let cell = 10, cols = 0, dpr = 1;
    function size() {
      dpr = Math.min(1.5, devicePixelRatio || 1);
      const w = cv.clientWidth, h = cv.clientHeight;
      cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
      cell = (h * dpr) / ROWS;
      cols = Math.ceil(cv.width / cell) + 1;
      const r = Math.ceil(cell * 0.95);
      dot.width = dot.height = r * 2;
      const grd = dg.createRadialGradient(r, r, 0, r, r, r);
      grd.addColorStop(0, "rgba(255,225,225,1)");
      grd.addColorStop(0.28, "rgba(255,60,72,1)");
      grd.addColorStop(0.46, "rgba(232,18,31,.6)");
      grd.addColorStop(0.62, "rgba(232,18,31,.12)");
      grd.addColorStop(1, "rgba(232,18,31,0)");
      dg.clearRect(0, 0, r * 2, r * 2);
      dg.fillStyle = grd; dg.fillRect(0, 0, r * 2, r * 2);
    }
    size();
    addEventListener("resize", size);
    let visible = false, x = 0, t = 0;
    new IntersectionObserver(es => { visible = es[0].isIntersecting; }).observe(cv);
    const hash = (a, b) => { const v = Math.sin(a * 127.1 + b * 311.7) * 43758.5453; return v - Math.floor(v); };
    function frame(dt) {
      if (!mask || builtFor !== lang() + "|" + displayHz) buildMask();
      t += dt;
      x += dt * (11 + Math.min(40, Math.abs(vel) / 60));
      g.clearRect(0, 0, cv.width, cv.height);
      const r = dot.width / 2, step = Math.floor(t * 6);
      for (let c = 0; c < cols; c++) {
        const mx = Math.floor(x + c) % maskW;
        for (let row = 0; row < ROWS; row++) {
          const cx = c * cell + cell / 2, cy = row * cell + cell / 2;
          if (mask[row * maskW + mx]) {
            g.globalAlpha = 0.85 + 0.15 * hash(c + step, row);
            g.drawImage(dot, cx - r, cy - r);
          } else {
            const tw = hash(c + Math.floor(x), row + step * 0.37);
            g.globalAlpha = tw > 0.985 ? 0.5 : 0.07 + tw * 0.05;
            g.drawImage(dot, cx - r * 0.45, cy - r * 0.45, r * 0.9, r * 0.9);
          }
        }
      }
      g.globalAlpha = 1;
    }
    if (reduce) {
      const go = () => { buildMask(); frame(0); };
      if (document.fonts) document.fonts.ready.then(go); else go();
    } else {
      if (document.fonts) document.fonts.ready.then(() => { mask = null; });
      tasks.add(dt => { if (visible && !document.hidden) frame(dt); });
    }
  }

  // ---------------------------------------------------------------- start
  const gl = heroGL();
  heroHud();
  heroTilt();
  heroDrift();
  reveals();
  showcase();
  ledWall();
  sideways();
  fpsScene();
  compare();
  splitHeads();
  decodeLabels();
  marquee();
  pointer();
  runIntro(() => {
    doc.classList.add("hero-go");
    const h1 = document.querySelector(".hero h1.wipe");
    if (h1) setTimeout(() => h1.classList.add("in"), reduce ? 0 : 120);
    if (gl) setTimeout(() => gl.launch(), reduce ? 0 : 150);
  });
})();
