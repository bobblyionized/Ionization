// Preview math: a procedural test scene plus JS ports of the shader's grading and
// the built-in LUT looks (kept in sync with src/lut.cpp and src/shaders.h).
(function () {
  const clamp01 = (x) => (x < 0 ? 0 : x > 1 ? 1 : x);
  const luma = (c) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
  const mix = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
  const sat = (c, s) => { const l = luma(c); return [l + (c[0] - l) * s, l + (c[1] - l) * s, l + (c[2] - l) * s]; };
  const sc = (x, k) => { const y = x - 0.5; return 0.5 + (y * (1 + k)) / (1 + k * Math.abs(2 * y)); };
  const curve = (c, k) => [sc(c[0], k), sc(c[1], k), sc(c[2], k)];
  const lift = (c, lo, hi) => c.map((v) => lo + v * (hi - lo));
  const add = (c, d, w) => [c[0] + d[0] * w, c[1] + d[1] * w, c[2] + d[2] * w];
  const mul = (c, m) => [c[0] * m[0], c[1] * m[1], c[2] * m[2]];

  const LOOKS = {
    "teal-orange": (c) => { const l = luma(c); c = add(c, [-0.06, 0.015, 0.07], (1 - l) * 0.9); c = add(c, [0.09, 0.025, -0.08], l * 0.9); return curve(sat(c, 1.12), 0.18); },
    "warm-film": (c) => lift(sat(mul(curve(c, 0.12), [1.06, 1, 0.88]), 0.9), 0.035, 0.97),
    "cold-steel": (c) => curve(mul(sat(c, 0.72), [0.92, 1, 1.1]), 0.25),
    "faded-matte": (c) => lift(curve(sat(c, 0.82), -0.08), 0.09, 0.93),
    "bleach-bypass": (c) => {
      const l = luma(c);
      const o = c.map((v) => (l < 0.5 ? 2 * v * l : 1 - 2 * (1 - v) * (1 - l)));
      return curve(sat(mix(c, o, 0.55), 0.55), 0.15);
    },
    "cross-process": (c) => {
      let o = [sc(c[0], 0.35), sc(c[1], 0.2), 0.12 + c[2] * 0.76];
      o = add(o, [0, 0.03, -0.02], 1 - luma(c));
      o = add(o, [0.04, 0, 0.05], luma(c));
      return sat(o, 1.08);
    },
    "amber-night": (c) => curve(sat(mul(c, [1, 0.84, 0.56]), 0.92), 0.05),
    mono: (c) => { const l = sc(luma(c), 0.22); return [l, l, l]; },
    vivid: (c) => curve(sat(c, 1.35), 0.14),
  };

  const toLin = (v) => (v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4));
  const toSrgb = (v) => { v = clamp01(v); return v <= 0.0031308 ? v * 12.92 : 1.055 * Math.pow(v, 1 / 2.4) - 0.055; };

  // A small synthetic "desktop + photo" scene: sky gradient, sun, hills, a UI card.
  const sceneCache = {};
  function scene(w, h) {
    const key = w + "x" + h;
    if (sceneCache[key]) return sceneCache[key];
    const c = document.createElement("canvas");
    c.width = w; c.height = h;
    const g = c.getContext("2d");
    const sky = g.createLinearGradient(0, 0, 0, h);
    sky.addColorStop(0, "#1d3a6b"); sky.addColorStop(0.45, "#e9895a"); sky.addColorStop(0.62, "#f3c27a");
    g.fillStyle = sky; g.fillRect(0, 0, w, h);
    const sun = g.createRadialGradient(w * 0.7, h * 0.5, 2, w * 0.7, h * 0.5, h * 0.42);
    sun.addColorStop(0, "#fff6d8"); sun.addColorStop(0.18, "#ffd98a"); sun.addColorStop(1, "rgba(255,180,90,0)");
    g.fillStyle = sun; g.fillRect(0, 0, w, h);
    const hill = (y, amp, col, ph) => {
      g.beginPath(); g.moveTo(0, h);
      for (let x = 0; x <= w; x += 4) g.lineTo(x, y + Math.sin(x / w * 6 + ph) * amp + Math.sin(x / w * 17 + ph * 2) * amp * 0.3);
      g.lineTo(w, h); g.closePath(); g.fillStyle = col; g.fill();
    };
    hill(h * 0.62, h * 0.05, "#5b3a5e", 0.4);
    hill(h * 0.72, h * 0.06, "#2c3f4a", 1.7);
    hill(h * 0.84, h * 0.05, "#16261c", 3.1);
    // UI card (like a window) to show how panels look.
    g.fillStyle = "rgba(13,14,17,0.92)";
    roundRect(g, w * 0.06, h * 0.1, w * 0.36, h * 0.34, 8); g.fill();
    g.fillStyle = "#d4f53c"; roundRect(g, w * 0.09, h * 0.16, w * 0.06, w * 0.06, 4); g.fill();
    g.fillStyle = "#f3f4f6"; g.fillRect(w * 0.17, h * 0.17, w * 0.2, 3);
    g.fillStyle = "#8c919a"; g.fillRect(w * 0.17, h * 0.22, w * 0.14, 2);
    g.fillStyle = "#3d8bfd"; roundRect(g, w * 0.09, h * 0.32, w * 0.12, h * 0.07, 4); g.fill();
    g.fillStyle = "#e5484d"; roundRect(g, w * 0.23, h * 0.32, w * 0.12, h * 0.07, 4); g.fill();
    const img = g.getImageData(0, 0, w, h);
    sceneCache[key] = img;
    return img;
  }
  function roundRect(g, x, y, w, h, r) {
    g.beginPath(); g.moveTo(x + r, y); g.arcTo(x + w, y, x + w, y + h, r); g.arcTo(x + w, y + h, x, y + h, r);
    g.arcTo(x, y + h, x, y, r); g.arcTo(x, y, x + w, y, r); g.closePath();
  }

  // Grade a scene into a canvas with the same math as PSGrade (minus bloom/sharpen/chroma).
  function render(canvas, s, opts) {
    opts = opts || {};
    const w = canvas.width, h = canvas.height;
    const src = scene(w, h);
    const out = canvas.getContext("2d").createImageData(w, h);
    const vis = s.visuals && s.visuals.enabled !== false;
    const C = s.color || {}, F = s.fx || {}, L = s.lut || {};
    const exp = vis ? Math.pow(2, C.exposure || 0) : 1;
    const con = vis ? (C.contrast || 0) / 100 : 0;
    const sa = vis ? (C.saturation ?? 100) / 100 : 1;
    const vib = vis ? (C.vibrance || 0) / 100 : 0;
    const tmp = vis ? (C.temperature || 0) / 100 : 0;
    const tnt = vis ? (C.tint || 0) / 100 : 0;
    const warm = vis ? (F.warmth || 0) / 100 : 0;
    const vig = vis ? (F.vignette || 0) / 100 : 0;
    const vsoft = (F.vignetteSoft ?? 50) / 100;
    const grain = vis ? (F.grain || 0) / 100 : 0;
    const look = vis && L.id && LOOKS[L.id] && (L.intensity ?? 100) > 0 ? LOOKS[L.id] : null;
    const lmix = (L.intensity ?? 100) / 100;
    const split = opts.split ? Math.floor(w * opts.split) : -1;
    const aspect = w / h, inner = 0.75 + (0.05 - 0.75) * vsoft;
    const lut = new Float32Array(256);
    for (let i = 0; i < 256; i++) lut[i] = toLin(i / 255);
    const d = src.data, o = out.data;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 4;
        if (x < split) { o[i] = d[i]; o[i + 1] = d[i + 1]; o[i + 2] = d[i + 2]; o[i + 3] = 255; continue; }
        let r = lut[d[i]] * exp, g = lut[d[i + 1]] * exp, b = lut[d[i + 2]] * exp;
        r *= 1 + tmp * 0.1; g *= 1 - tnt * 0.08; b *= 1 - tmp * 0.1;
        r *= 1; g *= 1 + (0.8 - 1) * warm; b *= 1 + (0.55 - 1) * warm;
        let c = [toSrgb(r), toSrgb(g), toSrgb(b)];
        c = c.map((v) => clamp01((v - 0.5) * (1 + con) + 0.5));
        let l = luma(c);
        c = [l + (c[0] - l) * sa, l + (c[1] - l) * sa, l + (c[2] - l) * sa];
        const mx = Math.max(c[0], c[1], c[2]), mn = Math.min(c[0], c[1], c[2]);
        const vv = 1 + vib * (1 - (mx - mn));
        c = [clamp01(l + (c[0] - l) * vv), clamp01(l + (c[1] - l) * vv), clamp01(l + (c[2] - l) * vv)];
        if (look) { const lc = look(c.slice()); c = mix(c, lc.map(clamp01), lmix); }
        if (vig > 0) {
          const dx = (x / w - 0.5) * aspect, dy = y / h - 0.5;
          const rr = Math.hypot(dx, dy) / Math.hypot(aspect * 0.5, 0.5);
          let t = clamp01((rr - inner) / (1.15 - inner)); t = t * t * (3 - 2 * t);
          const k = 1 - t * vig; c = c.map((v) => v * k);
        }
        if (grain > 0) { const n = Math.random() - 0.5; const k = n * grain * 0.16 * (1 - 0.6 * l); c = c.map((v) => v + k); }
        o[i] = clamp01(c[0]) * 255; o[i + 1] = clamp01(c[1]) * 255; o[i + 2] = clamp01(c[2]) * 255; o[i + 3] = 255;
      }
    }
    canvas.getContext("2d").putImageData(out, 0, 0);
    if (split > 0) {
      const g = canvas.getContext("2d");
      g.fillStyle = "rgba(243,244,246,0.9)"; g.fillRect(split, 0, 1, h);
    }
  }

  window.Looks = { LOOKS, render };
})();
