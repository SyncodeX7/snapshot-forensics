"use strict";

// ---------------------------------------------------------------------------
// tiny helpers
// ---------------------------------------------------------------------------
const $ = (sel, root = document) => root.querySelector(sel);

function el(tag, props = {}, children = []) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === "class") node.className = v;
    else if (k === "text") node.textContent = v;
    else if (k === "html") node.innerHTML = v;
    else if (k.startsWith("on")) node.addEventListener(k.slice(2), v);
    else if (v !== null && v !== undefined) node.setAttribute(k, v);
  }
  for (const child of [].concat(children)) {
    if (child === null || child === undefined) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
}

const fmtNum = (n) => (n === null || n === undefined) ? "-" : Number(n).toLocaleString();
const fmtHex = (n, w = 16) => "0x" + BigInt(n).toString(16).toUpperCase().padStart(w, "0");
const fmtBytes = (n) => {
  if (n === null || n === undefined) return "-";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let v = Number(n), i = 0;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return `${v.toFixed(v >= 100 || i === 0 ? 0 : 1)} ${units[i]}`;
};
const fmtPct = (v) => (v === null || v === undefined) ? "-" : `${(v * 100).toFixed(1)}%`;
const fmtNs = (n) => n ? `${(Number(n) / 1e6).toFixed(1)} ms` : "-";

function toast(msg, ms = 4000) {
  const t = $("#toast");
  t.textContent = msg;
  t.classList.add("show");
  clearTimeout(toast._t);
  toast._t = setTimeout(() => t.classList.remove("show"), ms);
}

async function api(path, opts) {
  const res = await fetch(path, opts);
  if (!res.ok) {
    let detail = res.statusText;
    try { detail = (await res.json()).error || detail; } catch (_) {}
    throw new Error(detail);
  }
  return res.json();
}

function parseHash() {
  const raw = decodeURIComponent(location.hash.slice(1));
  if (!raw) return { rid: null, tab: null };
  const [rid, query] = raw.split("?");
  const params = new URLSearchParams(query || "");
  return { rid: rid || null, tab: params.get("tab") };
}

function updateHash() {
  if (!state.rid) return;
  const suffix = state.tab && state.tab !== "overview" ? `?tab=${encodeURIComponent(state.tab)}` : "";
  history.replaceState(null, "", "#" + encodeURIComponent(state.rid) + suffix);
}

const state = {
  runs: [],
  rid: null,
  info: null,
  tab: "overview",
  slot: 0,
  pageOffset: 0,
  poll: null,
};

// ---------------------------------------------------------------------------
// sidebar + run loading
// ---------------------------------------------------------------------------
async function loadRuns(selectId) {
  const data = await api("/api/runs");
  state.runs = data.runs || [];
  renderRunList();
  renderRunSelect();
  $("#side-foot").textContent = `${state.runs.length} run(s) under out/`;
  if (selectId) {
    selectRun(selectId);
  } else if (!state.rid && state.runs.length) {
    const h = parseHash();
    const wanted = state.runs.some(r => r.id === h.rid) ? h.rid : state.runs[0].id;
    selectRun(wanted, h.tab);
  }
}

function renderRunList() {
  const list = $("#run-list");
  list.replaceChildren();
  if (!state.runs.length) {
    list.append(el("div", { class: "empty", text: "No RAM dumps yet. Run scripts/run_qemu.sh or scripts/boxes.sh capture." }));
    return;
  }
  for (const r of state.runs) {
    const item = el("div", {
      class: "run-item" + (r.id === state.rid ? " active" : ""),
      onclick: () => selectRun(r.id),
    }, [
      el("div", { class: "rid", text: r.id }),
      el("div", { class: "meta" }, [
        r.error ? el("span", { class: "badge bad", text: "no header" }) : el("span", { class: "badge", text: r.mode }),
        r.error ? null : el("span", { text: `${r.ticks} ticks` }),
        r.error ? el("span", { class: "muted", text: r.error }) : el("span", { text: fmtBytes(r.bytes_copied) }),
        r.has_extract ? el("span", { class: "badge good", text: "extracted" }) : null,
        r.has_vol ? el("span", { class: "badge good", text: "vol" }) : null,
      ]),
    ]);
    list.append(item);
  }
}

function renderRunSelect() {
  const sel = $("#run-select");
  sel.replaceChildren();
  for (const r of state.runs) {
    sel.append(el("option", { value: r.id, text: r.id }));
  }
  if (state.rid) sel.value = state.rid;
}

