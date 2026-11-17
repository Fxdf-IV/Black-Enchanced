// Uso: node tools/cdp.mjs <porta> eval '<js>' | shot <arquivo.png> | key <KeyW> <ms>
const [port, cmd, a, b] = process.argv.slice(2);
const tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const tab = tabs.find(t => t.type === "page");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => (ws.onopen = r));
let id = 0; const pend = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); pend.get(m.id)?.(m); };
const send = (method, params = {}) => new Promise(r => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
if (cmd === "eval") {
  const r = await send("Runtime.evaluate", { expression: a, awaitPromise: true, returnByValue: true });
  console.log(JSON.stringify(r.result?.result?.value ?? r.result));
} else if (cmd === "shot") {
  const r = await send("Page.captureScreenshot", { format: "png" });
  (await import("node:fs")).writeFileSync(a, Buffer.from(r.result.data, "base64"));
  console.log("ok", a);
} else if (cmd === "key") {
  const code = a, key = code.replace(/^Key/, "").toLowerCase();
  const vk = key.toUpperCase().charCodeAt(0);
  await send("Input.dispatchKeyEvent", { type: "rawKeyDown", code, key, windowsVirtualKeyCode: vk });
  await new Promise(r => setTimeout(r, Number(b || 500)));
  await send("Input.dispatchKeyEvent", { type: "keyUp", code, key, windowsVirtualKeyCode: vk });
  console.log("ok");
}
ws.close(); process.exit(0);
