"use strict";
// Macros page: list, editor, trigger capture, recording and the step list.

const MOD = { ALT: 1, CTRL: 2, SHIFT: 4, WIN: 8 };
const VK_NAMES = {
  8: "Backspace", 9: "Tab", 13: "Enter", 16: "Shift", 17: "Ctrl", 18: "Alt", 19: "Pause", 20: "Caps Lock", 27: "Esc", 32: "Space",
  33: "Page Up", 34: "Page Down", 35: "End", 36: "Home", 37: "Left", 38: "Up", 39: "Right", 40: "Down", 44: "Print Screen", 45: "Insert", 46: "Delete",
  91: "Win", 92: "Win", 93: "Menu", 96: "Num 0", 97: "Num 1", 98: "Num 2", 99: "Num 3", 100: "Num 4", 101: "Num 5", 102: "Num 6", 103: "Num 7",
  104: "Num 8", 105: "Num 9", 106: "Num *", 107: "Num +", 109: "Num -", 110: "Num .", 111: "Num /", 144: "Num Lock", 145: "Scroll Lock",
  160: "Left Shift", 161: "Right Shift", 162: "Left Ctrl", 163: "Right Ctrl", 164: "Left Alt", 165: "Right Alt",
  186: ";", 187: "=", 188: ",", 189: "-", 190: ".", 191: "/", 192: "`", 219: "[", 220: "\\", 221: "]", 222: "'",
};
const MOUSE_NAMES = { 3: "Middle mouse", 4: "Mouse side 1", 5: "Mouse side 2" };
const BUTTON_NAMES = ["Left", "Right", "Middle", "Side 1", "Side 2"];
// KeyboardEvent.code -> [scan code, extended]; scan codes make games accept the input.
const SCAN = {
  Escape: [1], Minus: [12], Equal: [13], Backspace: [14], Tab: [15], BracketLeft: [26], BracketRight: [27], Enter: [28],
  ControlLeft: [29], Semicolon: [39], Quote: [40], Backquote: [41], ShiftLeft: [42], Backslash: [43], Comma: [51], Period: [52],
  Slash: [53], ShiftRight: [54], AltLeft: [56], Space: [57], CapsLock: [58], F11: [87], F12: [88], ControlRight: [29, 1], AltRight: [56, 1],
  ArrowUp: [72, 1], ArrowLeft: [75, 1], ArrowRight: [77, 1], ArrowDown: [80, 1], Insert: [82, 1], Delete: [83, 1], Home: [71, 1],
  End: [79, 1], PageUp: [73, 1], PageDown: [81, 1],
};
"1234567890".split("").forEach((d, i) => (SCAN["Digit" + d] = [2 + i]));
"QWERTYUIOP".split("").forEach((c, i) => (SCAN["Key" + c] = [16 + i]));
"ASDFGHJKL".split("").forEach((c, i) => (SCAN["Key" + c] = [30 + i]));
"ZXCVBNM".split("").forEach((c, i) => (SCAN["Key" + c] = [44 + i]));
for (let i = 1; i <= 10; i++) SCAN["F" + i] = [58 + i];

function vkName(vk) {
  if (VK_NAMES[vk]) return VK_NAMES[vk];
  if ((vk >= 48 && vk <= 57) || (vk >= 65 && vk <= 90)) return String.fromCharCode(vk);
  if (vk >= 112 && vk <= 135) return "F" + (vk - 111);
  return "Key " + vk;
}
const kbd = (t) => `<kbd>${escapeHtml(t)}</kbd>`;
function triggerKeys(t) {
  if (!t || (!t.vk && !t.mouse)) return [];
  const k = [];
  if (t.mods & MOD.CTRL) k.push("Ctrl");
  if (t.mods & MOD.ALT) k.push("Alt");
  if (t.mods & MOD.SHIFT) k.push("Shift");
  if (t.mods & MOD.WIN) k.push("Win");
  k.push(t.mouse ? MOUSE_NAMES[t.mouse] : vkName(t.vk));
  return k;
}