async function selectRun(rid, tab) {
  state.rid = rid;
  state.slot = 0;
  state.pageOffset = 0;
  if (tab && TABS.some(([id]) => id === tab)) state.tab = tab;
  updateHash();
  renderRunList();
  $("#run-select").value = rid;
  $("#view").replaceChildren(el("div", { class: "empty", text: "Parsing snapshot header…" }));
  try {
    state.info = await api(`/api/run/${encodeURIComponent(rid)}`);
  } catch (err) {
    state.info = { error: err.message };
  }
  renderTabs();
  renderView();
}

// ---------------------------------------------------------------------------
// tabs
// ---------------------------------------------------------------------------
const TABS = [
  ["overview", "Overview"],
  ["snapshots", "Snapshots"],
  ["ranges", "Memory map"],
  ["heatmap", "Heatmap"],
  ["hex", "Hex"],
  ["search", "Search"],
  ["logs", "Logs"],
  ["volatility", "Volatility"],
];

function renderTabs() {
  const nav = $("#tabs");
  nav.replaceChildren();
  if (!state.info || state.info.error) return;
  for (const [id, label] of TABS) {
    nav.append(el("div", {
      class: "tab" + (state.tab === id ? " active" : ""),
      text: label,
      onclick: () => { state.tab = id; updateHash(); renderTabs(); renderView(); },
    }));
  }
}

function renderView() {
  const view = $("#view");
  view.replaceChildren();
  if (!state.info) return;
  if (state.info.error) {
    view.append(el("div", { class: "panel" }, [
      el("h3", { text: "Cannot parse this dump" }),
      el("p", { class: "bad", text: state.info.error }),
      el("p", { class: "muted", text: "The dump must contain the SnapshotPkg reserved buffer. Check that the OVMF build has the snapshot drivers and that SMM is enabled." }),
    ]));
    return;
  }
  const fn = {
    overview: renderOverview,
    snapshots: renderSnapshots,
    ranges: renderRanges,
    heatmap: renderHeatmap,
    hex: renderHex,
    search: renderSearch,
    logs: renderLogs,
    volatility: renderVolatility,
  }[state.tab];
  fn(view);
}

// ---------------------------------------------------------------------------
// overview
// ---------------------------------------------------------------------------
function card(k, v, s, cls) {
  return el("div", { class: "card" }, [
    el("div", { class: "k", text: k }),
    el("div", { class: "v" + (cls ? " " + cls : ""), text: v }),
    s ? el("div", { class: "s", text: s }) : null,
  ]);
}

function renderOverview(view) {
  const info = state.info, h = info.header;
  const ext = info.extraction && info.extraction.meta;
  const ver = ext && ext.verification;
  const mism = ver ? ver.hash_mismatches : null;
  const cov = ver ? ver.coverage_of_ranges : null;

  view.append(el("div", { class: "cards" }, [
    card("Mode", h.mode_name, `state: ${h.state_name}`),
    card("Ticks", fmtNum(h.tick), `${fmtNum(h.smi_count)} SMIs`),
    card("Sequence", fmtNum(h.sequence), `full: ${fmtNum(h.full_sequence)}`),
    card("Pages / snapshot", fmtNum(h.pages_last), `${fmtNum(h.pages_total)} total`),
    card("Bytes copied", fmtBytes(h.bytes_copied), `buffer ${fmtBytes(h.buffer_size)}`),
    card("TSC", h.tsc_hz ? `${(h.tsc_hz / 1e9).toFixed(3)} GHz` : "-", h.tsc_hz ? "calibrated from ticks" : "not calibrated"),
    card("SMI time (max)", fmtNs(h.smi_tsc_max), `total ${fmtNs(h.smi_tsc_total)}`),
    card("Coverage", cov !== null && cov !== undefined ? fmtPct(cov) : "—", mism === null ? "run Analyze to verify" : `${mism} hash mismatches`, mism === 0 ? "good" : (mism ? "bad" : "")),
  ]));

  const kv = el("div", { class: "kv" });
  const add = (k, v) => kv.append(el("span", { class: "k", text: k }), el("span", { class: "v", text: v }));
  add("Run", info.id);
  add("RAM dump", info.ram);
  add("Header offset", fmtHex(info.header_offset));
  add("Buffer base", fmtHex(h.buffer_base));
  add("Buffer size", `${fmtBytes(h.buffer_size)} (${fmtHex(h.buffer_size)})`);
  add("Guest RAM top", `${fmtBytes(h.guest_ram_top)} (${fmtHex(h.guest_ram_top)})`);
  add("Tracked ranges", fmtNum(h.range_count));
  add("Slot directory", `${h.slots.length} entries, delta write index ${h.delta_write_index}`);
  add("Configured period", `${h.reserved0} s`);
  if (ext) {
    add("Extracted image", ext.image || "-");
    add("Image pages", fmtNum(ver ? ver.unique_pages : null));
  }
  view.append(el("div", { class: "panel" }, [el("h3", { text: "Header" }), kv]));

  const slot = info.slots[info.slots.length - 1];
  if (slot) {
    view.append(el("div", { class: "panel" }, [
      el("h3", { text: "Newest slot" }),
      el("div", { class: "kv" }, [
        el("span", { class: "k", text: "Sequence" }), el("span", { class: "v", text: String(slot.sequence) }),
        el("span", { class: "k", text: "Pages" }), el("span", { class: "v", text: `${fmtNum(slot.page_count)} / ${fmtNum(slot.capacity_pages)}` }),
        el("span", { class: "k", text: "Data bytes" }), el("span", { class: "v", text: fmtBytes(slot.data_bytes) }),
        el("span", { class: "k", text: "SMI duration" }), el("span", { class: "v", text: fmtNs(slot.smi_tsc) }),
        el("span", { class: "k", text: "Flags" }), el("span", { class: "v", text: flagNames(slot, true).join(", ") || "none" }),
      ]),
    ]));
  }

  view.append(el("div", { class: "panel" }, [
    el("h3", { text: "Analyze pipeline" }),
    el("p", { class: "muted", text: "Extract the snapshot into a sparse raw image, verify every page hash, then run Volatility 3 linux.pslist with the guest ISF." }),
    el("button", { class: "primary", text: "Extract + Volatility", onclick: runAnalyze }),
  ]));
}

