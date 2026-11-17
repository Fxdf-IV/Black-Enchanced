// Uso: node tools/cdp.mjs <porta> eval '<js>' | shot <arquivo.png> | key <KeyW> <ms> | click x,y <ms>
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
if (cmd === "click") {
  const [x, y] = (a || "460,300").split(",").map(Number);
  for (const type of ["mousePressed", "mouseReleased"]) {
    await send("Input.dispatchMouseEvent", { type, x, y, button: "left", clickCount: 1, buttons: type === "mousePressed" ? 1 : 0 });
    await new Promise(r => setTimeout(r, Number(b || 120)));
  }
  console.log("ok");
}
if (cmd === "aimfire") { // aimfire dy,steps : segura o botão e desce a mira
  const [dy, steps] = (a || "5,4").split(",").map(Number);
  let y = 300;
  await send("Input.dispatchMouseEvent", { type: "mousePressed", x: 460, y, button: "left", clickCount: 1, buttons: 1 });
  for (let i = 0; i < steps; i++) { y += dy; await send("Input.dispatchMouseEvent", { type: "mouseMoved", x: 460, y, button: "left", buttons: 1 }); await new Promise(r => setTimeout(r, 120)); }
  await new Promise(r => setTimeout(r, 400));
  await send("Input.dispatchMouseEvent", { type: "mouseReleased", x: 460, y, button: "left", clickCount: 1, buttons: 0 });
  console.log("ok");
}
ws.close(); process.exit(0);
