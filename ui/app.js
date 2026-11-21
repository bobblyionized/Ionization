"use strict";
// Ionization panel UI. Talks to the native host over WebView2 postMessage; falls
// back to a mock host when opened in a normal browser (for design work).

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const svgUse = (id, size = 14) => `<svg width="${size}" height="${size}"><use href="#${id}"/></svg>`;

const DEFAULTS = {
  blur: { enabled: true, mode: "trail", strength: 45 },
  visuals: { enabled: true },
  color: { exposure: 0, contrast: 0, saturation: 100, vibrance: 0, temperature: 0, tint: 0 },
  lut: { id: "neutral", intensity: 100 },
  fx: { vignette: 0, vignetteSoft: 50, grain: 0, bloom: 0, bloomThreshold: 70, sharpen: 0, chroma: 0, warmth: 0 },
  clock: { enabled: false, style: "volt", hour24: true, seconds: true, date: true, backdrop: true, size: 100, opacity: 100, layer: "top", corner: "tr", x: 0, y: 0 },
  display: { monitor: 0 },
  app: { autoUpdate: true, keepPanelSharp: false, hotkeys: true, trayHintShown: false },
  profiles: [],
  activeProfile: "Clean",
};

const BUILTIN_PROFILES = [
  { name: "Clean", desc: "Light trail, untouched color. The everyday default.",
    data: { blur: { enabled: true, mode: "trail", strength: 35 }, visuals: { enabled: true }, color: {}, lut: { id: "neutral", intensity: 100 }, fx: {} } },
  { name: "Cinematic", desc: "Teal and orange grade, soft vignette, fine grain.",
    data: { blur: { enabled: true, mode: "trail", strength: 55 }, visuals: { enabled: true }, color: { contrast: 10, saturation: 95 }, lut: { id: "teal-orange", intensity: 70 }, fx: { vignette: 35, vignetteSoft: 60, grain: 12 } } },
  { name: "Dreamy", desc: "Long smear, bloom on highlights, lifted blacks.",
    data: { blur: { enabled: true, mode: "trail", strength: 75 }, visuals: { enabled: true }, color: { exposure: 0.15, saturation: 110 }, lut: { id: "faded-matte", intensity: 40 }, fx: { bloom: 45, bloomThreshold: 55, vignette: 20, chroma: 15 } } },
  { name: "Retro", desc: "Frame-blend ghosting, cross-processed film, heavy grain.",
    data: { blur: { enabled: true, mode: "blend", strength: 50 }, visuals: { enabled: true }, color: { contrast: 8 }, lut: { id: "cross-process", intensity: 65 }, fx: { grain: 35, chroma: 30, vignette: 40 } } },
  { name: "Night", desc: "Warm amber screen for late hours, gentle trail.",
    data: { blur: { enabled: true, mode: "trail", strength: 40 }, visuals: { enabled: true }, color: { exposure: -0.2, saturation: 90 }, lut: { id: "amber-night", intensity: 50 }, fx: { warmth: 55 } } },
];

const state = {
  settings: structuredClone(DEFAULTS),
  monitors: [],
  luts: [],
  version: "0.0.0-dev",
  dev: true,
  stats: { running: false, fps: 0, captureFps: 0, gpuMs: 0, width: 0, height: 0, refresh: 60 },
  autostart: false,
  hotkeys: { blur: true, visuals: true, clock: true },
  update: null,
  clockEditing: false,
};

// ------------------------------------------------------------------ bridge
const host = window.chrome && window.chrome.webview;
function send(type, extra = {}) {
  if (host) host.postMessage({ type, ...extra });
  else Mock.handle({ type, ...extra });
}
let sendTimer = 0;
function pushSettings() {
  if (sendTimer) return;
  sendTimer = setTimeout(() => { sendTimer = 0; send("settings", { value: state.settings }); }, 16);
}
function onHost(msg) {
  switch (msg.type) {
    case "init":
      state.settings = merge(structuredClone(DEFAULTS), msg.settings || {});
      state.monitors = msg.monitors || [];
      state.luts = msg.luts || [];
      state.version = msg.version || state.version;
      state.dev = !!msg.dev;
      state.autostart = !!msg.autostart;
      state.hotkeys = msg.hotkeys || state.hotkeys;
      state.clockEditing = !!msg.clockEditing;
      if (msg.update) state.update = msg.update;
      renderAll();
      if (msg.view) showView(msg.view);
      reportPainted();
      if (state.update && state.update.status === "updated") toast(`Updated to v${state.update.latest}`, "ok");
      break;
    case "state":
      state.settings = merge(structuredClone(DEFAULTS), msg.settings || {});
      if ("clockEditing" in msg) state.clockEditing = !!msg.clockEditing;
      renderAll();
      break;
    case "stats": state.stats = msg; renderStats(); break;
    case "luts": state.luts = msg.luts || []; renderLuts(); break;
    case "monitors": state.monitors = msg.monitors || []; renderMonitors(); renderHeader(); break;
    case "update": state.update = msg.value; renderUpdate(); break;
    case "toast": toast(msg.text, msg.tone); break;
    case "clockEditing": state.clockEditing = !!msg.value; renderClock(); break;
    case "autostart": state.autostart = !!msg.value; $("#autostartToggle").checked = state.autostart; break;
    case "hotkeys": state.hotkeys = msg.value || state.hotkeys; renderShortcuts(); break;
  }
}
if (host) host.addEventListener("message", (e) => onHost(e.data));

function merge(base, over) {
  for (const k of Object.keys(over || {})) {
    const v = over[k];
    if (v && typeof v === "object" && !Array.isArray(v) && base[k] && typeof base[k] === "object" && !Array.isArray(base[k])) merge(base[k], v);
    else base[k] = v;
  }
  return base;
}
function get(path) { return path.split(".").reduce((o, k) => (o == null ? undefined : o[k]), state.settings); }
function set(path, value) {
  const keys = path.split(".");
  let o = state.settings;
  for (let i = 0; i < keys.length - 1; i++) o = o[keys[i]] ??= {};
  if (o[keys.at(-1)] === value) return;
  o[keys.at(-1)] = value;
  const section = keys[0];
  if (["blur", "color", "lut", "fx"].includes(section) && state.settings.activeProfile) {
    state.settings.activeProfile = "";
    renderProfiles();
  }
  pushSettings();
  refreshFor(section);
}

// ------------------------------------------------------------------ controls
const sliders = [];