function flagNames(slot, entry) {
  const out = [];
  if (slot.complete) out.push("complete");
  if (slot.full) out.push("full");
  if (slot.truncated) out.push("truncated");
  if (slot.partial) out.push("partial");
  if (entry && slot.slot_flags & 0x8) out.push("capturing");
  return out;
}

// ---------------------------------------------------------------------------
// snapshots
// ---------------------------------------------------------------------------
function renderSnapshots(view) {
  const info = state.info;
  if (!info.slots.length) {
    view.append(el("div", { class: "panel" }, [el("p", { class: "muted", text: "No complete slots in the directory yet." })]));
    return;
  }
  const chart = el("canvas", { height: "130" });
  view.append(el("div", { class: "panel" }, [
    el("h3", { text: "Slot timeline (pages per SMI)" }),
    chart,
    el("div", { class: "legend" }, [
      el("span", {}, [el("span", { class: "swatch", style: "background:#4fd1c5" }), "full"]),
      el("span", {}, [el("span", { class: "swatch", style: "background:#7aa2f7" }), "delta / sliced"]),
    ]),
  ]));
  drawSlotChart(chart, info.slots);

  const body = el("tbody");
  for (const s of [...info.slots].reverse()) {
    body.append(el("tr", {
      onclick: () => { state.slot = s.index; state.pageOffset = 0; state.tab = "snapshots"; renderTabs(); renderView(); },
      style: "cursor:pointer",
    }, [
      el("td", { class: "mono", text: String(s.sequence) }),
      el("td", { class: "mono", text: String(s.index) }),
      el("td", { text: flagNames(s).join(", ") }),
      el("td", { class: "right mono", text: fmtNum(s.page_count) }),
      el("td", { class: "right mono", text: fmtBytes(s.data_bytes) }),
      el("td", { class: "right mono", text: fmtNs(s.smi_tsc) }),
      el("td", { class: "mono", text: fmtHex(s.offset) }),
    ]));
  }
  view.append(el("div", { class: "panel" }, [
    el("h3", { text: "Snapshot directory" }),
    el("div", { class: "scroll" }, [el("table", {}, [
      el("thead", {}, [el("tr", {}, ["Seq", "Dir", "Flags", "Pages", "Data", "SMI", "Offset"].map(h => el("th", { text: h })))]),
      body,
    ])]),
    el("p", { class: "muted", text: "Click a slot to browse its page map." }),
  ]));

  renderPageMap(view);
}

