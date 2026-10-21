"use strict"
// The launcher's page: state from the host (Bridge.cs) drawn; every change saved back to settings.ini.

const $ = (id) => document.getElementById(id)
let state = null
let update = null
let nextId = 1
const pending = new Map()

// ---- host calls

const host = window.chrome && window.chrome.webview
if (host) host.addEventListener("message", (e) => {
  const m = e.data
  if (m.event === "progress") return onProgress(m.data)
  const p = pending.get(m.id)
  if (!p) return
  pending.delete(m.id)
  m.ok ? p.resolve(m.result) : p.reject(new Error(m.error))
})

function call(cmd, args) {
  if (!host) return Promise.reject(new Error("Not running inside the launcher."))
  const id = nextId++
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject })
    host.postMessage({ id, cmd, args })
  })
}

// ---- helpers

function el(tag, cls, text) {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text !== undefined) e.textContent = text
  return e
}
function size(bytes) {
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + " GB"
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MB"
  return Math.max(1, Math.round(bytes / 1024)) + " KB"
}
function toast(text, good) {
  const t = $("toast")
  t.textContent = text
  t.className = "toast" + (good ? " good" : "")
  t.hidden = false
  clearTimeout(toast.timer)
  toast.timer = setTimeout(() => { t.hidden = true }, 5000)
}
function confirmBox(title, text, yes) {
  return new Promise((resolve) => {
    $("confirm-title").textContent = title
    $("confirm-text").textContent = text
    $("confirm-yes").textContent = yes || "OK"
    const box = $("confirm")
    box.hidden = false
    box.onclick = (e) => {
      const c = e.target.dataset && e.target.dataset.choice
      if (!c && e.target !== box) return
      box.hidden = true
      resolve(c === "yes")
    }
  })
}
async function guarded(fn) {
  try { return await fn() } catch (e) { toast(e.message) }
}

// ---- tooltips: any element with data-tip, shown after a short hover

let tipTimer = 0
let tipFor = null
document.addEventListener("mouseover", (e) => {
  const t = e.target.closest && e.target.closest("[data-tip]")
  if (t === tipFor) return
  hideTip()
  if (!t) return
  tipFor = t
  tipTimer = setTimeout(() => showTip(t), 450)
})
document.addEventListener("mousedown", hideTip)
document.addEventListener("scroll", hideTip, true)
function showTip(t) {
  const tip = $("tooltip")
  tip.textContent = t.dataset.tip
  tip.hidden = false
  const r = t.getBoundingClientRect()
  const w = tip.offsetWidth, h = tip.offsetHeight
  let x = r.left + Math.min(r.width / 2, 120) - 20
  let y = r.bottom + 8
  if (y + h > innerHeight - 8) y = r.top - h - 8
  x = Math.max(8, Math.min(x, innerWidth - w - 8))
  tip.style.left = x + "px"
  tip.style.top = y + "px"
  requestAnimationFrame(() => tip.classList.add("on"))
}
function hideTip() {
  clearTimeout(tipTimer)
  tipFor = null
  const tip = $("tooltip")
  tip.classList.remove("on")
  tip.hidden = true
}

// ---- tabs

$("tabs").onclick = (e) => {
  const b = e.target.closest("button")
  if (!b) return
  for (const t of $("tabs").children) t.classList.toggle("on", t === b)
  for (const p of ["play", "photos", "settings", "updates"]) $("page-" + p).hidden = p !== b.dataset.tab
  if (b.dataset.tab === "photos") loadPhotos()
  else if (b.dataset.tab !== "play") refresh()
}

// ---- drawing

function fill(select, items, value) {
  select.replaceChildren()
  for (const it of items) {
    const o = el("option", "", it.name)
    o.value = String(it.value)
    select.append(o)
  }
  select.value = String(value)
  if (select.selectedIndex < 0 && select.options.length) select.selectedIndex = select.options.length - 1
}

