// PicoGUS Setup: the web page of the PicoGUS web server, a text-mode setup
// program built from the Textmode design system (textmode/README.md).
//
// One 80x25 screen. Windows (menus, dialogs, message boxes) are kept on a
// stack and only the top one takes input, as in a DOS program:
//   arrows, Tab     move the bar           Enter   select / default button
//   letters         the command's hotkey   Esc     back
//   (Alt+letter while typing in a field)   F1 help, F2 save
// The mouse does the same by pointing and clicking.
"use strict";
(function () {
const D = document, $ = id => D.getElementById(id);
const root = $("dos"), body = $("body"), bar = $("bar"), descEl = $("desc"), verEl = $("ver"), fileEl = $("file");
const COLS = 80, ROWS = 23;

// ---- small helpers ---------------------------------------------------------

function h(tag, attrs, ...kids) {
  const e = D.createElement(tag);
  for (const k in attrs || {}) {
    const v = attrs[k];
    if (k === "class") e.className = v;
    else if (k === "style") e.style.cssText = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v === true) e.setAttribute(k, "");
    else if (v !== false && v != null) e.setAttribute(k, v);
  }
  for (const k of kids.flat(3)) if (k != null && k !== false) e.append(k);
  return e;
}
// "Select &Sound Card": the letter after & is the command's hotkey
function hot(s) {
  const i = s.indexOf("&");
  if (i < 0) return {nodes: [s], key: "", text: s};
  return {nodes: [s.slice(0, i), h("span", {class: "dos-hotkey"}, s[i + 1]), s.slice(i + 2)],
          key: s[i + 1].toLowerCase(), text: s.slice(0, i) + s.slice(i + 1)};
}
// label padded with dot leaders to a common column: "I/O port ....."
function lead(label, width) {
  const t = hot(label), n = width - t.text.length;
  return {nodes: [...t.nodes, n > 0 ? " " + ".".repeat(n - 1) : ""], key: t.key};
}
const pre = text => h("p", {class: "dos-pre"}, text);
const hex = v => (v ? v.toString(16).toUpperCase() + "h" : "off");
const sleep = ms => new Promise(r => setTimeout(r, ms));
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// ---- the card --------------------------------------------------------------

const MODES = {1: "&Gravis Ultrasound", 2: "&AdLib", 3: "&MPU-401", 4: "&Tandy and CMS", 5: "&Sound Blaster", 6: "&USB Only"};
const modeName = m => (MODES[m] ? hot(MODES[m]).text : "Mode " + m);
let st = null, feats = new Set(), online = false, quiet = false, warned = false;
const has = f => !f || feats.has(f);

async function api(path, form) {
  const o = form ? {method: "POST", headers: {"Content-Type": "application/x-www-form-urlencoded"},
                    body: new URLSearchParams(form).toString()} : {};
  const r = await fetch(path, o);
  let j = {};
  try { j = await r.json(); } catch (e) { /* no body */ }
  if (!r.ok) throw new Error(j.error || "The card answered with error " + r.status + ".");
  return j;
}
// a request whose failure is shown in an error box; null if it failed
async function call(path, form) {
  try {
    const j = await api(path, form || {});
    if (j.fw) status(j);
    return j;
  } catch (e) {
    await errorBox(e instanceof TypeError ? "No answer from the card." : e.message);
    return null;
  }
}
function upload(path, file, progress) {
  return new Promise((ok, fail) => {
    const x = new XMLHttpRequest();
    x.open("POST", path);
    x.setRequestHeader("Content-Type", "application/octet-stream");
    x.upload.onprogress = e => { if (e.lengthComputable) progress(Math.floor(e.loaded * 100 / e.total)); };
    x.onload = () => {
      let j = {};
      try { j = JSON.parse(x.responseText); } catch (e) { /* no body */ }
      if (x.status === 200) ok(j); else fail(new Error(j.error || "The card answered with error " + x.status + "."));
    };
    x.onerror = () => fail(new Error("The connection to the card was lost."));
    x.send(file);
  });
}

// ---- windows on the cell grid ----------------------------------------------

const stack = [];
const top = () => stack[stack.length - 1];
let cellW = 9, cellH = 16;

function measureCell() {
  const p = h("span", {style: "position:absolute;visibility:hidden;width:10ch;height:1em"});
  root.append(p);
  const r = p.getBoundingClientRect();
  if (r.width) { cellW = r.width / 10; cellH = r.height; }
  p.remove();
}
// Size the windows of a view in whole cells and centre them on the desktop,
// one below the other, one row apart
function place(v) {
  measureCell();
  const dims = v.wins.map(w => {
    w.style.setProperty("--x", 0);
    w.style.setProperty("--y", 0);
    if (!w.dataset.cols) w.style.removeProperty("--cols");
    const r = w.getBoundingClientRect();
    const cols = w.dataset.cols ? +w.dataset.cols : Math.min(COLS, Math.ceil(r.width / cellW - 0.05));
    w.style.setProperty("--cols", cols);
    // what the style sheet centres (title, buttons) may land between two
    // cells, where the bitmap font blurs: put it on the grid
    const snap = (e, inner) => {
      if (!e) return;
      e.style.marginLeft = e.style.paddingLeft = "";
      const used = Math.round((inner ? [...e.children].reduce((n, c) => n + c.getBoundingClientRect().width, 0) : e.getBoundingClientRect().width) / cellW);
      const gaps = inner ? 2 * (e.children.length - 1) : 0;
      e.style[inner ? "paddingLeft" : "marginLeft"] = Math.max(0, (cols - (inner ? 4 : 0) - used - gaps) >> 1) + "ch";
    };
    snap(w.querySelector(".dos-window__title"));
    const acts = w.querySelector(".dos-window__actions");
    if (acts) { acts.style.justifyContent = "flex-start"; snap(acts, true); }
    return {w, cols, rows: Math.round(w.getBoundingClientRect().height / cellH)};
  });
  let y = Math.max(0, (ROWS - dims.reduce((n, d) => n + d.rows + 1, -1)) >> 1);
  for (const d of dims) {
    d.w.style.setProperty("--x", Math.max(0, (COLS - d.cols) >> 1));
    d.w.style.setProperty("--y", y);
    y += d.rows + 1;
  }
}
function show(v) {
  const prev = top();
  if (prev) {
    prev.last = D.activeElement;
    for (const w of prev.wins) { w.inert = true; if (!v.modal) w.hidden = true; }
  }
  stack.push(v);
  for (const w of v.wins) {
    w.classList.add("win");
    w.style.zIndex = stack.length;
    body.append(w);
  }
  place(v);
  hints();
  focus(v.first && v.first() || v.wins[0].querySelector("[data-first]") || stops(v)[0], true);
  return v;
}
// Close a view (and anything opened on top of it)
function close(v) {
  const i = stack.indexOf(v || top());
  if (i < 0) return;
  while (stack.length > i) {
    const t = stack.pop();
    for (const w of t.wins) w.remove();
    if (t.closed) t.closed();
  }
  const p = top();
  if (!p) return;
  for (const w of p.wins) { w.inert = false; w.hidden = false; }
  place(p);
  hints();
  focus(p.last && p.last.isConnected && !p.last.disabled ? p.last : stops(p)[0]);
}

// The controls of a view that can take the focus, in order. A group of radio
// buttons counts once, as its checked button.
function stops(v) {
  const out = [], seen = new Set();
  for (const w of v.wins) {
    for (const e of w.querySelectorAll("button,input,[tabindex='0']")) {
      if (e.disabled || e.getAttribute("aria-disabled") === "true" || e.closest("[hidden]")) continue;
      if (e.type === "radio") {
        if (seen.has(e.name)) continue;
        seen.add(e.name);
        out.push(w.querySelector('input[name="' + e.name + '"]:checked:not(:disabled)') || e);
      } else out.push(e);
    }
  }
  return out;
}
const isField = e => !!e && e.matches && e.matches("input.dos-field:not([readonly])");
function focus(e, select) {
  if (!e) return;
  e.focus({preventScroll: true});
  if (select && isField(e)) e.select();
  if (e.type === "radio" && e.closest(".dos-listbox")) reveal(e);
}

// The bar: one element carries it at a time, wherever the focus is
let marked = [];
function mark(t) {
  for (const [e, c] of marked) e.classList.remove(c);
  marked = [];
  const add = (e, c) => { if (e) { e.classList.add(c); marked.push([e, c]); } };
  if (t.matches(".dos-menu__item")) add(t, "is-selected");
  else if (t.matches(".dos-button")) add(t, "is-focused");
  else {
    if (t.matches(".dos-choice__input")) add(t.closest(".dos-choice"), "is-focused");
    if (t.matches("input.dos-field[readonly]")) add(t, "is-focused");
    const row = t.closest(".dos-row");
    add(row && row.querySelector(".dos-label"), "is-focused");
  }
}
root.addEventListener("focusin", e => { mark(e.target); describe(); });

// ---- status line -----------------------------------------------------------

const KEYNAMES = {"Enter": "Enter", "Esc": "Escape", "F1": "F1", "F2": "F2", "Tab": "Tab"};
let message = "", messageTimer = 0;

function hints() {
  const v = top();
  for (const e of bar.querySelectorAll(".hint")) e.remove();
  if (!v) return describe();
  for (const [key, label] of v.keys || []) {
    const name = KEYNAMES[key];
    const kids = [h("kbd", {class: "dos-key"}, key), "=" + label];
    descEl.before(name
      ? h("button", {class: "hint", tabindex: -1, type: "button", onclick: () => press(name)}, kids)
      : h("span", {class: "hint"}, kids));
  }
  describe();
}
// The right end of the status line: a message, or what the bar is on
function describe() {
  const v = top(), a = D.activeElement;
  let text = message || (a && a.dataset && a.dataset.desc) || (v && v.desc) || "";
  let used = 2;
  for (const e of bar.querySelectorAll(".hint")) used += e.textContent.length + 2;
  const room = COLS - used;
  if (text.length > room) text = room > 3 ? text.slice(0, room - 3) + "..." : "";
  descEl.textContent = text;
}
function say(text) {
  message = text;
  clearTimeout(messageTimer);
  messageTimer = setTimeout(() => { message = ""; describe(); }, 6000);
  describe();
}

// ---- keyboard --------------------------------------------------------------

function press(key) { onKey({key, code: "", preventDefault() {}, stopPropagation() {}}); }

function onKey(e) {
  if (e.ctrlKey || e.metaKey || e.isComposing) return;
  const v = top();
  if (!v) return;
  let a = D.activeElement;
  if (!a || !v.wins.some(w => w.contains(a))) { a = stops(v)[0]; focus(a); if (!a) return; }
  const k = e.key, done = () => { e.preventDefault(); e.stopPropagation(); };

  if (k === "F1") { done(); if (!v.modal) helpBox(); return; }
  if (k === "F2") { done(); if (v.menu) saveSettings(); return; }
  if (k === "Escape") { done(); if (v.cancel) v.cancel(); else if (stack.length > 1) close(v); return; }
  if (v.key && v.key(e, a)) { done(); return; }

  const s = stops(v);
  const cur = s.findIndex(x => x === a || (a.type === "radio" && x.type === "radio" && x.name === a.name));
  const wrap = (list, n) => list[((n % list.length) + list.length) % list.length];
  const acts = s.filter(x => x.closest(".dos-window__actions"));
  const inActs = acts.includes(a);
  const group = a.type === "radio" ? [...v.wins[0].querySelectorAll('input[name="' + a.name + '"]:not(:disabled)')] : null;
  const inList = !!a.closest(".dos-listbox");
  const slider = a.matches(".slider");
  const choose = r => { r.checked = true; focus(r); r.dispatchEvent(new Event("change", {bubbles: true})); };

  switch (k) {
  case "Tab":
    done();
    focus(wrap(s, cur + (e.shiftKey ? -1 : 1)), true);
    return;
  case "ArrowDown": case "ArrowUp": {
    done();
    const d = k === "ArrowDown" ? 1 : -1;
    if (inList) { choose(group[clamp(group.indexOf(a) + d, 0, group.length - 1)]); return; }
    // the row of buttons at the bottom counts as one line
    const lines = s.filter(x => !acts.includes(x));
    if (!lines.length) { focus(wrap(acts, acts.indexOf(a) + d)); return; }
    if (acts.length) lines.push(acts.find(b => b.matches(".dos-button--default")) || acts[0]);
    const at = inActs ? lines.length - 1 : lines.findIndex(x => x === s[cur]);
    focus(wrap(lines, at + d), true);
    return;
  }
  case "ArrowLeft": case "ArrowRight": {
    if (isField(a)) return;     // moves the cursor in the field
    done();
    const d = k === "ArrowRight" ? 1 : -1;
    if (slider) a.setValue(a.value + d * a.step);
    else if (group && !inList) choose(wrap(group, group.indexOf(a) + d));
    else if (inActs) focus(wrap(acts, acts.indexOf(a) + d));
    else if (v.menu && d > 0 && a.dataset.sub) a.click();
    else if (v.menu && d < 0 && stack.length > 1) close(v);
    return;
  }
  case "Home": case "End": case "PageUp": case "PageDown": {
    if (isField(a)) return;
    done();
    const first = k === "Home" || k === "PageUp";
    if (slider) a.setValue(k === "Home" ? 0 : k === "End" ? 100 : a.value + (first ? 10 : -10));
    else if (inList) {
      const i = group.indexOf(a), page = k === "PageUp" ? i - 8 : k === "PageDown" ? i + 8 : first ? 0 : group.length - 1;
      choose(group[clamp(page, 0, group.length - 1)]);
    } else if (v.menu) focus(first ? s[0] : s[s.length - 1]);
    return;
  }
  case "Enter": {
    done();
    if (a.matches("button")) { a.click(); return; }
    if (a.dataset.enter) { v.wins[0].querySelector(a.dataset.enter).click(); return; }
    const def = v.wins[v.wins.length - 1].querySelector(".dos-button--default:not(:disabled)");
    if (def) { focus(def); def.click(); }
    return;
  }
  case " ":
    if (a.matches("button")) { done(); a.click(); }
    return;         // check boxes and radio buttons take the space themselves
  }

  // Hotkeys: the highlighted letter of a command. While the cursor is in a
  // text field letters are typed into it, and Alt+letter is the hotkey.
  let ch = k.length === 1 ? k.toLowerCase() : "";
  if (e.altKey && /^(Key[A-Z]|Digit\d)$/.test(e.code)) ch = e.code.slice(-1).toLowerCase();
  if (!ch || (isField(a) && !e.altKey)) return;
  for (const w of v.wins) {
    const t = w.querySelector('[data-hk="' + (window.CSS && CSS.escape ? CSS.escape(ch) : ch) + '"]');
    if (!t || t.disabled || t.getAttribute("aria-disabled") === "true") continue;
    done();
    if (t.type === "radio") focus(s.find(x => x.type === "radio" && x.name === t.name));
    else { focus(t, true); if (t.matches("button,input[type=checkbox]")) t.click(); }
    return;
  }
}
D.addEventListener("keydown", e => { root.classList.add("kbd"); onKey(e); });

// ---- mouse -----------------------------------------------------------------

D.addEventListener("mousemove", e => {
  root.classList.remove("kbd");
  const v = top(), t = e.target.closest && e.target.closest("button,.dos-choice,.slider");
  if (!v || !t || !v.wins.some(w => w.contains(t))) return;
  // the bar follows the pointer, so there is never a second one
  const f = t.matches(".dos-choice") ? t.querySelector("input") : t;
  if (f !== D.activeElement && !f.disabled && f.getAttribute("aria-disabled") !== "true") f.focus({preventScroll: true});
});
// a click on the desktop or on plain text must not take the focus away
root.addEventListener("mousedown", e => {
  if (!e.target.closest("input,label,.slider") && !e.target.closest("button:not(.hint)")) e.preventDefault();
});
window.addEventListener("focus", () => {
  const v = top();
  if (v && (!D.activeElement || D.activeElement === D.body)) focus(v.last || stops(v)[0]);
});
root.addEventListener("focusout", e => { const v = top(); if (v) v.last = e.target; });

// ---- screen size -----------------------------------------------------------

// The font is a bitmap: only whole multiples of its 9x16 cell stay sharp
function fit() {
  const n = clamp(Math.floor(Math.min(window.innerWidth / 720, window.innerHeight / 400)), 1, 3);
  root.classList.remove("screen-1x", "screen-2x", "screen-3x");
  root.classList.add("screen-" + n + "x");
}
window.addEventListener("resize", fit);
fit();

// ---- building blocks -------------------------------------------------------

function win(title, variant, cols, kids) {
  return h(variant === "menu" ? "nav" : "section",
           {class: "dos-window" + (variant ? " dos-window--" + variant : ""), "data-cols": cols || null,
            style: "min-inline-size:" + (title.length + 8) + "ch"},
           title ? h("h2", {class: "dos-window__title"}, title) : null, kids);
}
function button(label, run, def, desc) {
  const t = hot(label);
  return h("button", {class: "dos-button" + (def ? " dos-button--default" : ""), type: "button",
                      "data-hk": t.key, "data-desc": desc, onclick: run}, t.nodes);
}
const actions = (...buttons) => [h("div", {class: "dos-window__rule"}), h("div", {class: "dos-window__actions"}, buttons)];

// A menu window. items: {label, run, sub, end, desc, off} or "-" for a rule
function menu(title, items, opt) {
  let cols = title.length + 8;
  const list = h("ul", {class: "dos-menu", role: "menu"}, items.map(it => {
    if (it === "-") return h("li", {class: "dos-menu__rule", role: "separator"});
    const t = hot(it.label), end = it.end || (it.sub ? "►" : "");
    cols = Math.max(cols, t.text.length + (end ? end.length + 2 : 0) + 4);
    return h("li", null, h("button", {class: "dos-menu__item", role: "menuitem", type: "button", "data-hk": t.key,
      "data-desc": it.desc, "data-sub": it.sub ? 1 : null, "aria-disabled": it.off ? "true" : null,
      "data-first": it.first ? 1 : null, onclick: it.off ? null : it.run},
      t.nodes, end ? h("span", {class: "dos-menu__item-end"}, end) : null));
  }));
  const v = Object.assign({wins: [win(title, "menu", Math.max(cols, 30), list)], menu: true,
    keys: [["↑↓", "Move"], ["Enter", "Select"], ["Esc", "Back"]]}, opt);
  return v;
}
// A dialog: rows of controls and a row of buttons
function dialog(title, rows, buttons, opt) {
  return Object.assign({wins: [win(title, opt && opt.variant, opt && opt.cols, [rows, actions(buttons)])],
    keys: [["Tab", "Next"], ["Enter", "OK"], ["Esc", "Cancel"]]}, opt);
}
// Message boxes: resolve when the box is closed
function box(title, text, buttons, variant) {
  return new Promise(done => {
    const v = {modal: true, keys: [["Enter", "Select"], ["Esc", "Cancel"]], closed: () => done(v.answer)};
    const lines = text.split("\n"), cols = Math.max(title.length + 8, ...lines.map(l => l.length + 4), 30);
    v.wins = [win(title, variant, cols + (cols & 1), [h("p", {class: "dos-window__text"}, text),
      actions(buttons.map(([label, answer], i) => button(label, () => { v.answer = answer; close(v); }, i === 0)))])];
    show(v);
  });
}
const confirmBox = (title, text) => box(title, text, [["&Yes", true], ["&No", false]]);
const infoBox = (title, text) => box(title, text, [["&OK", true]]);
const errorBox = text => box("Error", text, [["&OK", true]], "error");
// A box that stays until the work is done: text, optionally a progress bar
function waitBox(title, text, withBar) {
  const line = h("p", {class: "dos-pre"}, text);
  const fill = h("div", {class: "dos-progress", role: "progressbar", "aria-valuemin": 0, "aria-valuemax": 100,
                         style: "--cols:48;--value:0"}, h("span", {class: "dos-progress__fill"}));
  // an even number of characters, so the centred text stays on the cell grid
  const pct2 = pct => ("" + pct).padStart(3) + "% complete ";
  const label = h("p", {class: "dos-progress__label"}, pct2(0));
  const v = {modal: true, keys: [], cancel() {}, wins: [win(title, "", 52, withBar
    ? [line, pre(" "), fill, label] : [line, h("p", {class: "dos-pre"}, "Please wait ", h("span", {class: "dos-blink"}, "_"))])]};
  v.set = (pct, text2) => {
    fill.style.setProperty("--value", pct);
    fill.firstChild.style.inlineSize = Math.floor(pct * 48 / 100) + "ch";
    fill.setAttribute("aria-valuenow", pct);
    label.textContent = pct2(pct);
    if (text2) line.textContent = text2;
  };
  return show(v);
}

// Rows of a dialog. Each has value(), which throws an Error for a bad entry.
function fieldRow(label, width, value, cols, unit, check, attrs) {
  const l = lead(label, width);
  const input = h("input", Object.assign({class: "dos-field", style: "--cols:" + cols, value, "data-hk": l.key,
    maxlength: cols, spellcheck: "false", autocomplete: "off", autocapitalize: "off"}, attrs));
  const row = h("div", {class: "dos-row"}, h("label", {class: "dos-label"}, l.nodes), input, unit ? h("span", null, unit) : null);
  row.value = () => check(input.value.trim(), hot(label).text);
  row.input = input;
  return row;
}
function checkRow(label, checked, desc) {
  const t = hot(label);
  const input = h("input", {class: "dos-choice__input", type: "checkbox", checked: !!checked, "data-hk": t.key, "data-desc": desc});
  const row = h("div", null, h("label", {class: "dos-choice"}, input, h("span", {class: "dos-choice__box"}), h("span", null, t.nodes)));
  row.value = () => (input.checked ? 1 : 0);
  row.input = input;
  return row;
}
let radioId = 0;
function radioRow(label, width, options, value, column, changed) {
  const l = lead(label, width), name = "r" + (++radioId);
  const group = h("div", {class: "dos-choices " + (column ? "dos-choices--col" : "dos-choices--row")},
    options.map(([v, text], i) => h("label", {class: "dos-choice"},
      h("input", {class: "dos-choice__input", type: "radio", name, value: v, checked: v === value,
                  "data-hk": i ? null : l.key, onchange: changed ? () => changed(v) : null}),
      h("span", {class: "dos-choice__box"}), text)));
  const row = h("div", {class: "dos-row"}, h("span", {class: "dos-label"}, l.nodes), group);
  row.value = () => { const c = group.querySelector("input:checked"); return c ? +c.value : value; };
  return row;
}
// A value from 0 to 100 as a bar of 50 cells
function sliderRow(label, width, value, changed) {
  const l = lead(label, width);
  const out = h("span", {class: "slider-value"});
  const s = h("div", {class: "dos-progress slider", role: "slider", tabindex: 0, "aria-valuemin": 0, "aria-valuemax": 100,
                      "data-hk": l.key, style: "--cols:50"}, h("span", {class: "dos-progress__fill"}));
  s.step = 2;
  s.setValue = (v, silent) => {
    v = clamp(Math.round(v), 0, 100);
    if (v === s.value) return;
    s.value = v;
    s.style.setProperty("--value", v);
    // whole cells, set here: the style sheet's rounding can lose the last one
    s.firstChild.style.inlineSize = (v >> 1) + "ch";
    s.setAttribute("aria-valuenow", v);
    out.textContent = v + "%";
    if (!silent) changed(v);
  };
  const point = e => { const r = s.getBoundingClientRect(); s.setValue(Math.round((e.clientX - r.left) / r.width * 50) * 2); };
  s.addEventListener("pointerdown", e => { s.setPointerCapture(e.pointerId); s.focus(); point(e); });
  s.addEventListener("pointermove", e => { if (s.hasPointerCapture(e.pointerId)) point(e); });
  s.setValue(value, true);
  const row = h("div", {class: "dos-row"}, h("span", {class: "dos-label"}, l.nodes), s, out);
  row.slider = s;
  return row;
}
// A list to pick one row from, with a scroll bar
function listBox(cols, rows) {
  const items = h("ul", {class: "dos-listbox__items"});
  const thumb = h("span", {class: "dos-scrollbar__thumb", style: "--pos:0"});
  const el = h("div", {class: "dos-listbox", style: "--cols:" + cols + ";--rows:" + rows}, items,
               h("div", {class: "dos-scrollbar", "aria-hidden": "true"}, thumb));
  const name = "l" + (++radioId);
  el.fill = (names, current, empty) => {
    items.replaceChildren(...(names.length ? names : [empty]).map((n, i) => h("li", null, h("label", {class: "dos-listbox__item"},
      h("input", {type: "radio", name, value: i + 1, checked: !!names.length && i + 1 === (current || 1), disabled: !names.length}), n))));
    items.scrollTop = 0;
    const c = items.querySelector("input:checked");
    if (c) reveal(c);
  };
  el.value = () => { const c = items.querySelector("input:checked"); return c ? +c.value : 0; };
  el.input = () => items.querySelector("input:checked") || items.querySelector("input");
  items.addEventListener("wheel", e => {
    e.preventDefault();
    items.scrollTop = Math.round((items.scrollTop + Math.sign(e.deltaY) * 3 * cellH) / cellH) * cellH;
    scrolled(items);
  }, {passive: false});
  return el;
}
function scrolled(items) {
  const max = items.scrollHeight - items.clientHeight, thumb = items.parentNode.querySelector(".dos-scrollbar__thumb");
  thumb.style.setProperty("--pos", max > 0 ? Math.round(items.scrollTop * 100 / max) : 0);
}
// Scroll a list so that the chosen row is in view, in whole rows
function reveal(input) {
  const items = input.closest(".dos-listbox__items"), li = input.closest("li");
  const rowTop = li.offsetTop - items.offsetTop;
  if (rowTop < items.scrollTop) items.scrollTop = rowTop;
  else if (rowTop + li.offsetHeight > items.scrollTop + items.clientHeight) items.scrollTop = rowTop + li.offsetHeight - items.clientHeight;
  scrolled(items);
}

// ---- screens ---------------------------------------------------------------

function mainMenu() {
  const v = menu("Main Menu", [
    {label: "&Volume", sub: 1, run: volume, desc: "Set the volume of each sound source.", off: !sources().length},
    has("multifw") && {label: "Card &Mode", sub: 1, run: cardMode, desc: "Choose the sound card to emulate."},
    {label: "&Settings", sub: 1, run: settingsMenu, desc: "Change ports, IRQ, DMA and options."},
    has("cd") && {label: "&CD-ROM Image", sub: 1, run: cdImage, desc: "Load a CD image from the USB drive."},
    has("mpu") && {label: "M&IDI and mt32-pi", sub: 1, run: midiMenu, desc: "Control the synth on the MIDI port."},
    {label: "&WiFi Network", run: wifi, desc: "Join another wireless network."},
    {label: "&Firmware Update", run: firmware, desc: "Install new firmware from a file."},
    {label: "Card I&nformation", run: information, desc: "Show the state of the card."},
    {label: "&Display", run: display, desc: "Choose the colours of this screen."},
    "-",
    {label: "S&ave Settings", run: saveSettings, desc: "Keep the settings after a restart."},
    {label: "&Restart Card", run: restart, desc: "Restart the card. Sound stops briefly."},
  ].filter(Boolean), {keys: [["↑↓", "Move"], ["Enter", "Select"], ["F1", "Help"], ["F2", "Save"]]});
  const lines = h("div");
  v.wins.push(win("Card Status", "single", 62, lines));
  v.paint = () => {
    const s = st.s, n = st.net, out = [];
    const res = {1: () => hex(s.gusport), 2: () => hex(s.oplport), 3: () => hex(s.mpuport),
                 4: () => "Tandy " + hex(s.tandyport) + "  CMS " + hex(s.cmsport),
                 5: () => hex(s.sbport) + "  IRQ " + s.sbirq + "  DMA " + s.sbdma + "  FM " + hex(s.oplport),
                 6: () => "Joystick " + (s.joy ? "on" : "off")}[st.mode];
    out.push("Mode   " + modeName(st.mode) + (res ? "  " + res() : ""));
    out.push("WiFi   " + (n.ip && n.ip !== "0.0.0.0" ? (n.ssid || "-") + (n.rssi ? "  " + n.rssi + " dBm" : "") + "  " + n.ip : "Not connected"));
    if (has("mpu") && st.mode !== 3) out.push("MIDI   MPU-401 at " + hex(s.mpuport));
    if (st.cd) out.push("CD     " + (st.cd.image ? st.cd.image.toUpperCase() : "No image loaded"));
    lines.replaceChildren(...out.map(t => pre(t.slice(0, 58))));
  };
  v.paint();
  return show(v);
}

const VOLUMES = [["mainvol", "&Main", null], ["gusvol", "&GUS", "gus"], ["sbvol", "&SB voice", "sb"],
                 ["oplvol", "&FM music", "opl"], ["cdvol", "C&D audio", "cd"], ["psgvol", "&PSG", "psg"]];
// the sound sources of this mode (the PSG volume is for Tandy and CMS)
const sources = () => VOLUMES.filter(x => x[2] && (has(x[2]) || (x[2] === "psg" && (has("tandy") || has("cms")))));
function volume() {
  let timer = 0;
  const pending = {};
  const send = () => { const f = Object.assign({}, pending); for (const k in pending) delete pending[k]; api("/api/set", f).then(status, () => {}); };
  const rows = [VOLUMES[0], ...sources()].map(([key, label]) => sliderRow(label, 11, st.s[key], v => { pending[key] = v; clearTimeout(timer); timer = setTimeout(send, 150); }));
  const v = dialog("Volume", [rows, pre(" "), pre("Changes are heard at once. Save Settings keeps them.")],
    [button("C&lose", () => close(v), true)],
    {keys: [["←→", "Adjust"], ["↑↓", "Move"], ["Esc", "Close"]]});
  show(v);
}

function cardMode() {
  const v = menu("Card Mode", Object.keys(MODES).map(m => ({label: MODES[m], end: +m === st.mode ? "√" : "",
    first: +m === st.mode, desc: +m === st.mode ? "The card is in this mode now." : "Restart the card in this mode.",
    run: () => switchMode(+m)})));
  show(v);
}
function switchMode(m) {
  const keep = checkRow("&Make it the default mode", true);
  const v = dialog("Card Mode", [h("p", {class: "dos-window__text"},
      (m === st.mode ? "Restart in " : "Switch to ") + modeName(m) + " mode?\nThe card restarts and sound stops for a few seconds."), pre(" "), keep],
    [button("&Yes", async () => {
      const save = keep.value();
      close(v);
      if (await call("/api/mode", {mode: m, save})) waitRestart("Starting " + modeName(m) + " mode.");
    }, true), button("&No", () => close(v))], {modal: true, cols: 58});
  show(v);
  focus(v.wins[0].querySelector(".dos-button--default"));
}

// key, label, feature, kind, options, unit
const P = "port", B = "bool", N = "num", S = "sel";
const GROUPS = [
  ["&General", "General", [["joy", "USB &joystick on the game port", "joy", B]]],
  ["Gravis &Ultrasound", "Gravis Ultrasound", [["gusport", "&Port", "gus", P], ["gusbuf", "Audio &buffer", "gus", N, [1, 256], "samples"],
    ["gusdma", "&DMA interval", "gus", N, [0, 255], "µs, 0 = set by the program"], ["gus44k", "&Fixed 44.1 kHz output", "gus", B]]],
  ["&Sound Blaster", "Sound Blaster", [["sbport", "&Port", "sb", P], ["sbirq", "&IRQ", "sb", S, [2, 3, 5, 7, 9, 10].map(x => [x, "" + x])],
    ["sbdma", "&DMA", "sb", S, [0, 1, 3].map(x => [x, "" + x])],
    ["sbtype", "&Type", "sb", S, [[1, "1.x"], [2, "Pro 1"], [3, "2.0"], [4, "Pro 2"], [6, "16"]]],
    ["sblockmixer", "&Lock mixer", "sb", S, [[0, "Off"], [1, "All but voice"], [2, "All"]]],
    ["sbfixtc", "&Fix time constant", "sb", B]]],
  ["&AdLib", "AdLib", [["oplport", "&Port", "opl", P], ["oplwait", "&Wait on FM register writes", "opl", B]]],
  ["&MPU-401", "MPU-401", [["mpuport", "&Port", "mpu", P], ["mpudelay", "&SysEx delay", "mpu", B],
    ["mpufake", "&Fake all notes off", "mpu", B], ["mpuwait", "&Extra wait on busy status (fast PCs)", "mpu", B]]],
  ["&Tandy and CMS", "Tandy and CMS", [["tandyport", "&Tandy port", "tandy", P], ["cmsport", "C&MS port", "cms", P]]],
  ["Serial M&ouse", "Serial Mouse", [["mouseport", "&Port", "mouse", S, [[0, "Off"], [1016, "COM1"], [760, "COM2"], [1000, "COM3"], [744, "COM4"]]],
    ["mouseproto", "P&rotocol", "mouse", S, [[0, "Microsoft"], [1, "Logitech"], [2, "IntelliMouse"], [3, "Mouse Systems"]], "column"],
    ["mouserate", "Report r&ate", "mouse", N, [20, 200], "Hz"], ["mousesen", "&Sensitivity", "mouse", N, [0, 1024], "256 = 100%"]]],
  ["&CD-ROM", "CD-ROM", [["cdport", "&Port", "cd", P], ["cdauto", "&Auto-advance the image", "cd", B]]],
];
function settingsMenu() {
  const groups = GROUPS.filter(g => g[2].some(x => has(x[2])));
  show(menu("Settings", [...groups.map(g => ({label: g[0], run: () => settings(g), desc: "Change the " + g[1] + " settings."})), "-",
    {label: "Reset to &Defaults", run: defaults, desc: "Load the settings the card came with."}]));
}
function settings(g) {
  const sets = g[2].filter(x => has(x[2]));
  const width = 2 + Math.max(0, ...sets.filter(x => x[3] !== B).map(x => hot(x[1]).text.length));
  const rows = sets.map(([key, label, , kind, opt, unit]) => {
    const now = st.s[key];
    let row;
    if (kind === B) row = checkRow(label, now);
    else if (kind === P) row = fieldRow(label, width, now ? now.toString(16).toUpperCase() : "0", 4, "h, 0 = off", (t, name) => {
      if (!/^[0-9a-f]{1,3}$/i.test(t)) throw new Error(name + ": enter a port in hex, such as 220.\nEnter 0 to switch it off.");
      return parseInt(t, 16);
    });
    else if (kind === N) row = fieldRow(label, width, "" + now, 5, unit, (t, name) => {
      if (!/^\d+$/.test(t) || +t < opt[0] || +t > opt[1]) throw new Error(name + ": enter a number from " + opt[0] + " to " + opt[1] + ".");
      return +t;
    });
    else row = radioRow(label, width, opt.some(o => o[0] === now) ? opt : [...opt, [now, "" + now]], now, unit === "column");
    row.key = key;
    return row;
  });
  const v = dialog(g[1], rows, [button("&OK", async () => {
    const changed = {};
    try {
      for (const r of rows) { const val = r.value(); if (val !== st.s[r.key]) changed[r.key] = val; }
    } catch (e) { errorBox(e.message); return; }
    if (Object.keys(changed).length && !await call("/api/set", changed)) return;
    close(v);
    if (Object.keys(changed).length) say("Applied. Save Settings keeps it.");
  }, true), button("&Cancel", () => close(v))]);
  show(v);
}
async function defaults() {
  if (!await confirmBox("Reset to Defaults", "Load the default settings?\nThe WiFi network is kept.")) return;
  if (await call("/api/defaults")) say("Defaults loaded. Not saved yet.");
}
async function saveSettings() {
  if (!await confirmBox("Save Settings", "Save the settings to the card?\nThey are then used after every restart.")) return;
  if (await call("/api/save")) say("Settings saved.");
}
async function restart() {
  if (!await confirmBox("Restart Card", "Restart the card?\nSound stops for a few seconds.")) return;
  if (await call("/api/reboot")) waitRestart("Restarting the card.");
}

function cdImage() {
  const list = listBox(56, 10);
  let names = [];
  const load = index => call("/api/cd/load", {index}).then(j => { if (j) { close(v); say(index ? "Loading " + names[index - 1].toUpperCase() + "." : "Image ejected."); } });
  const v = dialog("CD-ROM Image", [list], [
    button("&Load", () => { if (list.value() && names.length) load(list.value()); }, true, "Load the highlighted image."),
    button("&Eject", () => load(0), false, "Unload the image."),
    button("&Refresh", () => read(), false, "Read the USB drive again."),
    button("&Close", () => close(v))], {keys: [["↑↓", "Move"], ["Enter", "Load"], ["Esc", "Close"]]});
  async function read() {
    list.fill([], 0, "Reading the USB drive.");
    for (let i = 0; i < 40 && stack.includes(v); i++) {
      let j;
      try { j = await api("/api/cd"); } catch (e) { list.fill([], 0, "No images."); errorBox(e.message); return; }
      if (j.busy) { await sleep(500); continue; }
      names = j.images;
      list.fill(names, j.current, "No images on the USB drive.");
      if (top() === v) focus(names.length ? list.input() : stops(v)[0]);
      if (j.error) errorBox(j.error); else if (j.truncated) say("The list is longer than shown.");
      return;
    }
    if (stack.includes(v)) { list.fill([], 0, "No answer from the USB drive."); }
  }
  list.addEventListener("dblclick", () => { if (names.length) load(list.value()); });
  show(v);
  read();
}

function sendMidi(bytes, what) {
  const hx = typeof bytes === "string" ? bytes : bytes.map(b => b.toString(16).padStart(2, "0")).join("");
  return call("/api/midi", {hex: hx}).then(j => { if (j) say(st && st.midiHeld ? "Waits: MPU-401 intelligent mode." : what || "Sent."); return j; });
}
// one entry: ask for a value, then run with it
function ask(title, label, value, cols, check, run, attrs) {
  const row = fieldRow(label, hot(label).text.length + 2, value, cols, "", check, attrs);
  const v = dialog(title, [row], [button("&OK", async () => {
    let val;
    try { val = row.value(); } catch (e) { errorBox(e.message); return; }
    close(v);
    run(val);
  }, true), button("&Cancel", () => close(v))], {modal: true});
  show(v);
}
function midiMenu() {
  const m = (label, bytes, desc) => ({label, desc, run: () => sendMidi(bytes, hot(label).text + ": sent.")});
  const sub = (title, items) => () => show(menu(title, items));
  show(menu("MIDI and mt32-pi", [
    m("Use &MT-32 Synth", "F07D0300F7", "Switch mt32-pi to MT-32 emulation."),
    m("Use &SoundFont Synth", "F07D0301F7", "Switch mt32-pi to its SoundFont."),
    {label: "MT-32 &ROM Set", sub: 1, desc: "Choose the MT-32 ROM set.", run: sub("MT-32 ROM Set", [
      m("MT-32 (&Old)", "F07D0100F7"), m("MT-32 (&New)", "F07D0101F7"), m("&CM-32L", "F07D0102F7")])},
    {label: "Select Sound&Font", desc: "Choose a SoundFont by number.", run: () =>
      ask("Select SoundFont", "SoundFont &number", "0", 3, t => {
        if (!/^\d+$/.test(t) || +t > 127) throw new Error("Enter a number from 0 to 127.");
        return +t;
      }, n => sendMidi([0xF0, 0x7D, 0x02, n, 0xF7], "SoundFont " + n + " selected."))},
    {label: "Re&versed Stereo", sub: 1, desc: "Swap left and right of the MT-32.", run: sub("Reversed Stereo", [
      m("&On", "F07D0401F7"), m("O&ff", "F07D0400F7")])},
    {label: "&Display Text", desc: "Show text on the MT-32 display.", run: () =>
      ask("Display Text", "&Text", "", 20, t => t, t => {
        const d = [...t.padEnd(20, " ").slice(0, 20)].map(c => { const x = c.charCodeAt(0); return x >= 32 && x < 127 ? x : 63; });
        const data = [0x20, 0, 0, ...d], sum = data.reduce((a, b) => a + b, 0);
        sendMidi([0xF0, 0x41, 0x10, 0x16, 0x12, ...data, (128 - sum % 128) % 128, 0xF7], "Text sent to the display.");
      })},
    "-",
    m("&GM Reset", "F07E7F0901F7", "Send General MIDI System On."),
    m("GS R&eset", "F04110421240007F0041F7", "Send the Roland GS reset."),
    m("M&T-32 Reset", "F0411016127F00000001F7", "Reset all MT-32 parameters."),
    {label: "&All Notes Off", desc: "Silence hanging notes on all channels.", run: () => {
      const a = [];
      for (let c = 0; c < 16; c++) a.push(0xB0 | c, 0x7B, 0, 0xB0 | c, 0x78, 0);
      sendMidi(a, "All notes off: sent.");
    }},
    {label: "Send Ra&w Bytes", desc: "Send MIDI bytes typed in hex.", run: () =>
      ask("Send Raw Bytes", "MIDI &bytes in hex", "", 40, t => {
        const x = t.replace(/[\s,:]/g, "");
        if (!x || x.length % 2 || /[^0-9a-f]/i.test(x)) throw new Error("Enter whole bytes in hex, such as 90 3C 7F.");
        return x;
      }, x => sendMidi(x), {maxlength: 120})},
    {label: "Send S&ysEx File", desc: "Send a .SYX file to the MIDI port.", run: sysexFile},
    "-",
    m("Re&boot mt32-pi", "F07D00F7", "Restart the mt32-pi."),
  ]));
}
function pickFile(accept) {
  return new Promise(done => {
    fileEl.value = "";
    fileEl.accept = accept;
    fileEl.onchange = () => done(fileEl.files[0] || null);
    fileEl.oncancel = () => done(null);
    fileEl.click();
  });
}
async function sysexFile() {
  const f = await pickFile(".syx");
  if (!f) return;
  const name = f.name.toUpperCase(), w = waitBox("Sending", "Sending " + name.slice(0, 30) + " to the MIDI port", true);
  quiet = true;
  try {
    const j = await upload("/api/upload/syx", f, p => w.set(p));
    close(w);
    infoBox("SysEx File", j.messages + " SysEx message" + (j.messages === 1 ? "" : "s") + " sent from " + name.slice(0, 30) + ".");
  } catch (e) { close(w); errorBox(e.message); }
  quiet = false;
}

function firmware() {
  let file = null;
  const pico = st.chip === "RP2350" ? "Pico 2 W" : "Pico W", uf2 = st.chip === "RP2350" ? "PG2350W.UF2" : "PICOGUSW.UF2";
  const row = fieldRow("&File", 8, "", 36, "", t => t, {readonly: true, "data-enter": "[data-browse]", placeholder: "No file chosen"});
  const browse = button("&Browse", async () => {
    const f = await pickFile(".uf2");
    if (f) { file = f; row.input.value = f.name.toUpperCase().slice(0, 36); focus(v.wins[0].querySelector(".dos-button--default")); }
  }, false, "Choose the firmware file.");
  browse.dataset.browse = 1;
  row.append(browse);
  const v = dialog("Firmware Update", [h("p", {class: "dos-window__text"},
      "Install new firmware from a .UF2 file. Use " + uf2 + ", the\nfirmware for " + pico + " boards with this setup program."),
    pre(" "), row, pre(" "), h("p", {class: "dos-window__text"},
      "Sound stutters while the firmware is written. Do not switch\noff the computer until the card has restarted.")],
    [button("&Install", async () => {
      if (!file) { errorBox("Choose a firmware file first."); return; }
      const name = file.name.toUpperCase().slice(0, 30);
      if (!await confirmBox("Firmware Update", "Install " + name + "?\nThe card restarts when it is done.")) return;
      close(v);
      const w = waitBox("Installing", "Writing " + name + " to the card", true);
      quiet = true;
      try {
        await upload("/api/upload/firmware", file, p => w.set(p));
        close(w);
        waitRestart("Starting the new firmware.");
      } catch (e) {
        quiet = false;
        close(w);
        errorBox(e.message + "\nIf the card does not start, hold BOOTSEL and\ncopy the file to the " + pico + " over USB.");
      }
    }, true), button("&Cancel", () => close(v))]);
  show(v);
}

function wifi() {
  const name = fieldRow("&Network name", 14, st.net.ssid || "", 32, "", t => { if (!t) throw new Error("Enter the name of the network."); return t; });
  const pass = fieldRow("&Password", 14, "", 32, "", t => t, {type: "password", maxlength: 63});
  const v = dialog("WiFi Network", [name, pass, pre(" "), h("p", {class: "dos-window__text"},
      "Leave the password empty for an open network. The card joins\nat once, and this page may lose it. Save Settings keeps the\nnetwork. PGUSINIT /wifissid sets it from DOS.")],
    [button("&Join", async () => {
      let ssid;
      try { ssid = name.value(); } catch (e) { errorBox(e.message); return; }
      if (!await confirmBox("WiFi Network", "Join " + ssid.slice(0, 32) + "?")) return;
      close(v);
      if (await call("/api/wifi", {ssid, pass: pass.input.value})) say("Joining " + ssid.slice(0, 20) + ".");
    }, true), button("&Cancel", () => close(v))]);
  show(v);
}

function information() {
  const lines = h("div");
  const v = dialog("Card Information", [lines], [button("&Close", () => close(v), true)], {keys: [["Enter", "Close"], ["Esc", "Close"]]});
  v.paint = () => {
    const n = st.net, t = st.uptime, pico = st.chip === "RP2350" ? "Pico 2 W" : "Pico W";
    const row = (k, val) => pre(lead(k, 12).nodes.join("") + " " + ("" + val).slice(0, 50));
    lines.replaceChildren(
      row("Mode", modeName(st.mode)),
      row("Firmware", st.fw),
      row("Hardware", (st.board ? "PicoGUS v2.0" : "PicoGUS v1.x or Femto, " + pico) + " (" + (st.chip || "RP2040") + ")"),
      row("Uptime", Math.floor(t / 3600) + " h " + (Math.floor(t / 60) % 60) + " min " + (t % 60) + " s"),
      row("Network", n.ssid ? n.ssid + (n.rssi ? ", " + n.rssi + " dBm" : "") : "None"),
      row("Address", n.ip),
      row("MAC", n.mac.toUpperCase()),
      row("Web server", (n.loadPermille / 10).toFixed(1) + "% of core 1, longest run " + (n.pollRecentUs / 1000).toFixed(1) + " ms"),
      st.cd ? row("CD image", st.cd.image ? "#" + st.cd.index + " " + st.cd.image.toUpperCase() : st.cd.error || "None") : null);
  };
  v.paint();
  show(v);
}

function display() {
  const now = D.documentElement.dataset.theme;
  const row = radioRow("&Colours", 9, [["setup", "Setup"], ["commander", "Commander"], ["mono", "Mono"]], now, false, theme);
  const v = dialog("Display", [row], [button("C&lose", () => close(v), true)]);
  show(v);
}
function theme(t) {
  D.documentElement.dataset.theme = t;
  try { localStorage.setItem("theme", t); } catch (e) { /* private window */ }
}

function helpBox() {
  const v = {modal: true, keys: [["Enter", "Close"], ["Esc", "Close"]]};
  v.wins = [win("Help", "", 66, [
    pre("↑ ↓ ← →    Move the bar, change a value"),
    pre("Enter      Select a command, press the default button"),
    pre("Esc        Go back to the previous window"),
    pre("Tab        Go to the next field"),
    pre("Letter     The highlighted letter runs its command"),
    pre("Alt+Letter The same, while typing in a field"),
    pre("F2         Save the settings to the card"),
    pre(" "),
    h("p", {class: "dos-window__text"}, "The mouse works too: point at a command and click."),
    pre(" "),
    h("p", {class: "dos-window__text"}, "Screen font: IBM VGA 9x16 from The Ultimate Oldschool PC Font\nPack by VileR, int10h.org. Licence: CC BY-SA 4.0."),
    actions(button("&Close", () => close(v), true))])];
  show(v);
}

// ---- status of the card ----------------------------------------------------

function status(j) {
  const first = !st || j.features.join() !== [...feats].join() || j.mode !== st.mode;
  st = j;
  feats = new Set(j.features);
  online = true;
  verEl.textContent = j.fw.replace(/^.* v/, "v");
  if (first) {
    // another mode has other commands: start again from the main menu
    while (stack.length) close(stack[0]);
    mainMenu();
  } else for (const v of stack) if (v.paint) v.paint();
  if (j.upload.kind === 1 && j.upload.state === 3 && !quiet && !warned) {
    warned = true;
    errorBox("The last firmware update failed:\n" + j.upload.error.replace(/ - /g, "\n"));
  }
}
async function poll() {
  if (quiet || D.hidden) return;
  try {
    const j = await api("/api/status");
    if (!quiet) status(j);
  } catch (e) {
    if (online) { online = false; verEl.textContent = "No answer"; }
  }
}
// After a restart the card needs a few seconds to join the network again
async function waitRestart(text) {
  quiet = true;
  const w = waitBox("Restarting", text);
  await sleep(2500);
  for (let i = 0; i < 40; i++) {
    try {
      const j = await api("/api/status");
      quiet = false;
      st = null;
      close(w);
      status(j);
      say("The card is ready.");
      return;
    } catch (e) { await sleep(2000); }
  }
  quiet = false;
  close(w);
  errorBox("The card did not answer.\nCheck the network with PGUSINIT /wifistatus.");
}

try { const t = localStorage.getItem("theme"); if (t) D.documentElement.dataset.theme = t; } catch (e) { /* private window */ }
(async function start() {
  if (D.fonts && D.fonts.ready) await D.fonts.ready;
  const w = waitBox("PicoGUS", "Connecting to the card.");
  for (;;) {
    try { const j = await api("/api/status"); close(w); status(j); break; }
    catch (e) { verEl.textContent = "No answer"; await sleep(2000); }
  }
  setInterval(poll, 3000);
  D.addEventListener("visibilitychange", poll);
})();
})();