async function renderPageMap(view) {
  const slot = state.info.slots.find(s => s.index === state.slot) || state.info.slots[0];
  if (!slot) return;
  const panel = el("div", { class: "panel" }, [el("h3", { text: `Page map — slot ${slot.index} (seq ${slot.sequence})` })]);
  view.append(panel);
  const body = el("tbody");
  const table = el("table", {}, [
    el("thead", {}, [el("tr", {}, ["#", "Physical address", "Page hash", "Flags"].map(h => el("th", { text: h })))]),
    body,
  ]);
  const pager = el("div", { class: "pager" });
  panel.append(pager, el("div", { class: "scroll" }, [table]));
  const status = el("span", { class: "muted" });
  pager.append(status);

  const limit = 200;
  const data = await api(`/api/run/${encodeURIComponent(state.info.id)}/pages?slot=${slot.index}&offset=${state.pageOffset}&limit=${limit}`);
  status.textContent = `${fmtNum(state.pageOffset + 1)}–${fmtNum(Math.min(state.pageOffset + limit, data.total))} of ${fmtNum(data.total)}`;
  const prev = el("button", { text: "← Prev", disabled: state.pageOffset <= 0, onclick: () => { state.pageOffset = Math.max(0, state.pageOffset - limit); renderView(); } });
  const next = el("button", { text: "Next →", disabled: state.pageOffset + limit >= data.total, onclick: () => { state.pageOffset += limit; renderView(); } });
  pager.prepend(prev, next);

  for (const p of data.pages) {
    const flags = [];
    if (p.flags & 1) flags.push("full");
    if (p.flags & 2) flags.push("delta");
    if (p.flags & 4) flags.push("dirty");
    if (p.flags & 8) flags.push("sliced");
    body.append(el("tr", {
      onclick: () => openHex(p.pa),
      style: "cursor:pointer",
    }, [
      el("td", { class: "mono", text: String(p.index) }),
      el("td", { class: "mono", text: fmtHex(p.pa, 9) }),
      el("td", { class: "mono", text: fmtHex(p.hash, 8) }),
      el("td", { text: flags.join(", ") || "—" }),
    ]));
  }
}

function drawSlotChart(canvas, slots) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth || 800;
  const cssH = 130;
  canvas.width = cssW * dpr;
  canvas.height = cssH * dpr;
  const ctx = canvas.getContext("2d");
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, cssW, cssH);
  const pad = 8;
  const max = Math.max(1, ...slots.map(s => s.page_count));
  const bw = (cssW - pad * 2) / slots.length;
  slots.forEach((s, i) => {
    // sqrt scale keeps small delta slots visible next to a full snapshot
    const h = (cssH - 28) * Math.sqrt(s.page_count / max);
    const x = pad + i * bw + 2;
    ctx.fillStyle = s.full ? "#4fd1c5" : "#7aa2f7";
    ctx.globalAlpha = s.complete ? 1 : 0.4;
    ctx.fillRect(x, cssH - 20 - h, Math.max(1, bw - 4), h);
    ctx.globalAlpha = 1;
    ctx.fillStyle = "#7d8fa3";
    ctx.font = "10px monospace";
    ctx.fillText(String(s.sequence), x, cssH - 6);
    canvas.title = "full snapshots and deltas; bar height uses a square-root scale";
  });
}

// ---------------------------------------------------------------------------
// ranges
// ---------------------------------------------------------------------------
function renderRanges(view) {
  const info = state.info;
  const body = el("tbody");
  for (const r of info.ranges) {
    body.append(el("tr", {}, [
      el("td", { class: "mono", text: fmtHex(r.base, 9) }),
      el("td", { class: "mono", text: fmtHex(r.base + r.length, 9) }),
      el("td", { class: "right mono", text: fmtBytes(r.length) }),
      el("td", { class: "mono", text: String(r.type) }),
      el("td", { class: "mono", text: fmtHex(r.flags, 2) }),
    ]));
  }
  view.append(el("div", { class: "panel" }, [
    el("h3", { text: `Tracked guest ranges (${info.ranges.length})` }),
    el("table", {}, [
      el("thead", {}, [el("tr", {}, ["Base", "End", "Size", "Type", "Flags"].map(h => el("th", { text: h })))]),
      body,
    ]),
    el("p", { class: "muted", text: "Captured before ExitBootServices from GetMemoryMap(): EfiConventionalMemory plus loader code/data." }),
  ]));
}