const TEMPLATES = {
  blank: () => ({ name: "New macro", trigger: { vk: 0, mods: 0, mouse: 0, mode: "press", block: true }, repeat: "once", steps: [] }),
  clicker: () => ({ name: "Auto-clicker", trigger: { vk: 117, mods: 0, mouse: 0, mode: "toggle", block: true }, repeat: "forever",
    steps: [{ k: "md", b: 0, w: 0 }, { k: "mu", b: 0, w: 15 }, { k: "dl", w: 85 }] }),
  spammer: () => ({ name: "Key spammer", trigger: { vk: 118, mods: 0, mouse: 0, mode: "toggle", block: true }, repeat: "forever",
    steps: [{ k: "kd", vk: 69, sc: 18, w: 0 }, { k: "ku", vk: 69, sc: 18, w: 30 }, { k: "dl", w: 70 }] }),
};

const Macros = {
  list: [], sel: null, capturing: null, saveTimer: 0,
  status: { recording: false, events: 0, playing: [] },

  init(msg) {
    this.list = Array.isArray(msg.macros) ? msg.macros : [];
    if (msg.macroStatus) this.status = msg.macroStatus;
    if (!this.cur() && this.list[0]) this.sel = this.list[0].id;
    this.render();
  },
  onMessage(msg) {
    if (msg.type === "macroStatus") { this.status = msg.value || this.status; this.renderStatus(); this.renderList(); }
    if (msg.type === "macroRecorded") {
      const m = this.cur();
      if (m && Array.isArray(msg.steps)) {
        if (msg.steps.length) { m.steps = msg.steps; this.save(); toast(`Recorded ${msg.steps.length} steps`, "ok"); }
        else toast("Nothing was recorded", "error");
        this.renderEditor();
      }
    }
  },
  cur() { return this.list.find((m) => m.id === this.sel); },
  save() {
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => send("macros", { value: this.list }), 150);
    this.renderList();
  },
  create(kind) {
    const t = (TEMPLATES[kind] || TEMPLATES.blank)();
    const m = Object.assign({ id: "m" + Date.now().toString(36), enabled: true, scope: "", count: 3, speed: 1, humanize: 0 }, t);
    this.list.push(m);
    this.sel = m.id;
    this.save();
    this.render();
    if (kind !== "blank") toast(`${m.name} added. ${triggerKeys(m.trigger).join("+")} starts and stops it`, "ok");
  },

  render() { this.renderList(); this.renderEditor(); this.renderStatus(); },

  renderList() {
    const ul = $("#macroList");
    if (!ul) return;
    $("#macroCount").textContent = String(this.list.length);
    $("#macroEmpty").style.display = this.list.length ? "none" : "";
    ul.innerHTML = "";
    for (const m of this.list) {
      const li = document.createElement("li");
      li.className = "click" + (m.id === this.sel ? " sel" : "");
      const playing = this.status.playing.includes(m.id);
      const keys = triggerKeys(m.trigger);
      li.innerHTML = `<div style="min-width:0"><p class="t" style="margin:0">${escapeHtml(m.name || "Macro")}</p>
          <p class="s" style="margin:3px 0 0">${m.steps.length} steps · ${escapeHtml(m.trigger.mode)}${m.enabled ? "" : " · off"}</p>
          <div class="mrow-keys" style="margin-top:6px">${keys.length ? keys.map(kbd).join("") : `<span class="tag dim">no trigger</span>`}</div></div>
        <div class="r"><button class="icon-btn${playing ? " on" : ""}" title="${playing ? "Stop" : "Play"}">${svgUse(playing ? "i-stop" : "i-play", 12)}</button>
          ${playing ? `<span class="tag">running</span>` : ""}</div>`;
      li.addEventListener("click", (e) => {
        if (e.target.closest(".icon-btn")) { playing ? send("macroStop") : send("macroPlay", { id: m.id }); return; }
        this.sel = m.id; this.render();
      });
      ul.appendChild(li);
    }
    $("[data-pip=macro]")?.classList.toggle("on", this.status.playing.length > 0 || this.status.recording);
  },

  renderEditor() {
    const m = this.cur();
    const ed = $("#macroEditor");
    if (!ed) return;
    ed.style.display = m ? "" : "none";
    if (!m) return;
    if (document.activeElement !== $("#mName")) $("#mName").value = m.name || "";
    $("#mEnabled").checked = m.enabled !== false;
    $("#mBlock").checked = m.trigger.block !== false;
    if (document.activeElement !== $("#mScope")) $("#mScope").value = m.scope || "";
    const last = state.game && state.game.lastExe;
    $("#mScopeLast").textContent = last ? `Use ${gameName(last)}` : "";
    $("#mScopeLast").style.display = last ? "" : "none";
    setSeg($("#mMode"), "v", m.trigger.mode);
    setSeg($("#mRepeat"), "v", m.repeat);
    $("#mCountBox").style.display = m.repeat === "times" ? "" : "none";
    if (document.activeElement !== $("#mCount")) $("#mCount").value = m.count || 1;
    $("#mModeHint").textContent = { press: "Runs once per press (with the repeat below)", toggle: "First press starts a loop, next press stops it",
      hold: "Repeats while you hold the trigger" }[m.trigger.mode];
    this.renderTrigger();
    this.renderSliders();
    this.renderSteps();
    this.renderStatus();
  },

  renderTrigger() {
    const m = this.cur();
    const b = $("#mTrigger");
    if (this.capturing === "trigger") { b.classList.add("listening"); b.innerHTML = "Press keys or a mouse button…"; return; }
    b.classList.remove("listening");
    const keys = triggerKeys(m.trigger);
    b.innerHTML = keys.length ? keys.map(kbd).join("") : "Set trigger";
  },

  renderSliders() {
    const host = $("#macroRows");
    const m = this.cur();
    host.innerHTML = "";
    const row = (label, hint, min, max, step, unit, value, onset, fmt) => {
      const d = document.createElement("div");
      d.className = "srow";
      d.innerHTML = `<div class="top"><span class="lab"><span class="name">${label}</span><span class="sub">${hint}</span></span>
        <label class="numbox"><input inputmode="decimal" spellcheck="false"><span class="unit">${unit}</span></label></div>
        <input type="range" min="${min}" max="${max}" step="${step}">`;
      const r = $("input[type=range]", d), n = $(".numbox input", d);
      const upd = (v) => { r.value = v; n.value = fmt(v); r.style.setProperty("--p", ((v - min) / (max - min)) * 100 + "%"); r.classList.toggle("hot", true); };
      upd(value);
      r.addEventListener("input", () => { onset(Number(r.value)); upd(Number(r.value)); this.save(); });
      n.addEventListener("change", () => { const v = clamp(parseFloat(n.value) || min, min, max); onset(v); upd(v); this.save(); });
      host.appendChild(d);
    };
    row("Speed", "playback rate", 25, 400, 5, "%", Math.round((m.speed || 1) * 100), (v) => (m.speed = v / 100), (v) => String(Math.round(v)));
    row("Humanize", "random timing variation", 0, 50, 1, "%", m.humanize || 0, (v) => (m.humanize = v), (v) => String(Math.round(v)));
  },

  // Group raw steps into readable rows (mouse paths collapse into one row).
  groups(steps) {
    const out = [];
    for (let i = 0; i < steps.length; i++) {
      const s = steps[i], n = steps[i + 1];
      if (s.k === "mv" || s.k === "rm") {
        let j = i;
        while (j + 1 < steps.length && steps[j + 1].k === s.k) j++;
        out.push({ from: i, to: j }); i = j; continue;
      }
      if ((s.k === "kd" && n && n.k === "ku" && n.vk === s.vk) || (s.k === "md" && n && n.k === "mu" && n.b === s.b)) { out.push({ from: i, to: i + 1 }); i++; continue; }
      out.push({ from: i, to: i });
    }
    return out;
  },
  describe(steps, g) {
    const s = steps[g.from], e = steps[g.to];
    const pair = g.to === g.from + 1 && (s.k === "kd" || s.k === "md");
    switch (s.k) {
      case "kd": return pair ? `Press ${kbd(vkName(s.vk))}<span class="k">held ${e.w} ms</span>` : `${kbd(vkName(s.vk))} down`;
      case "ku": return `${kbd(vkName(s.vk))} up`;
      case "md": return pair ? `${BUTTON_NAMES[s.b] || "Mouse"} click<span class="k">held ${e.w} ms</span>` : `${BUTTON_NAMES[s.b] || "Mouse"} button down`;
      case "mu": return `${BUTTON_NAMES[s.b] || "Mouse"} button up`;
      case "mv": {
        const n = g.to - g.from + 1;
        const ms = steps.slice(g.from + 1, g.to + 1).reduce((a, x) => a + (x.w || 0), 0);
        return n > 1 ? `Mouse path<span class="k">${n} points, ${ms} ms, to ${Math.round(e.x * 100)}% ${Math.round(e.y * 100)}%</span>`
          : `Move mouse to<span class="k">${Math.round(s.x * 100)}% across, ${Math.round(s.y * 100)}% down</span>`;
      }
      case "rm": return `Move mouse by<span class="k">${steps.slice(g.from, g.to + 1).reduce((a, x) => a + x.dx, 0)}, ${steps.slice(g.from, g.to + 1).reduce((a, x) => a + x.dy, 0)} px</span>`;
      case "wh": return `Scroll ${s.h ? (s.d > 0 ? "right" : "left") : s.d > 0 ? "up" : "down"}<span class="k">${Math.abs(Math.round(s.d / 120)) || 1} notch${Math.abs(s.d) > 120 ? "es" : ""}</span>`;
      case "tx": return `Type <input class="field" data-text value="${escapeHtml(s.s || "")}" placeholder="text to type" style="height:24px;flex:1;min-width:120px">`;
      case "dl": return `Wait<span class="k">pause only</span>`;
    }
    return escapeHtml(s.k);
  },
  renderSteps() {
    const m = this.cur();
    const ul = $("#mSteps");
    const steps = m.steps;
    $("#mStepCount").textContent = `${steps.length} ${steps.length === 1 ? "STEP" : "STEPS"}`;
    ul.innerHTML = "";
    if (!steps.length) { ul.innerHTML = `<li class="steps-empty" style="display:block">Press Record and do it once, or add steps below.</li>`; return; }
    const groups = this.groups(steps);
    const shown = groups.slice(0, 400);
    for (const g of shown) {
      const li = document.createElement("li");
      li.innerHTML = `<span class="wait"><input value="${steps[g.from].w || 0}" inputmode="numeric" title="Wait before this step">ms</span>
        <span class="desc">${this.describe(steps, g)}</span>
        <button class="x" title="Remove">${svgUse("i-trash", 13)}</button>`;
      $(".wait input", li).addEventListener("change", (e) => { steps[g.from].w = Math.max(0, parseInt(e.target.value, 10) || 0); this.save(); });
      const t = $("[data-text]", li);
      if (t) t.addEventListener("input", (e) => { steps[g.from].s = e.target.value; this.save(); });
      $(".x", li).addEventListener("click", () => { steps.splice(g.from, g.to - g.from + 1); this.save(); this.renderSteps(); });
      ul.appendChild(li);
    }
    if (groups.length > shown.length) {
      const li = document.createElement("li");
      li.className = "steps-empty"; li.style.display = "block";
      li.textContent = `…and ${groups.length - shown.length} more rows`;
      ul.appendChild(li);
    }
  },

  renderStatus() {
    const btn = $("#mRecord");
    if (!btn) return;
    const rec = this.status.recording;
    btn.classList.toggle("on", rec);
    $("span", btn).textContent = rec ? "Stop recording" : "Record";
    $("#mRecStatus").textContent = rec ? `${this.status.events} events · clicks on this panel are ignored` : "";
    const m = this.cur();
    if (m) {
      const playing = this.status.playing.includes(m.id);
      $("#mPlay").innerHTML = `${svgUse(playing ? "i-stop" : "i-play", 12)}<span>${playing ? "Stop" : "Play"}</span>`;
    }
  },

  // Key / mouse capture for the trigger and for "+ Key" steps.
  onKey(e) {
    if (!this.capturing) return;
    e.preventDefault(); e.stopPropagation();
    const m = this.cur();
    if (!m) return;
    if (e.key === "Escape" && this.capturing === "trigger" && !e.ctrlKey && !e.altKey) { this.capturing = null; this.renderTrigger(); return; }
    const isMod = [16, 17, 18, 91, 92].includes(e.keyCode);
    if (this.capturing === "trigger") {
      if (isMod) return;   // wait for the actual key
      m.trigger.vk = e.keyCode;
      m.trigger.mouse = 0;
      m.trigger.mods = (e.ctrlKey ? MOD.CTRL : 0) | (e.altKey ? MOD.ALT : 0) | (e.shiftKey ? MOD.SHIFT : 0) | (e.metaKey ? MOD.WIN : 0);
      this.capturing = null;
      this.save(); this.renderTrigger();
      return;
    }
    if (this.capturing === "step") {
      const [sc, ext] = SCAN[e.code] || [0, 0];
      m.steps.push({ k: "kd", vk: e.keyCode, sc, e: !!ext, w: m.steps.length ? 50 : 0 }, { k: "ku", vk: e.keyCode, sc, e: !!ext, w: 30 });
      this.capturing = null;
      this.save(); this.renderSteps();
      toast(`Added ${vkName(e.keyCode)}`, "ok");
    }
  },
  onMouseTrigger(e) {
    if (this.capturing !== "trigger") return false;
    const map = { 1: 3, 3: 4, 4: 5 };
    if (!map[e.button]) return false;
    e.preventDefault();
    const m = this.cur();
    m.trigger.mouse = map[e.button];
    m.trigger.vk = 0;
    m.trigger.mods = (e.ctrlKey ? MOD.CTRL : 0) | (e.altKey ? MOD.ALT : 0) | (e.shiftKey ? MOD.SHIFT : 0);
    this.capturing = null;
    this.save(); this.renderTrigger();
    return true;
  },

  wire() {
    $$("[data-template]").forEach((b) => b.addEventListener("click", () => this.create(b.dataset.template)));
    $("#mName").addEventListener("input", (e) => { const m = this.cur(); if (m) { m.name = e.target.value; this.save(); } });
    $("#mEnabled").addEventListener("change", (e) => { this.cur().enabled = e.target.checked; this.save(); });
    $("#mBlock").addEventListener("change", (e) => { this.cur().trigger.block = e.target.checked; this.save(); });
    $("#mScope").addEventListener("change", (e) => { this.cur().scope = e.target.value.trim().toLowerCase(); this.save(); });
    $("#mScopeLast").addEventListener("click", () => { this.cur().scope = state.game.lastExe; this.save(); this.renderEditor(); });
    $("#mCount").addEventListener("change", (e) => { this.cur().count = clamp(parseInt(e.target.value, 10) || 1, 1, 9999); this.save(); });
    $$("#mMode button").forEach((b) => b.addEventListener("click", () => { this.cur().trigger.mode = b.dataset.v; this.save(); this.renderEditor(); }));
    $$("#mRepeat button").forEach((b) => b.addEventListener("click", () => { this.cur().repeat = b.dataset.v; this.save(); this.renderEditor(); }));
    $("#mDelete").addEventListener("click", () => {
      const m = this.cur();
      if (!m) return;
      this.list = this.list.filter((x) => x.id !== m.id);
      this.sel = this.list[0] ? this.list[0].id : null;
      this.save(); this.render();
      toast(`Deleted ${m.name}`, "ok");
    });
    $("#mPlay").addEventListener("click", () => {
      const m = this.cur();
      if (!m) return;
      if (this.status.playing.includes(m.id)) send("macroStop");
      else if (!m.steps.length) toast("This macro has no steps yet", "error");
      else send("macroPlay", { id: m.id });
    });
    $("#mRecord").addEventListener("click", () => {
      if (this.status.recording) send("macroRecord", { start: false });
      else send("macroRecord", { start: true, mouse: $("#mRecMouse").checked });
    });
    $("#mTrigger").addEventListener("click", () => { this.capturing = this.capturing === "trigger" ? null : "trigger"; this.renderTrigger(); });
    $("#mTrigger").addEventListener("mousedown", (e) => this.onMouseTrigger(e));
    $("#mTrigger").addEventListener("auxclick", (e) => e.preventDefault());
    $$("[data-add]").forEach((b) => b.addEventListener("click", () => {
      const m = this.cur();
      if (!m) return;
      const w = m.steps.length ? 50 : 0;
      switch (b.dataset.add) {
        case "key": this.capturing = "step"; toast("Press the key to add", "info"); return;
        case "click": m.steps.push({ k: "md", b: 0, w }, { k: "mu", b: 0, w: 30 }); break;
        case "text": m.steps.push({ k: "tx", s: "", w }); break;
        case "wait": m.steps.push({ k: "dl", w: 250 }); break;
        case "scroll": m.steps.push({ k: "wh", d: -120, w }); break;
      }
      this.save(); this.renderSteps();
      if (b.dataset.add === "text") setTimeout(() => { const ins = $$("#mSteps [data-text]"); if (ins.length) ins[ins.length - 1].focus(); }, 0);
    }));
    document.addEventListener("keydown", (e) => this.onKey(e), true);
  },
};
window.Macros = Macros;
Macros.wire();