function render() {
  const s = state, set = s.settings, o = s.options
  $("version").textContent = "Version " + s.version + " · native PC port"

  const chips = $("chips")
  chips.replaceChildren()
  const chip = (cls, text, tip) => { const c = el("span", "chip " + cls); c.append(el("span", "dot"), text); if (tip) c.dataset.tip = tip; chips.append(c) }
  chip(s.checks.game ? "ok" : "bad", s.checks.game ? "Game files" : "Game files missing", "The game built from your disc, in the install's game folder.")
  chip(s.checks.update ? "ok" : "warn", s.checks.update ? "Title update" : "No title update", "The title update (needed for DLC), in Documents\\fm2\\update.")
  chip(s.checks.running ? "warn" : "ok", s.checks.running ? "Game running" : "Not running")

  // play page
  if (document.activeElement !== $("profile")) $("profile").value = set.profile
  fill($("resolution"), o.resolutions.map((r) => ({ value: r, name: r.replace("x", " × ") })), set.resolution)
  fill($("aspect"), o.aspects, set.aspect)
  fill($("maxFps"), o.fps.map((f) => ({ value: f, name: f + " fps" })), set.maxFps)
  fill($("msaa"), o.msaa, set.msaa)
  fill($("language"), o.languages.map((l) => ({ value: l.id, name: l.name })), set.language)
  $("lang-note").textContent = o.missingLanguages.length
    ? `Your disc also has ${o.missingLanguages.join(", ")} text. Those can't be picked yet.` : ""
  for (const b of $("display").children) b.classList.toggle("on", (b.dataset.full === "1") === set.fullscreen)
  for (const k of ["vsync", "enhance", "skipIntro"]) $(k).classList.toggle("on", !!set[k])

  const lang = o.languages.find((l) => l.id === set.language)
  const aspect = o.aspects.find((a) => a.value === set.aspect)
  $("summary").innerHTML = ""
  const line = (label, value) => { const d = el("div"); d.append(label + ": "); d.append(el("b", "", value)); $("summary").append(d) }
  line("Profile", set.profile || "Player")
  line("Display", `${set.resolution.replace("x", " × ")}, ${set.fullscreen ? "fullscreen" : "windowed"}${aspect && set.aspect !== "auto" ? ", " + aspect.name : ""}`)
  line("Frame rate", `${set.maxFps} fps cap, VSync ${set.vsync ? "on" : "off"}`)
  line("Anti-aliasing", (o.msaa.find((m) => m.value === set.msaa) || { name: "Off" }).name)
  line("Language", lang ? lang.name : "English")
  line("Intro", set.skipIntro ? "skipped" : "plays")

  const cl = $("checklist")
  cl.replaceChildren()
  const item = (ok, text) => { const d = el("div", "check-item " + (ok ? "ok" : "bad")); d.append(el("span", "dot"), text); cl.append(d) }
  item(s.checks.exe, s.checks.exe ? "fm2.exe found" : "fm2.exe is missing from this install")
  item(s.checks.game, s.checks.game ? "Game files ready" : "Game files missing: run the installer again")
  item(s.shaders.exists, s.shaders.exists ? `Shaders compiled (${s.shaders.shaders.toLocaleString()})` : "First start compiles shaders (a few minutes)")
  if (s.dlc.length) item(true, `${s.dlc.filter((d) => d.enabled).length} of ${s.dlc.length} DLC packs on`)
  $("launch").disabled = !s.checks.exe || !s.checks.game || s.checks.running
  $("launch").textContent = s.checks.running ? "Running" : "Play"

  // settings page
  const st = $("shader-stats")
  st.replaceChildren()
  const stat = (n, l) => { const d = el("div", "stat"); d.append(el("div", "n", n), el("div", "l", l)); st.append(d) }
  stat(s.shaders.exists ? s.shaders.shaders.toLocaleString() : "0", "compiled shaders")
  stat(s.shaders.exists ? size(s.shaders.bytes) : "-", "on disk")
  stat(s.shaders.built ? new Date(s.shaders.built).toLocaleDateString() : "-", "pipelines saved")
  stat(s.shaders.exists ? "Ready" : "Empty", "state")
  $("shaders-delete").disabled = !s.shaders.exists || s.checks.running
  $("shaders-rebuild").disabled = s.checks.running || !s.checks.exe

  const dl = $("dlc")
  dl.replaceChildren()
  if (!s.dlc.length) dl.append(el("div", "empty", "No DLC installed. Add DLC to bring in the car and track packs."))
  for (const p of s.dlc) {
    const it = el("div", "item" + (p.enabled ? "" : " off"))
    const main = el("div", "main")
    main.append(el("div", "title", p.name), el("div", "meta", size(p.bytes)))
    const sw = el("button", "switch" + (p.enabled ? " on" : ""))
    sw.dataset.tip = p.enabled ? "On: the game loads this pack. Click to switch it off (it's kept in dlc-disabled)." : "Off: kept, not loaded. Click to switch it on."
    sw.onclick = () => guarded(async () => { state = await call("dlc.toggle", { id: p.id, enabled: !p.enabled }); render() })
    it.append(main, sw)
    dl.append(it)
  }
  $("dlc-add").disabled = s.checks.running

  const f = $("folders")
  f.replaceChildren()
  for (const [k, v] of [["Install", s.paths.root], ["User data", s.paths.user]]) f.append(el("div", "k", k), el("div", "v", v))
}