// ---------------------------------------------------------------------------
// heatmap
// ---------------------------------------------------------------------------
function renderHeatmap(view) {
  const canvas = el("canvas", { height: "180" });
  const legend = el("div", { class: "legend" });
  const panel = el("div", { class: "panel" }, [
    el("h3", { text: "Captured pages across physical address space" }),
    canvas,
    legend,
  ]);
  view.append(panel);
  api(`/api/run/${encodeURIComponent(state.info.id)}/heatmap?bins=512`).then(data => {
    drawHeatmap(canvas, data);
    legend.replaceChildren(
      el("span", { text: `${fmtNum(data.captured_pages)} unique pages captured` }),
      el("span", { text: `bin ${fmtBytes(data.bin_size)}` }),
      el("span", { text: `max ${fmtNum(data.max)} pages/bin` }),
      el("span", { text: `RAM top ${fmtHex(data.ram_top)}` }),
    );
  }).catch(err => toast("heatmap: " + err.message));
}

function drawHeatmap(canvas, data) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth || 800;
  const cssH = 180;
  canvas.width = cssW * dpr;
  canvas.height = cssH * dpr;
  const ctx = canvas.getContext("2d");
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, cssW, cssH);
  const n = data.bins.length;
  const bw = cssW / n;
  for (let i = 0; i < n; i++) {
    const t = data.max ? data.bins[i] / data.max : 0;
    const hue = 190 - 150 * t;
    const light = 12 + 48 * t;
    ctx.fillStyle = data.bins[i] ? `hsl(${hue} 75% ${light}%)` : "#0a1016";
    ctx.fillRect(i * bw, 0, Math.ceil(bw), cssH);
  }
  ctx.fillStyle = "#7d8fa3";
  ctx.font = "10px monospace";
  ctx.fillText("0x0", 4, cssH - 6);
  const top = "0x" + data.ram_top.toString(16).toUpperCase();
  ctx.fillText(top, cssW - ctx.measureText(top).width - 4, cssH - 6);
}

// ---------------------------------------------------------------------------
// hex / search
// ---------------------------------------------------------------------------
function openHex(pa) {
  state.tab = "hex";
  renderTabs();
  renderView(pa);
}

function renderHex(view, jump) {
  const input = el("input", { type: "text", value: jump !== undefined ? fmtHex(jump, 9) : "0x000000000", placeholder: "physical address (hex)" });
  const len = el("input", { type: "text", value: "256", size: 6, placeholder: "length" });
  const src = el("select", {}, [
    el("option", { value: "dump", text: "RAM dump (full address space)" }),
    el("option", { value: "image", text: "extracted image (captured pages)" }),
  ]);
  const out = el("pre", { class: "hexdump" });
  const status = el("span", { class: "muted" });
  const go = async () => {
    let pa;
    try { pa = BigInt(input.value.trim()); } catch (_) { toast("bad address"); return; }
    const n = Math.min(Math.max(parseInt(len.value, 10) || 256, 16), 65536);
    try {
      const data = await api(`/api/run/${encodeURIComponent(state.info.id)}/hex?pa=${pa}&len=${n}&source=${src.value}`);
      out.replaceChildren();
      for (const row of data.rows) {
        const line = el("div");
        line.append(el("span", { class: "addr", text: fmtHex(row.addr, 9) + "  " }));
        line.append(el("span", { class: "hex", text: row.hex.padEnd(47, " ") + "  " }));
        line.append(el("span", { class: "asc", text: row.ascii }));
        out.append(line);
      }
      status.textContent = `${data.len} bytes from ${data.source}`;
    } catch (err) { toast(err.message); }
  };
  view.append(el("div", { class: "panel" }, [
    el("h3", { text: "Physical memory viewer" }),
    el("div", { class: "row" }, [
      el("label", { class: "muted", text: "Address" }), input,
      el("label", { class: "muted", text: "Length" }), len,
      el("label", { class: "muted", text: "Source" }), src,
      el("button", { class: "primary", text: "Read", onclick: go }),
      status,
    ]),
    out,
  ]));
  go();
}