function fmtNum(v, step) {
  const dec = step < 1 ? (String(step).split(".")[1] || "").length : 0;
  return Number(v).toFixed(dec);
}

function makeSlider(host, o) {
  const row = document.createElement("div");
  row.className = o.hero ? "" : "srow";
  const id = "s_" + o.path.replace(/\./g, "_");
  const signed = o.min < 0;
  row.innerHTML = o.hero
    ? `<input type="range" id="${id}" min="${o.min}" max="${o.max}" step="${o.step}" aria-label="${o.label}">`
    : `<div class="top"><span class="lab"><span class="name" title="Double-click to reset">${o.label}</span>${o.hint ? `<span class="sub">${o.hint}</span>` : ""}</span>
       <label class="numbox"><input inputmode="decimal" spellcheck="false" aria-label="${o.label} value"><span class="unit">${o.unit || ""}</span></label></div>
       <input type="range" id="${id}" min="${o.min}" max="${o.max}" step="${o.step}" aria-label="${o.label}"${signed ? ' class="center"' : ""}>`;
  let box;
  if (o.hero) {
    host.appendChild(row.firstElementChild);
    box = document.createElement("label");
    box.className = "numbox";
    box.innerHTML = `<input inputmode="decimal" spellcheck="false" aria-label="${o.label} value"><span class="unit">${o.unit || ""}</span>`;
    host.appendChild(box);
  } else {
    host.appendChild(row);
    box = $(".numbox", row);
  }
  const range = $("#" + id);
  const num = $("input", box);
  const def = o.def;
  const s = {
    o, range, num,
    update() {
      const v = get(o.path) ?? def;
      if (document.activeElement !== range) range.value = v;
      if (document.activeElement !== num) num.value = (signed && v > 0 ? "+" : "") + fmtNum(v, o.step);
      const p = ((v - o.min) / (o.max - o.min)) * 100;
      range.style.setProperty("--p", p + "%");
      range.classList.toggle("hot", !!o.hero || Math.abs(v - def) > 1e-9);
    },
  };
  range.addEventListener("input", () => { set(o.path, Number(fmtNum(Number(range.value), o.step))); s.update(); });
  const commit = () => {
    const raw = parseFloat(num.value.replace(/[^\d.+-]/g, ""));
    if (!isNaN(raw)) set(o.path, clamp(Number(fmtNum(Math.round(raw / o.step) * o.step, o.step)), o.min, o.max));
    s.update();
  };
  num.addEventListener("change", commit);
  num.addEventListener("blur", () => { commit(); });
  num.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { commit(); num.blur(); }
    else if (e.key === "Escape") { s.update(); num.blur(); }
    else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
      e.preventDefault();
      const cur = get(o.path) ?? def;
      const k = (e.shiftKey ? 10 : 1) * o.step * (e.key === "ArrowUp" ? 1 : -1);
      set(o.path, clamp(Number(fmtNum(cur + k, o.step)), o.min, o.max));
      s.update();
      num.select();
    }
  });
  num.addEventListener("focus", () => setTimeout(() => num.select(), 0));
  const name = $(".name", row);
  if (name) name.addEventListener("dblclick", () => { set(o.path, def); s.update(); });
  range.addEventListener("dblclick", () => { set(o.path, def); s.update(); });
  sliders.push(s);
  s.update();
  return s;
}

function placeIndicator(seg) {
  const ind = $(".ind", seg);
  const act = $("button.active", seg);
  if (!ind) return;
  if (!act) { ind.style.opacity = 0; return; }
  ind.style.opacity = 1;
  ind.style.left = act.offsetLeft + "px";
  ind.style.width = act.offsetWidth + "px";
}
function placeTabBar(tabs) {
  const bar = $(".bar", tabs), act = $("button.active", tabs);
  if (!bar || !act) return;
  bar.style.left = act.offsetLeft + 8 + "px";
  bar.style.width = act.offsetWidth - 16 + "px";
}
function setSeg(seg, attr, value) {
  $$("button", seg).forEach((b) => b.classList.toggle("active", b.dataset[attr] === String(value)));
  placeIndicator(seg);
}

// ------------------------------------------------------------------ blur view
const refreshRate = () => state.stats.refresh || (state.monitors[get("display.monitor")] || state.monitors[0] || {}).refresh || 60;
function trailTau(s) { return 0.004 + 0.32 * Math.pow(s / 100, 1.7); }
function blendN(s) { return 2 + Math.round((s / 100) * 10); }

function renderBlur() {
  const b = state.settings.blur;
  const on = b.enabled && b.strength > 0;
  const hz = refreshRate();
  $$("#modeTabs button").forEach((x) => x.classList.toggle("active", x.dataset.mode === b.mode));
  placeTabBar($("#modeTabs"));
  const presets = [18, 40, 65, 90];
  setSeg($("#presetSeg"), "preset", presets.includes(b.strength) ? b.strength : "none");

  $("#kStrength").textContent = Math.round(b.strength);
  const chip = $("#kStrengthChip");
  chip.className = "delta" + (on ? "" : " mute");
  chip.lastElementChild.textContent = on ? b.mode : "off";
  if (b.mode === "trail") {
    const tau = trailTau(b.strength) * 1000;
    $("#kShutterLabel").textContent = "Shutter";
    $("#kShutter").textContent = Math.round(tau);
    $("#kShutterUnit").textContent = "ms";
    $("#kShutterChip").textContent = "decay";
    $("#kShutterNote").textContent = `~${Math.round(tau * 3)} ms tail`;
  } else {
    const n = blendN(b.strength);
    $("#kShutterLabel").textContent = "Samples";
    $("#kShutter").textContent = n;
    $("#kShutterUnit").textContent = "frames";
    $("#kShutterChip").textContent = `${n}x`;
    $("#kShutterNote").textContent = `${Math.round((n * 1000) / hz)} ms window`;
  }
  $("#heroVal").textContent = Math.round(b.strength) + "%";
  const presetName = { 18: "Subtle", 40: "Smooth", 65: "Cinematic", 90: "Max" }[b.strength];
  $("#kPreset").textContent = presetName ? presetName.toLowerCase() + " preset" : "custom";
  const hc = $("#heroChip");
  hc.className = "delta" + (on ? "" : " mute");
  hc.innerHTML = on ? `${svgUse("i-pulse", 10)}<span>live</span>` : `<span>paused</span>`;
  const cmd = $("#cmdBlur");
  cmd.classList.toggle("on", !!b.enabled);
  $("#cmdLabel").textContent = b.enabled ? "Motion blur is on" : "Motion blur is off";
  Demo.kick();
  $("#heroSub").textContent = b.mode === "trail" ? `trail, ${Math.round(trailTau(b.strength) * 1000)} ms` : `blend, ${blendN(b.strength)} frames`;
  $("#compareLbl").textContent = b.mode === "trail" ? "Compare frame blend" : "Compare trail";
  $("#bTrail").textContent = Math.round(b.strength) + "%";
  $("#bTrailBar").style.width = b.strength + "%";
  drawChart();
}

