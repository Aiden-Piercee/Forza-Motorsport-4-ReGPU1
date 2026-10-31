"use strict"
// The message channel to the launcher (Bridge.cs): Photino's window.external, on WebView2 (Windows) and WebKitGTK
// (Linux) alike. Same shape as WebView2's channel: postMessage(object), "message" listeners getting { data }.

const host = (() => {
  const ext = window.external
  if (!ext || typeof ext.sendMessage !== "function") return null
  const listeners = []
  ext.receiveMessage((text) => {
    const m = JSON.parse(text)
    for (const f of listeners) f({ data: m })
  })
  return { postMessage: (msg) => ext.sendMessage(JSON.stringify(msg)), addEventListener: (_, f) => listeners.push(f) }
})()