function renderSearch(view) {
  const input = el("input", { type: "text", placeholder: "ASCII string, e.g. SNAPSHOT-GUEST", value: "SNAPSHOT-GUEST" });
  const out = el("div", { class: "scroll" });
  const go = async () => {
    const q = input.value;
    if (!q) return;
    out.replaceChildren(el("div", { class: "empty", text: "Searching…" }));
    try {
      const data = await api(`/api/run/${encodeURIComponent(state.info.id)}/search?q=${encodeURIComponent(q)}&limit=100`);
      out.replaceChildren();
      if (!data.matches.length) {
        out.append(el("div", { class: "empty", text: "No matches." }));
        return;
      }
      const body = el("tbody");
      for (const m of data.matches) {
        body.append(el("tr", {
          style: "cursor:pointer",
          onclick: () => openHex(m.pa),
        }, [
          el("td", { class: "mono", text: fmtHex(m.pa, 9) }),
          el("td", { class: "mono", text: m.context }),
          el("td", { class: "mono muted", text: m.source }),
        ]));
      }
      out.append(el("table", {}, [
        el("thead", {}, [el("tr", {}, ["Address", "Context", "Source"].map(h => el("th", { text: h })))]),
        body,
      ]));
    } catch (err) { out.replaceChildren(el("div", { class: "bad", text: err.message })); }
  };
  view.append(el("div", { class: "panel" }, [
    el("h3", { text: "Search captured memory" }),
    el("div", { class: "row" }, [input, el("button", { class: "primary", text: "Search", onclick: go })]),
    out,
  ]));
  go();
}

// ---------------------------------------------------------------------------
// logs / volatility
// ---------------------------------------------------------------------------
function renderLogs(view) {
  const pre = el("pre");
  const sel = el("select", {}, [
    el("option", { value: "serial", text: "guest serial" }),
    el("option", { value: "debug", text: "OVMF debug port" }),
  ]);
  const load = async () => {
    const which = sel.value;
    try {
      const res = await fetch(`/api/run/${encodeURIComponent(state.info.id)}/log?which=${which}&lines=1000`);
      pre.textContent = await res.text();
      pre.scrollTop = pre.scrollHeight;
    } catch (err) { pre.textContent = err.message; }
  };
  sel.addEventListener("change", load);
  view.append(el("div", { class: "panel" }, [
    el("h3", { text: "Logs" }),
    el("div", { class: "row" }, [sel, el("button", { text: "Reload", onclick: load })]),
    pre,
  ]));
  load();
}

function renderVolatility(view) {
  const vol = state.info.volatility;
  const panel = el("div", { class: "panel" }, [el("h3", { text: "Volatility 3 — linux.pslist" })]);
  if (!vol || !vol.pslist) {
    panel.append(
      el("p", { class: "muted", text: "No Volatility output for this run yet." }),
      el("button", { class: "primary", text: "Run extraction + Volatility", onclick: runAnalyze }),
    );
  } else {
    panel.append(el("pre", { text: vol.pslist }));
    if (vol.pstree) panel.append(el("h3", { text: "pstree" }), el("pre", { text: vol.pstree }));
    if (vol.bash) panel.append(el("h3", { text: "bash" }), el("pre", { text: vol.bash }));
    if (vol.malfind) panel.append(el("h3", { text: "malfind" }), el("pre", { text: vol.malfind }));
  }
  view.append(panel);
}

// ---------------------------------------------------------------------------
// analyze job
// ---------------------------------------------------------------------------
async function runAnalyze() {
  if (!state.rid) return;
  const btn = $("#btn-analyze");
  btn.disabled = true;
  try {
    const { job } = await api(`/api/run/${encodeURIComponent(state.rid)}/analyze`, { method: "POST" });
    toast("Analysis started…");
    if (state.poll) clearInterval(state.poll);
    state.poll = setInterval(async () => {
      const jobs = await api("/api/jobs");
      const j = (jobs.jobs || []).find(x => x.id === job);
      if (!j || j.status === "running") return;
      clearInterval(state.poll);
      state.poll = null;
      btn.disabled = false;
      toast(j.status === "done" ? "Analysis complete" : "Analysis failed — see logs");
      await selectRun(state.rid);
    }, 1500);
  } catch (err) {
    btn.disabled = false;
    toast("analyze: " + err.message);
  }
}

// ---------------------------------------------------------------------------
// boot
// ---------------------------------------------------------------------------
$("#btn-refresh").addEventListener("click", () => loadRuns(state.rid));
$("#btn-analyze").addEventListener("click", runAnalyze);
$("#run-select").addEventListener("change", (e) => selectRun(e.target.value));
window.addEventListener("hashchange", () => {
  const h = parseHash();
  if (h.rid && h.rid !== state.rid) {
    selectRun(h.rid, h.tab);
  } else if (h.tab && h.tab !== state.tab && TABS.some(([id]) => id === h.tab)) {
    state.tab = h.tab;
    renderTabs();
    renderView();
  }
});

loadRuns().catch(err => {
  $("#view").replaceChildren(el("div", { class: "panel bad", text: "Failed to load: " + err.message }));
});