function kernel(mode, s, hz, spanMs) {
  const dt = 1000 / hz;
  const pts = [];
  const n = Math.ceil(spanMs / dt);
  if (s <= 0) { for (let k = 0; k <= n; k++) pts.push([k * dt, k === 0 ? 1 : 0]); return pts; }
  if (mode === "trail") {
    const d = Math.exp(-(dt / 1000) / trailTau(s));
    for (let k = 0; k <= n; k++) pts.push([k * dt, (1 - d) * Math.pow(d, k)]);
  } else {
    const N = blendN(s);
    for (let k = 0; k <= n; k++) pts.push([k * dt, k < N ? 1 / N : 0]);
  }
  return pts;
}

function drawChart() {
  const el = $("#chart");
  if (!el || !el.clientWidth) return;
  const W = el.clientWidth, H = el.clientHeight;
  const padR = 52, padB = 22, top = 8;
  const w = W - padR, h = H - padB - top;
  const b = state.settings.blur;
  const hz = refreshRate();
  const span = 300;
  $("#chartSpan").textContent = span;
  const cur = kernel(b.mode, b.enabled ? b.strength : 0, hz, span);
  const other = kernel(b.mode === "trail" ? "blend" : "trail", b.strength, hz, span);
  const maxW = Math.max(0.0001, ...cur.map((p) => p[1]), ...($("#compareToggle").checked ? other.map((p) => p[1]) : []));
  const X = (ms) => w - (ms / span) * w;
  const Y = (v) => top + h - (v / maxW) * h * 0.92;
  const smooth = (pts, step) => {
    if (step) { // boxcar: draw as steps
      let d = `M${X(pts[0][0])},${Y(pts[0][1])}`;
      for (let i = 1; i < pts.length; i++) d += ` L${X(pts[i][0])},${Y(pts[i - 1][1])} L${X(pts[i][0])},${Y(pts[i][1])}`;
      return d;
    }
    let d = `M${X(pts[0][0])},${Y(pts[0][1])}`;
    for (let i = 1; i < pts.length; i++) {
      const [x0, y0] = [X(pts[i - 1][0]), Y(pts[i - 1][1])], [x1, y1] = [X(pts[i][0]), Y(pts[i][1])];
      const cx = (x0 + x1) / 2;
      d += ` C${cx},${y0} ${cx},${y1} ${x1},${y1}`;
    }
    return d;
  };
  const curD = smooth(cur, b.mode === "blend");
  const otherD = smooth(other, b.mode !== "blend");
  const area = `${curD} L${X(cur.at(-1)[0])},${top + h} L${X(0)},${top + h} Z`;
  const grid = [0.5, 1].map((f) => {
    const y = top + h - f * h * 0.92;
    return `<line class="gridline" x1="0" x2="${w}" y1="${y}" y2="${y}"/><text class="axis" x="${w + 10}" y="${y + 4}">${Math.round(maxW * f * 100)}%</text>`;
  }).join("");
  const xt = [300, 225, 150, 75, 0].map((ms) => `<text class="axis" x="${X(ms)}" y="${H - 4}" text-anchor="${ms === 300 ? "start" : ms === 0 ? "end" : "middle"}">${ms === 0 ? "now" : "-" + ms + "ms"}</text>`).join("");
  const nowY = Y(cur[0][1]);
  el.innerHTML = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">
    <defs><linearGradient id="wash" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="#d4f53c" stop-opacity="0.22"/><stop offset="1" stop-color="#d4f53c" stop-opacity="0"/></linearGradient></defs>
    ${grid}
    <line class="baseline" x1="0" x2="${w}" y1="${top + h}" y2="${top + h}"/>
    <path d="${area}" fill="url(#wash)"/>
    ${$("#compareToggle").checked ? `<path d="${otherD}" fill="none" stroke="#8c919a" stroke-width="1.25" stroke-dasharray="3 4"/>` : ""}
    <path d="${curD}" fill="none" stroke="#d4f53c" stroke-width="1.75" stroke-linejoin="round"/>
    <circle cx="${X(0)}" cy="${nowY}" r="7" fill="#d4f53c" opacity=".18"/>
    <circle cx="${X(0)}" cy="${nowY}" r="3.5" fill="#d4f53c" stroke="#0d0e11" stroke-width="1.5"/>
    ${xt}
  </svg>`;
}

function renderShortcuts() {
  const en = state.settings.app.hotkeys;
  const rows = [
    ["Motion blur", "B", "blur", "blur.enabled"],
    ["Color & effects", "V", "visuals", "visuals.enabled"],
    ["Desktop clock", "C", "clock", "clock.enabled"],
  ];
  $("#hkList").innerHTML = rows.map(([name, key, id, path]) => {
    const ok = state.hotkeys[id];
    const tag = !en ? `<span class="tag dim">disabled</span>` : ok ? `<span class="tag">${get(path) ? "on" : "ready"}</span>` : `<span class="tag warn">in use</span>`;
    return `<li><div><p class="t" style="margin:0">${name}</p><p class="s" style="margin:2px 0 0">global hotkey</p></div>
      <div class="r"><span style="display:flex;gap:3px"><kbd>Ctrl</kbd><kbd>Alt</kbd><kbd>${key}</kbd></span>${tag}</div></li>`;
  }).join("");
  $("#hkDot").className = "dot" + (en && Object.values(state.hotkeys).some(Boolean) ? " on" : "");
}

// ------------------------------------------------------------------ stats + header
function renderStats() {
  const s = state.stats;
  const hz = refreshRate();
  const running = !!s.running;
  $("#liveDot").className = "dot" + (running ? " on" : "");
  $("#liveText").textContent = running ? "Live" : "Idle";
  $("#liveSub").textContent = running ? `${Math.round(s.fps)} fps` : "effects off";
  $("#kFps").textContent = running ? Math.round(s.fps) : 0;
  $("#kFpsChip").textContent = `capture ${running ? Math.round(s.captureFps) : 0}`;
  $("#kRes").textContent = s.width ? `${s.width}x${s.height}` : "-";
  const frameMs = 1000 / hz;
  $("#kLat").textContent = running ? frameMs.toFixed(1) : "0.0";
  $("#kLatChip").className = "delta" + (running ? "" : " mute");
  const budget = running ? clamp((s.gpuMs / frameMs) * 100, 0, 100) : 0;
  $("#bPresent").textContent = `${running ? Math.round(s.fps) : 0} fps`;
  $("#bPresentBar").style.width = (running ? clamp((s.fps / hz) * 100, 0, 100) : 0) + "%";
  $("#bCapture").textContent = `${running ? Math.round(s.captureFps) : 0} fps`;
  $("#bCaptureBar").style.width = (running ? clamp((s.captureFps / hz) * 100, 0, 100) : 0) + "%";
  $("#bGpu").textContent = `${budget.toFixed(budget < 10 ? 1 : 0)}%`;
  $("#bGpuBar").style.width = budget + "%";
  $("#sideMs").textContent = running ? s.gpuMs.toFixed(2) : "0.00";
  $("#sideMeter").style.width = Math.max(running ? 2 : 0, budget) + "%";
  $("#sideBudget").textContent = `${budget.toFixed(1)}% of a ${frameMs.toFixed(1)} ms frame`;
}

function renderHeader() {
  const m = state.monitors[clamp(get("display.monitor"), 0, Math.max(0, state.monitors.length - 1))];
  $("#displayBadge").textContent = m ? `${m.name} · ${m.width}x${m.height} · ${m.refresh}Hz` : "No display";
  $("#sideVer").textContent = state.dev ? "dev build" : "v" + state.version;
  const ap = state.settings.activeProfile;
  $("#sideProfile").textContent = ap ? ap : "custom";
  $("#sideProfile").className = "tag" + (ap ? "" : " dim");
  $("[data-pip=blur]").classList.toggle("on", !!(state.settings.blur.enabled && state.settings.blur.strength > 0));
  $("[data-pip=visuals]").classList.toggle("on", !!state.settings.visuals.enabled && visualsActive());
  $("[data-pip=clock]").classList.toggle("on", !!state.settings.clock.enabled);
  $("[data-pip=update]").classList.toggle("on", !!state.update && ["available", "downloading", "restart"].includes(state.update.status));
}
function visualsActive() {
  const c = state.settings.color, f = state.settings.fx, l = state.settings.lut;
  return c.exposure || c.contrast || c.saturation !== 100 || c.vibrance || c.temperature || c.tint ||
    (l.id !== "neutral" && l.intensity > 0) || f.vignette || f.grain || f.bloom || f.sharpen || f.chroma || f.warmth;
}

// ------------------------------------------------------------------ color / fx previews
let previewTimer = 0;
function schedulePreviews() {
  if (previewTimer) return;
  previewTimer = requestAnimationFrame(() => {
    previewTimer = 0;
    const view = $(".view.active").dataset.view;
    const S = state.settings;
    if (view === "color") {
      Looks.render($("#colorPreview"), S, { split: 0.5 });
      const c = S.color;
      const parts = [];
      if (c.exposure) parts.push(`${c.exposure > 0 ? "+" : ""}${c.exposure.toFixed(2)} EV`);
      if (c.saturation !== 100) parts.push(`sat ${c.saturation}%`);
      if (c.temperature) parts.push(`temp ${c.temperature > 0 ? "+" : ""}${c.temperature}`);
      $("#colorCap").textContent = parts.length ? "After, " + parts.join(", ") : "After";
    }
    if (view === "fx") Looks.render($("#fxPreview"), S, { split: 0.5 });
    if (view === "luts") Looks.render($("#lutPreview"), S, { split: 0.5 });
  });
}
function renderVisualsGate() {
  const on = !!state.settings.visuals.enabled;
  $$("[data-needs-visuals]").forEach((el) => el.classList.toggle("muted-block", !on));
}

// ------------------------------------------------------------------ LUTs
function lutSwatch(canvas, id) {
  const s = structuredClone(DEFAULTS);
  s.lut = { id, intensity: 100 };
  Looks.render(canvas, s);
}
function renderLuts() {
  const cur = state.settings.lut.id;
  $("#lutCount").textContent = `${state.luts.length} LUTS`;
  const list = $("#lutList");
  list.innerHTML = "";
  for (const l of state.luts) {
    const li = document.createElement("li");
    li.className = "click";
    const active = l.id === cur;
    li.innerHTML = `<div class="lead"><span class="swatch"><canvas width="52" height="32"></canvas></span>
      <div style="min-width:0"><p class="t" style="margin:0">${escapeHtml(l.name)}</p><p class="s" style="margin:2px 0 0">${l.size ? l.size + "³" : "identity"} · ${l.kind}</p></div></div>
      <div class="r">${active ? `<span class="tag">active</span>` : ""}</div>`;
    li.tabIndex = 0;
    li.setAttribute("role", "button");
    li.setAttribute("aria-pressed", active ? "true" : "false");
    const pick = () => { set("lut.id", l.id); renderLuts(); };
    li.addEventListener("click", pick);
    li.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); pick(); } });
    list.appendChild(li);
    const cv = $("canvas", li);
    if (l.kind === "built-in") lutSwatch(cv, l.id);
    else { const g = cv.getContext("2d"); const gr = g.createLinearGradient(0, 0, 52, 0); gr.addColorStop(0, "#2b2e35"); gr.addColorStop(1, "#d4f53c"); g.fillStyle = gr; g.fillRect(0, 0, 52, 32); }
  }
  const curEntry = state.luts.find((l) => l.id === cur);
  $("#lutActiveTag").textContent = curEntry ? curEntry.name.toUpperCase() : "NEUTRAL";
  schedulePreviews();
}
function escapeHtml(s) { return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])); }

// ------------------------------------------------------------------ clock
function renderClock() {
  const c = state.settings.clock;
  setSeg($("#clockStyleSeg"), "v", c.style);
  setSeg($("#clockLayerSeg"), "v", c.layer);
  $$("#cornerPick button[data-c]").forEach((b) => b.classList.toggle("active", b.dataset.c === c.corner));
  const names = { tl: "Top left", tr: "Top right", bl: "Bottom left", br: "Bottom right", tc: "Top center", custom: "Custom position (dragged)" };
  $("#clockPosText").textContent = (names[c.corner] || "Custom") + (c.corner === "custom" ? "" : " of the work area");
  $("#clockLayerTag").textContent = c.layer === "desktop" ? "DESKTOP" : "ON TOP";
  const w = $("#clockW");
  w.className = "clockw " + c.style + (c.backdrop ? " backdrop" : "");
  const k = clamp(c.size, 40, 250) / 100 * 0.78;
  w.style.opacity = clamp(c.opacity, 5, 100) / 100;
  const pos = { tl: "top:18px;left:18px", tr: "top:18px;right:18px", bl: "bottom:18px;left:18px", br: "bottom:18px;right:18px", tc: "top:18px;left:50%", custom: "top:50%;left:50%" }[c.corner] || "top:18px;right:18px";
  const origin = { tl: "top left", tr: "top right", bl: "bottom left", br: "bottom right", tc: "top center", custom: "center" }[c.corner] || "top right";
  const shift = c.corner === "tc" ? "translateX(-50%) " : c.corner === "custom" ? "translate(-50%,-50%) " : "";
  w.style.cssText += ";top:auto;left:auto;right:auto;bottom:auto;" + pos + `;transform:${shift}scale(${k});transform-origin:${origin};text-align:${c.corner.endsWith("r") ? "right" : "left"}`;
  $(".date", w).style.display = c.date ? "" : "none";
  $(".date", w).style.justifyContent = c.corner.endsWith("r") ? "flex-end" : "flex-start";
  $(".time", w).style.justifyContent = c.corner.endsWith("r") ? "flex-end" : "flex-start";
  const btn = $("#clockMoveBtn");
  btn.classList.toggle("primary", state.clockEditing);
  $("span", btn).textContent = state.clockEditing ? "Done moving" : "Move clock";
  btn.disabled = !c.enabled;
  tickClock();
}
function tickClock() {
  const c = state.settings.clock;
  const d = new Date();
  let h = d.getHours();
  const m = String(d.getMinutes()).padStart(2, "0");
  const s = String(d.getSeconds()).padStart(2, "0");
  let ampm = "";
  if (!c.hour24) { ampm = h < 12 ? " AM" : " PM"; h = h % 12 || 12; }
  const hh = c.hour24 ? String(h).padStart(2, "0") : String(h);
  const days = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"];
  const months = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"];
  if (c.style === "mono") {
    $("#cwTime").innerHTML = `${hh}<span class="colon">:</span>${m}${c.seconds ? `<span class="colon">:</span>${s}` : ""}`;
    $("#cwSec").textContent = ampm.trim();
  } else {
    $("#cwTime").textContent = `${hh}:${m}`;
    $("#cwSec").textContent = (c.seconds ? s : "") + ampm;
  }
  $("#cwDate").textContent = `${days[d.getDay()]}  ${String(d.getDate()).padStart(2, "0")} ${months[d.getMonth()]} ${d.getFullYear()}`;
}

// ------------------------------------------------------------------ profiles
function allProfiles() {
  return [...BUILTIN_PROFILES.map((p) => ({ ...p, builtin: true })), ...(state.settings.profiles || []).map((p) => ({ ...p, builtin: false, desc: p.desc || "Saved look" }))];
}
function profileSettings(p) {
  const s = structuredClone(DEFAULTS);
  merge(s, p.data);
  return s;
}
function applyProfile(p) {
  const s = profileSettings(p);
  for (const k of ["blur", "visuals", "color", "lut", "fx"]) state.settings[k] = s[k];
  state.settings.activeProfile = p.name;
  pushSettings();
  renderAll();
  toast(`${p.name} applied`, "ok");
}
function renderProfiles() {
  const grid = $("#profileGrid");
  if (!grid) return;
  grid.innerHTML = "";
  for (const p of allProfiles()) {
    const el = document.createElement("div");
    const active = state.settings.activeProfile === p.name;
    el.className = "pcard" + (active ? " active" : "");
    el.innerHTML = `<div class="thumb"><canvas width="256" height="144"></canvas></div>
      <div class="row"><span class="n">${escapeHtml(p.name)}</span>${active ? `<span class="tag">active</span>` : p.builtin ? `<span class="tag dim">built-in</span>` : `<button class="del" title="Delete">${svgUse("i-trash", 14)}</button>`}</div>
      <p class="d" style="margin:3px 0 0">${escapeHtml(p.desc)}</p>`;
    el.addEventListener("click", (e) => {
      if (e.target.closest(".del")) {
        state.settings.profiles = state.settings.profiles.filter((x) => x.name !== p.name);
        if (state.settings.activeProfile === p.name) state.settings.activeProfile = "";
        pushSettings(); renderProfiles(); return;
      }
      applyProfile(p);
    });
    grid.appendChild(el);
    if ($(".view.active")?.dataset.view === "profiles") requestAnimationFrame(() => Looks.render($("canvas", el), profileSettings(p)));
  }
  if (!(state.settings.profiles || []).length) {
    const e = document.createElement("div");
    e.className = "pcard empty";
    e.innerHTML = `<div><span class="n" style="font-size:13px;color:var(--fg-2)">Your looks</span><p>Tune blur, color and effects, then name it above to save it here.</p></div>`;
    grid.appendChild(e);
  }
  renderHeader();
}

// ------------------------------------------------------------------ display / about
function renderMonitors() {
  const cur = get("display.monitor");
  $("#monCount").textContent = `${state.monitors.length} FOUND`;
  const list = $("#monList");
  list.innerHTML = "";
  state.monitors.forEach((m, i) => {
    const li = document.createElement("li");
    li.className = "click";
    li.innerHTML = `<div class="lead"><span class="logo" style="width:32px;height:32px;border-radius:8px;color:${i === cur ? "var(--volt)" : "var(--fg-3)"}">${svgUse("i-display", 16)}</span>
      <div><p class="t" style="margin:0">${escapeHtml(m.name)}${m.primary ? ` <span class="badge" style="height:18px;font-size:11px;margin-left:6px">PRIMARY</span>` : ""}</p>
      <p class="s" style="margin:2px 0 0">${m.width}x${m.height} · ${m.refresh} Hz · ${escapeHtml(m.device.replace(/^\\\\\.\\/, ""))}</p></div></div>
      <div class="r">${i === cur ? `<span class="tag">rendering</span>` : `<span class="tag dim">use</span>`}</div>`;
    li.addEventListener("click", () => { set("display.monitor", i); renderMonitors(); renderHeader(); });
    list.appendChild(li);
  });
  $("#autostartToggle").checked = state.autostart;
}

function renderUpdate() {
  const u = state.update || (state.dev ? { status: "dev" } : {});
  $("#aboutVer").textContent = state.dev ? "dev build" : "v" + state.version;
  const chip = $("#updChip");
  const prog = $("#updProg");
  let text = "Ionization checks GitHub for new builds on launch and every six hours, verifies the download, and restarts itself in under a second.";
  let chipText = "current", tone = "";
  prog.style.display = "none";
  switch (u.status) {
    case "checking": chipText = "checking"; tone = " mute"; break;
    case "current": chipText = "up to date"; text = `You are on the latest release (${u.latest}).`; break;
    case "available": chipText = "update ready"; text = `${u.latest} is available.`; break;
    case "downloading": chipText = `${u.progress || 0}%`; text = `Downloading ${u.latest}...`; prog.style.display = ""; $("#updBar").style.width = (u.progress || 0) + "%"; break;
    case "restart": chipText = "restarting"; text = `Installing ${u.latest}, restarting...`; prog.style.display = ""; $("#updBar").style.width = "100%"; break;
    case "updated": chipText = "updated"; text = `Updated to v${u.latest}. Welcome back.`; break;
    case "error": chipText = "error"; tone = " bad"; text = u.error || "Update check failed."; break;
    case "dev": chipText = "dev build"; tone = " mute"; text = "This is a local development build, so auto-update is off. Builds from GitHub Releases update themselves."; break;
  }
  chip.className = "delta" + tone;
  chip.innerHTML = `<span>${chipText}</span>`;
  $("#updText").textContent = text;
  $("#checkBtn").disabled = u.status === "checking" || u.status === "downloading";
  renderHeader();
}

// ------------------------------------------------------------------ all
function syncBinds() {
  $$("input[type=checkbox][data-bind]").forEach((el) => { el.checked = !!get(el.dataset.bind); });
  sliders.forEach((s) => s.update());
}
function refreshFor(section) {
  syncBinds();
  renderHeader();
  if (section === "blur") renderBlur();
  if (section === "color" || section === "fx" || section === "lut" || section === "visuals") { schedulePreviews(); renderVisualsGate(); }
  if (section === "lut") renderLuts();
  if (section === "clock") renderClock();
  if (section === "app") renderShortcuts();
}
function renderAll() {
  syncBinds();
  renderHeader();
  renderBlur();
  renderShortcuts();
  renderStats();
  renderLuts();
  renderClock();
  renderProfiles();
  renderMonitors();
  renderUpdate();
  renderVisualsGate();
  schedulePreviews();
  $$(".seg").forEach(placeIndicator);
}

function toast(text, tone = "info") {
  const t = document.createElement("div");
  t.className = "toast " + (tone || "");
  t.innerHTML = `<i class="dot on"></i><span></span>`;
  $("span", t).textContent = text;
  $("#toasts").appendChild(t);
  setTimeout(() => { t.classList.add("out"); setTimeout(() => t.remove(), 300); }, 2600);
}

// ------------------------------------------------------------------ wiring
function build() {
  makeSlider($("#heroSlider"), { path: "blur.strength", label: "Blur strength", min: 0, max: 100, step: 1, unit: "%", def: 45, hero: true });
  const rows = (host, defs) => defs.forEach((d) => makeSlider($(host), d));
  rows("#colorRows", [
    { path: "color.exposure", label: "Exposure", min: -2, max: 2, step: 0.05, unit: "EV", def: 0 },
    { path: "color.contrast", label: "Contrast", min: -100, max: 100, step: 1, unit: "", def: 0 },
    { path: "color.saturation", label: "Saturation", min: 0, max: 200, step: 1, unit: "%", def: 100 },
    { path: "color.vibrance", label: "Vibrance", min: -100, max: 100, step: 1, unit: "", def: 0, hint: "protects saturated colors" },
  ]);
  rows("#wbRows", [
    { path: "color.temperature", label: "Temperature", min: -100, max: 100, step: 1, unit: "", def: 0, hint: "cool / warm" },
    { path: "color.tint", label: "Tint", min: -100, max: 100, step: 1, unit: "", def: 0, hint: "green / magenta" },
  ]);
  rows("#lutRows", [{ path: "lut.intensity", label: "Intensity", min: 0, max: 100, step: 1, unit: "%", def: 100 }]);
  rows("#lensRows", [
    { path: "fx.vignette", label: "Vignette", min: 0, max: 100, step: 1, unit: "%", def: 0 },
    { path: "fx.vignetteSoft", label: "Vignette softness", min: 0, max: 100, step: 1, unit: "%", def: 50 },
    { path: "fx.chroma", label: "Chromatic aberration", min: 0, max: 100, step: 1, unit: "%", def: 0, hint: "RGB fringing" },
    { path: "fx.sharpen", label: "Sharpen", min: 0, max: 100, step: 1, unit: "%", def: 0 },
  ]);
  rows("#glowRows", [
    { path: "fx.bloom", label: "Bloom", min: 0, max: 100, step: 1, unit: "%", def: 0, hint: "glow on highlights" },
    { path: "fx.bloomThreshold", label: "Bloom threshold", min: 0, max: 100, step: 1, unit: "%", def: 70 },
  ]);
  rows("#filmRows", [
    { path: "fx.grain", label: "Film grain", min: 0, max: 100, step: 1, unit: "%", def: 0 },
    { path: "fx.warmth", label: "Night warmth", min: 0, max: 100, step: 1, unit: "%", def: 0, hint: "cuts blue light" },
  ]);
  rows("#clockRows", [
    { path: "clock.size", label: "Size", min: 50, max: 200, step: 1, unit: "%", def: 100 },
    { path: "clock.opacity", label: "Opacity", min: 10, max: 100, step: 1, unit: "%", def: 100 },
  ]);

  $$("input[type=checkbox][data-bind]").forEach((el) => el.addEventListener("change", () => set(el.dataset.bind, el.checked)));
  $("#autostartToggle").addEventListener("change", (e) => send("autostart", { value: e.target.checked }));
  $("#compareToggle").addEventListener("change", drawChart);
  $("#cmdBlur").addEventListener("click", () => set("blur.enabled", !get("blur.enabled")));

  $$("#nav button").forEach((b) => b.addEventListener("click", () => showView(b.dataset.view)));
  $$("#modeTabs button").forEach((b) => b.addEventListener("click", () => set("blur.mode", b.dataset.mode)));
  $$("#presetSeg button").forEach((b) => b.addEventListener("click", () => { set("blur.strength", Number(b.dataset.preset)); if (!get("blur.enabled")) set("blur.enabled", true); }));
  $$("#clockStyleSeg button").forEach((b) => b.addEventListener("click", () => set("clock.style", b.dataset.v)));
  $$("#clockLayerSeg button").forEach((b) => b.addEventListener("click", () => set("clock.layer", b.dataset.v)));
  $$("#cornerPick button[data-c]").forEach((b) => b.addEventListener("click", () => set("clock.corner", b.dataset.c)));
  $("#clockMoveBtn").addEventListener("click", () => send("clockEdit", { value: !state.clockEditing }));
  $$("[data-reset]").forEach((b) => b.addEventListener("click", () => {
    const sec = b.dataset.reset;
    state.settings[sec] = structuredClone(DEFAULTS[sec]);
    state.settings.activeProfile = "";
    pushSettings(); renderAll();
  }));
  $$("[data-action]").forEach((b) => b.addEventListener("click", () => send(b.dataset.action)));
  $("#saveProfile").addEventListener("submit", (e) => {
    e.preventDefault();
    const name = $("#profileName").value.trim();
    if (!name) { $("#profileName").focus(); return; }
    if (BUILTIN_PROFILES.some((p) => p.name.toLowerCase() === name.toLowerCase())) { toast("That name belongs to a built-in profile", "error"); return; }
    const data = {};
    for (const k of ["blur", "visuals", "color", "lut", "fx"]) data[k] = structuredClone(state.settings[k]);
    const list = (state.settings.profiles || []).filter((p) => p.name !== name);
    list.push({ name, desc: describe(data), data });
    state.settings.profiles = list;
    state.settings.activeProfile = name;
    $("#profileName").value = "";
    pushSettings(); renderProfiles();
    toast(`Saved "${name}"`, "ok");
  });

  // Window dragging from empty top-bar space.
  $("#topbar").addEventListener("mousedown", (e) => {
    if (e.button !== 0 || e.target.closest("button, kbd, input, .command")) return;
    send("drag");
  });
  document.addEventListener("contextmenu", (e) => { if (!state.dev) e.preventDefault(); });
  window.addEventListener("resize", () => { drawChart(); $$(".seg").forEach(placeIndicator); placeTabBar($("#modeTabs")); });
  setInterval(tickClock, 1000);
}

function describe(d) {
  const bits = [`${d.blur.mode} ${d.blur.strength}%`];
  const lut = state.luts.find((l) => l.id === d.lut.id);
  if (lut && d.lut.id !== "neutral") bits.push(lut.name);
  if (d.fx.bloom) bits.push("bloom");
  if (d.fx.grain) bits.push("grain");
  if (d.fx.vignette) bits.push("vignette");
  return bits.join(", ");
}

function showView(v) {
  $$("#nav button").forEach((b) => b.classList.toggle("active", b.dataset.view === v));
  $$(".view").forEach((s) => s.classList.toggle("active", s.dataset.view === v));
  $("#main").scrollTop = 0;
  requestAnimationFrame(() => {
    $$(".seg").forEach(placeIndicator);
    placeTabBar($("#modeTabs"));
    if (v === "blur") { drawChart(); Demo.kick(); }
    if (v === "profiles") renderProfiles();
    schedulePreviews();
  });
}

// ------------------------------------------------------------------ live blur preview
// Renders a small scene (a window being dragged, a page scrolling, a fast orb) and
// blurs it with the same weights the GPU uses: exponential for trail, box for blend.
// Frames are re-averaged from a ring every tick, so there is no 8-bit ghosting.
const Demo = {
  ring: [], max: 32, head: 0, filled: 0, raf: 0, last: 0, t: 0, w: 0, h: 0, dpr: 1,
  reduce: window.matchMedia("(prefers-reduced-motion: reduce)"),
  kick() { if (!this.raf) this.raf = requestAnimationFrame((ts) => this.frame(ts)); },
  active() { const v = $(".view.active"); return !document.hidden && v && v.dataset.view === "blur"; },
  resize() {
    const c = $("#demoCanvas");
    const r = c.getBoundingClientRect();
    const dpr = Math.min(1.5, window.devicePixelRatio || 1);
    const w = Math.round(r.width * dpr), h = Math.round(r.height * dpr);
    if (!w || !h) return false;
    if (w !== this.w || h !== this.h) {
      this.w = c.width = w; this.h = c.height = h; this.dpr = dpr;
      this.ring = []; this.filled = 0; this.head = 0;
    }
    return true;
  },
  cardX(t, W) { return W * 0.56 + Math.sin(t * 1.5) * W * 0.27; },
  scene(g, t) {
    const d = this.dpr, W = this.w / d, H = this.h / d;
    g.setTransform(d, 0, 0, d, 0, 0);
    g.fillStyle = "#0a0b0d"; g.fillRect(0, 0, W, H);
    g.strokeStyle = "rgba(255,255,255,0.035)"; g.lineWidth = 1;
    g.beginPath();
    for (let x = 0.5; x < W; x += 24) { g.moveTo(x, 0); g.lineTo(x, H); }
    for (let y = 0.5; y < H; y += 24) { g.moveTo(0, y); g.lineTo(W, y); }
    g.stroke();
    // A page scrolling in the left column.
    const off = (t * 110) % 26;
    g.fillStyle = "#15171b"; rr(g, 16, 14, 150, H - 28, 7); g.fill();
    g.save(); rr(g, 16, 14, 150, H - 28, 7); g.clip();
    for (let i = -1; i < 9; i++) {
      const y = 26 + i * 26 - off;
      const k = (i + Math.floor(t * 110 / 26)) % 5;
      g.fillStyle = k === 0 ? "#f3f4f6" : "#5d626b";
      rr(g, 28, y, k === 0 ? 70 : 110 - k * 12, 5, 2.5); g.fill();
      if (k === 2) { g.fillStyle = "#2b2e35"; rr(g, 28, y + 9, 120, 9, 3); g.fill(); }
    }
    g.restore();
    // A fast orb on a figure-eight.
    const ox = W * 0.62 + Math.cos(t * 2.6) * W * 0.3, oy = H * 0.5 + Math.sin(t * 5.2) * H * 0.3;
    g.fillStyle = "#d4f53c"; g.beginPath(); g.arc(ox, oy, 9, 0, Math.PI * 2); g.fill();
    // A window being dragged.
    const cx = this.cardX(t, W), cy = H * 0.5 - 40;
    g.fillStyle = "#15171b"; rr(g, cx - 80, cy, 160, 82, 9); g.fill();
    g.strokeStyle = "#2b2e35"; rr(g, cx - 79.5, cy + 0.5, 159, 81, 8.5); g.stroke();
    g.fillStyle = "#d4f53c"; rr(g, cx - 66, cy + 14, 18, 18, 5); g.fill();
    g.fillStyle = "#f3f4f6"; rr(g, cx - 40, cy + 16, 82, 5, 2.5); g.fill();
    g.fillStyle = "#8c919a"; rr(g, cx - 40, cy + 26, 56, 4, 2); g.fill();
    g.fillStyle = "#3d8bfd"; rr(g, cx - 66, cy + 50, 60, 18, 5); g.fill();
    g.fillStyle = "#2b2e35"; rr(g, cx + 2, cy + 50, 60, 18, 5); g.fill();
    function rr(g, x, y, w, h, r) { g.beginPath(); g.roundRect(x, y, w, h, r); }
  },
  cursor(g, t) {
    // The real cursor is drawn above the overlay, so it stays sharp: show that.
    const d = this.dpr, W = this.w / d, H = this.h / d;
    const x = this.cardX(t, W) - 20, y = H * 0.5 - 30;
    g.setTransform(d, 0, 0, d, 0, 0);
    g.beginPath();
    g.moveTo(x, y); g.lineTo(x, y + 16); g.lineTo(x + 4.2, y + 12.2); g.lineTo(x + 7, y + 18.4);
    g.lineTo(x + 9.6, y + 17.2); g.lineTo(x + 6.9, y + 11.2); g.lineTo(x + 12, y + 11.2); g.closePath();
    g.fillStyle = "#ffffff"; g.fill();
    g.lineWidth = 1.2; g.strokeStyle = "#0b0d02"; g.stroke();
  },
  frame(ts) {
    this.raf = 0;
    if (!this.active() || !this.resize()) { this.last = 0; return; }
    const dt = this.last ? Math.min(0.05, (ts - this.last) / 1000) : 1 / 60;
    this.last = ts;
    const still = this.reduce.matches;
    if (!still) this.t += dt;
    let cv = this.ring[this.head];
    if (!cv) { cv = this.ring[this.head] = document.createElement("canvas"); cv.width = this.w; cv.height = this.h; }
    this.scene(cv.getContext("2d"), this.t);
    this.head = (this.head + 1) % this.max;
    this.filled = Math.min(this.filled + 1, this.max);

    const b = state.settings.blur;
    const s = b.enabled ? b.strength : 0;
    let ws = [1];
    if (s > 0 && b.mode === "trail") {
      ws = [];
      const d = Math.exp(-dt / trailTau(s));
      for (let k = 0, w = 1; k < this.max && w > 0.004; k++, w *= d) ws.push(w);
    } else if (s > 0) {
      ws = new Array(blendN(s)).fill(1);
    }
    ws = ws.slice(0, this.filled);
    const out = $("#demoCanvas").getContext("2d");
    out.setTransform(1, 0, 0, 1, 0, 0);
    let cum = 0;
    for (let k = ws.length - 1; k >= 0; k--) {
      cum += ws[k];
      out.globalAlpha = ws[k] / cum;
      out.drawImage(this.ring[(this.head - 1 - k + this.max * 2) % this.max], 0, 0);
    }
    out.globalAlpha = 1;
    this.cursor(out, this.t);
    const cap = still ? "Animation paused to respect reduced motion"
      : s <= 0 ? "Blur is off: this is the raw frame"
      : b.mode === "trail" ? `Dragging a window with a ${Math.round(trailTau(s) * 1000)} ms trail`
      : `Dragging a window, ${blendN(s)} frames averaged`;
    const capEl = $("#demoCap");
    if (capEl.textContent !== cap) capEl.textContent = cap;
    this.kick();
  },
};
document.addEventListener("visibilitychange", () => Demo.kick());

// ------------------------------------------------------------------ mock host (browser only)
const Mock = {
  timer: 0,
  handle(m) {
    if (m.type === "ready") {
      setTimeout(() => onHost({
        type: "init", settings: structuredClone(DEFAULTS), version: "0.0.0-dev", dev: true, autostart: false,
        hotkeys: { blur: true, visuals: true, clock: true },
        monitors: [{ name: "DELL U2720Q", device: "\\\\.\\DISPLAY1", width: 2560, height: 1440, refresh: 144, primary: true },
                   { name: "LG HDR WFHD", device: "\\\\.\\DISPLAY2", width: 2560, height: 1080, refresh: 75, primary: false }],
        luts: [{ id: "neutral", name: "Neutral", kind: "built-in", size: 0 }, ...Object.keys(Looks.LOOKS).map((id) => ({
          id, kind: "built-in", size: 33,
          name: { "teal-orange": "Teal & Orange", "warm-film": "Warm Film", "cold-steel": "Cold Steel", "faded-matte": "Faded Matte", "bleach-bypass": "Bleach Bypass", "cross-process": "Cross Process", "amber-night": "Amber Night", mono: "Mono", vivid: "Vivid" }[id],
        }))],
      }), 30);
      this.timer = setInterval(() => {
        const on = (state.settings.blur.enabled && state.settings.blur.strength > 0) || (state.settings.visuals.enabled && visualsActive());
        const hz = (state.monitors[state.settings.display.monitor] || {}).refresh || 60;
        onHost({ type: "stats", running: on, fps: on ? hz - Math.random() * 0.6 : 0, captureFps: on ? hz * (0.55 + Math.random() * 0.3) : 0, gpuMs: on ? 0.9 + Math.random() * 0.25 : 0, width: 2560, height: 1440, refresh: hz });
      }, 500);
    } else if (m.type === "checkUpdate") {
      onHost({ type: "update", value: { status: "dev" } });
    } else if (m.type === "clockEdit") {
      onHost({ type: "clockEditing", value: m.value });
    } else if (m.type === "autostart") {
      onHost({ type: "autostart", value: m.value });
    } else if (m.type === "importLut") {
      toast("Import opens a file picker in the app", "info");
    }
  },
};

build();
// Tell the host once the first real frame is on screen, so the window never shows empty.
let painted = false;
function reportPainted() {
  if (painted) return;
  painted = true;
  document.fonts.ready.then(() => requestAnimationFrame(() => requestAnimationFrame(() => send("painted"))));
}
document.fonts.ready.then(() => { renderAll(); });
send("ready");