function renderUpdate() {
  const u = update
  $("v-installed").textContent = (u && u.installed) || (state && state.version) || "-"
  $("v-latest").textContent = (u && u.latest) || "-"
  $("update-badge").hidden = !(u && u.available)
  const us = $("update-state")
  if (!u || !u.latest) { us.textContent = ""; us.className = "update-state" }
  else if (u.available) { us.textContent = `Version ${u.latest} is available.`; us.className = "update-state new" }
  else { us.textContent = "You're on the latest version."; us.className = "update-state ok" }
  $("update-apply").disabled = !(u && u.available && u.hasPatch) || (state && state.checks.running)
  $("update-apply").textContent = u && u.patchBytes ? `Download & install (${size(u.patchBytes)})` : "Download & install"
  $("notes-title").textContent = u && u.name ? u.name : "Release notes"
  const n = $("notes")
  n.replaceChildren()
  if (u && u.notes) n.append(...markdown(u.notes))
  else if (u) n.append(el("div", "empty", "No notes for this release."))
}

// A small Markdown subset (what the release notes use): headings, bullet lists, **bold**, `code`, paragraphs.
function markdown(text) {
  const out = []
  let list = null
  const inline = (s, into) => {
    for (const part of s.split(/(\*\*[^*]+\*\*|`[^`]+`)/)) {
      if (part.startsWith("**") && part.endsWith("**")) into.append(el("b", "", part.slice(2, -2)))
      else if (part.startsWith("`") && part.endsWith("`")) into.append(el("code", "", part.slice(1, -1)))
      else if (part) into.append(part.replace(/\[([^\]]+)\]\([^)]+\)/g, "$1"))
    }
    return into
  }
  for (const raw of text.replace(/\r/g, "").split("\n")) {
    const line = raw.trim()
    if (/^[-*] /.test(line)) {
      if (!list) { list = el("ul"); out.push(list) }
      list.append(inline(line.slice(2), el("li")))
      continue
    }
    list = null
    if (!line || line.startsWith("🤖")) continue
    const h = line.match(/^#+\s*(.*)$/)
    out.push(h ? inline(h[1], el("h3")) : inline(line, el("p")))
  }
  return out
}

// ---- settings changes

function current() {
  return { ...state.settings }
}
let saveTimer = 0
function change(patch) {
  Object.assign(state.settings, patch)
  render()
  clearTimeout(saveTimer)
  saveTimer = setTimeout(() => guarded(async () => { state = await call("save", state.settings); render() }), 250)
}
$("profile").oninput = (e) => { state.settings.profile = e.target.value; clearTimeout(saveTimer); saveTimer = setTimeout(() => change({}), 500) }
$("resolution").onchange = (e) => change({ resolution: e.target.value })
$("aspect").onchange = (e) => change({ aspect: e.target.value })
$("maxFps").onchange = (e) => change({ maxFps: +e.target.value })
$("msaa").onchange = (e) => change({ msaa: +e.target.value })
$("language").onchange = (e) => change({ language: +e.target.value })
$("display").onclick = (e) => { const b = e.target.closest("button"); if (b) change({ fullscreen: b.dataset.full === "1" }) }
for (const k of ["vsync", "enhance", "skipIntro"]) $(k).onclick = () => change({ [k]: !state.settings[k] })

// ---- actions

$("launch").onclick = () => guarded(async () => {
  const status = $("status")
  $("launch").disabled = true
  status.className = "status"
  status.textContent = state.shaders.exists ? "Starting…" : "Starting: the first start compiles shaders behind the loading screen (a few minutes)."
  try {
    await call("launch", { settings: current() })
    status.className = "status done"
    status.textContent = "Game started."
    setTimeout(refresh, 2500)
  } catch (e) {
    status.className = "status error"
    status.textContent = e.message
    $("launch").disabled = false
  }
})

$("shaders-delete").onclick = () => guarded(async () => {
  if (!await confirmBox("Delete the shader cache?", "The next start compiles every shader again (a few minutes behind the loading screen). Saves and settings are kept.", "Delete")) return
  state = await call("shaders.delete")
  render()
  toast("Shader cache deleted.", true)
})
$("shaders-rebuild").onclick = () => guarded(async () => {
  if (!await confirmBox("Rebuild the shader cache?", "The cache is deleted and the game starts to compile it again. Let it run: after the loading screen there can be up to a minute of black screen while pipelines build.", "Rebuild")) return
  state = await call("shaders.rebuild")
  render()
  toast("Game started: compiling shaders.", true)
})
$("dlc-add").onclick = () => guarded(async () => {
  const r = await call("dlc.add")
  state = r.state
  render()
  if (r.added) toast(`${r.added} package${r.added > 1 ? "s" : ""} added. The game unpacks them on the next start.`, true)
})
for (const b of document.querySelectorAll("[data-open]")) b.onclick = () => guarded(() => call("open", b.dataset.open))

async function checkUpdates(quiet) {
  try {
    update = await call("update.check")
    renderUpdate()
  } catch (e) {
    if (!quiet) toast("Couldn't reach GitHub: " + e.message)
    $("notes").replaceChildren(el("div", "empty", "Couldn't reach GitHub. Check your connection and try again."))
  }
}
$("update-check").onclick = () => checkUpdates(false)
$("update-page").onclick = () => guarded(() => call("url", (update && update.page) || "https://github.com/zer0ducksgiven/Forza-Motorsport-2-ReGPU/releases"))
$("update-apply").onclick = () => guarded(async () => {
  if (!await confirmBox(`Install ${update.latest}?`, "The patch is downloaded from GitHub and installed over this install. Saves and settings are kept. The first start afterwards may rebuild the shader cache.", "Install")) return
  $("update-apply").disabled = true
  $("update-check").disabled = true
  $("progress").classList.add("on")
  $("update-status").className = "status"
  try {
    const r = await call("update.apply")
    update = r.update
    state = r.state
    render()
    renderUpdate()
    $("update-status").className = "status done"
    $("update-status").textContent = `Installed ${r.files} file${r.files === 1 ? "" : "s"}.`
    if (r.launcherChanged && await confirmBox("Restart the launcher?", "The update includes a new launcher. Restart it now to use the new version.", "Restart")) await call("restart")
  } catch (e) {
    $("update-status").className = "status error"
    $("update-status").textContent = e.message
    renderUpdate()
  } finally {
    $("update-check").disabled = false
    $("progress").classList.remove("on")
  }
})
function onProgress(d) {
  $("progress").querySelector(".bar").style.width = Math.round(d.p * 100) + "%"
  $("update-status").textContent = d.text
}

// ---- photos (Photo Mode saves, read straight from the profiles folder)

let photos = []
let photoId = null
async function loadPhotos() {
  await guarded(async () => {
    photos = await call("photos.list")
    if (!photos.some((p) => p.id === photoId)) photoId = photos.length ? photos[0].id : null
    renderPhotos()
  })
}
function renderPhotos() {
  $("photo-count").textContent = photos.length ? String(photos.length) : ""
  const t = $("thumbs")
  t.replaceChildren()
  if (!photos.length) t.append(el("div", "empty", "Nothing saved yet."))
  for (const p of photos) {
    const b = el("button", "thumb" + (p.id === photoId ? " on" : ""))
    const img = el("img")
    img.src = p.url
    img.loading = "lazy"
    img.alt = p.name
    b.append(img, el("span", "", p.name))
    b.onclick = () => { photoId = p.id; renderPhotos() }
    t.append(b)
  }
  const p = photos.find((x) => x.id === photoId)
  const v = $("viewer")
  if (p) {
    let img = v.querySelector("img")
    if (!img) { img = el("img"); v.replaceChildren(img) }
    if (img.src !== p.url) img.src = p.url
    img.alt = p.name
    $("photo-title").textContent = p.name
    $("photo-meta").textContent = `${p.profile} · ${new Date(p.taken).toLocaleString()} · ${size(p.bytes)}`
  } else {
    v.replaceChildren(el("div", "empty", "No photos yet. In a race, pause and pick Photo Mode, take a shot and save it."))
    $("photo-title").textContent = "Photos"
    $("photo-meta").textContent = ""
  }
  for (const k of ["photo-copy", "photo-save", "photo-reveal"]) $(k).disabled = !p
}
function stepPhoto(d) {
  const i = photos.findIndex((p) => p.id === photoId)
  if (i < 0) return
  const n = photos[Math.max(0, Math.min(photos.length - 1, i + d))]
  if (n && n.id !== photoId) { photoId = n.id; renderPhotos(); document.querySelector(".thumb.on")?.scrollIntoView({ block: "nearest" }) }
}
document.addEventListener("keydown", (e) => {
  if ($("page-photos").hidden || !$("confirm").hidden) return
  if (e.key === "ArrowLeft" || e.key === "ArrowUp") { stepPhoto(-1); e.preventDefault() }
  if (e.key === "ArrowRight" || e.key === "ArrowDown") { stepPhoto(1); e.preventDefault() }
  if (e.key === "c" && e.ctrlKey && photoId) $("photo-copy").click()
})
$("photo-copy").onclick = () => guarded(async () => { await call("photos.copy", photoId); toast("Photo copied to the clipboard.", true) })
$("photo-save").onclick = () => guarded(async () => { if (await call("photos.save", photoId)) toast("Photo saved.", true) })
$("photo-reveal").onclick = () => guarded(() => call("photos.reveal", photoId))
$("photos-folder").onclick = () => guarded(() => call("photos.reveal", null))

async function refresh() {
  await guarded(async () => { state = await call("state"); render(); renderUpdate() })
}

refresh().then(() => checkUpdates(true))
setInterval(() => { if (!document.hidden && $("confirm").hidden && !["SELECT", "INPUT"].includes(document.activeElement.tagName)) refresh() }, 5000)
