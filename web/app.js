import {
  checkOpfsCached,
  streamAssetsFromRemote,
  listOpfsAssetFiles,
  normalizePickedFiles,
  createBrowserVfsBridge,
} from "./opfs_loader.js";
import { BlackWebGpuRenderer } from "./webgpu_renderer.js";
import { WebRtcTransport } from "./netplay_webrtc.js";

const NO_WEAPON = 0xff;
const TACTICAL_ARMOR = 1 << 0;
const TACTICAL_PACK = 1 << 1;

const WEB_KEY_MAP = Object.freeze({
  KeyW: 1 << 0,
  KeyS: 1 << 1,
  KeyA: 1 << 2,
  KeyD: 1 << 3,
  ArrowUp: 1 << 4,
  ArrowDown: 1 << 5,
  ArrowLeft: 1 << 6,
  ArrowRight: 1 << 7,
  Enter: 1 << 8,
  ShiftLeft: 1 << 10,
  ShiftRight: 1 << 10,
  ControlLeft: 1 << 11,
  ControlRight: 1 << 11,
  Space: 1 << 12,
  KeyC: 1 << 13,
  Backspace: 1 << 13,
  KeyR: 1 << 14,
  KeyE: 1 << 15,
  KeyG: 1 << 16,
  KeyQ: 1 << 17,
  KeyV: 1 << 18,
  KeyF: 1 << 19,
  KeyY: 1 << 20,
  Digit1: 1 << 20,
  Digit2: 1 << 20,
  KeyJ: 1 << 22,
  KeyL: 1 << 23,
  KeyI: 1 << 24,
  KeyK: 1 << 25,
});
const WEB_KEY_Y = 1 << 20;

// Teclas discretas que mantêm pulso >= 120ms para o poll ~30-60Hz do XInputGetState nunca perder toques rápidos.
const DISCRETE_LATCH_CODES = new Set([
  "Enter",
  "Space",
  "Backspace",
  "KeyC",
  "KeyE",
  "KeyF",
  "KeyR",
  "KeyG",
  "KeyQ",
  "KeyV",
  "KeyY",
  "Digit1",
  "Digit2",
  "ArrowUp",
  "ArrowDown",
  "ArrowLeft",
  "ArrowRight",
]);

const DEFAULT_CATALOG = {
  maxCredits: 8000,
  armorPrice: 650,
  armorBonus: 25,
  tacticalPrice: 300,
  buyWindowTicks: 450,
  weapons: [
    { id: 0, name: "Glock 19", slot: 0, price: 0, damage: 22, mag: 19, range: 80, fireTicks: 5 },
    { id: 1, name: "DC3 Elite", slot: 0, price: 450, damage: 28, mag: 12, range: 85, fireTicks: 5 },
    { id: 2, name: "Magnum", slot: 0, price: 700, damage: 55, mag: 6, range: 100, fireTicks: 12 },
    { id: 3, name: "MP5", slot: 1, price: 1250, damage: 20, mag: 30, range: 90, fireTicks: 2 },
    { id: 4, name: "MAC-10", slot: 1, price: 1050, damage: 18, mag: 32, range: 70, fireTicks: 2 },
    { id: 5, name: "Uzi", slot: 1, price: 1100, damage: 19, mag: 32, range: 75, fireTicks: 2 },
    { id: 6, name: "P90", slot: 1, price: 1800, damage: 21, mag: 50, range: 95, fireTicks: 2 },
    { id: 7, name: "AK-47", slot: 1, price: 2700, damage: 34, mag: 30, range: 160, fireTicks: 3 },
    { id: 8, name: "M16A2", slot: 1, price: 2900, damage: 30, mag: 30, range: 175, fireTicks: 3 },
    { id: 9, name: "G36C", slot: 1, price: 2400, damage: 29, mag: 30, range: 165, fireTicks: 3 },
    { id: 10, name: "SPAS-12", slot: 1, price: 1600, damage: 75, mag: 8, range: 35, fireTicks: 15 },
    { id: 11, name: "Remington 870", slot: 1, price: 1400, damage: 85, mag: 8, range: 32, fireTicks: 18 },
    { id: 12, name: "M249 SAW", slot: 1, price: 4500, damage: 32, mag: 100, range: 170, fireTicks: 2 },
    { id: 13, name: "WA2000", slot: 1, price: 4200, damage: 95, mag: 6, range: 300, fireTicks: 20 },
    { id: 14, name: "M203", slot: 1, price: 2200, damage: 100, mag: 1, range: 120, fireTicks: 45 },
    { id: 15, name: "RPG-7", slot: 1, price: 3600, damage: 100, mag: 1, range: 200, fireTicks: 60 },
  ],
};

const BUY_RESULT_LABELS = [
  "Loadout equipado na partida.",
  "Créditos insuficientes para este item.",
  "Kit ultrapassa o teto de orçamento da sala.",
  "Item inválido para este slot.",
  "Janela de compra encerrada (tente no próximo spawn).",
  "Indisponível enquanto estiver abatido ou no fim da partida.",
];

function loadEnhancedPrefs() {
  const defaults = {
    mouseDirect: true,
    mouseSens: 0.085,
    adsSensMult: 0.7,
    invertY: false,
    skipIntro: true,
    fovDeg: 88,
    hitmarker: true,
    renderPercent: 100,
    overhaulEnabled: false,
    playerDamageMult: 1.35,
    enemyDamageMult: 1.0,
    recoilMult: 0.6,
    ammoBonusEnabled: true,
  };
  try {
    const raw = localStorage.getItem("black_enhanced_prefs_v1");
    if (!raw) return defaults;
    return { ...defaults, ...JSON.parse(raw) };
  } catch {
    return defaults;
  }
}

function saveEnhancedPrefs(prefs) {
  try {
    localStorage.setItem("black_enhanced_prefs_v1", JSON.stringify(prefs));
  } catch {}
}

const state = {
  launchTab: "solo", // "solo" | "mp"
  role: "offline",   // "offline" | "host" | "client"
  roomCode: "VEX401",
  playerName: `OP-${Math.floor(10 + Math.random() * 89)}`,
  localPlayerId: 0,
  tick: 0,
  packetsTx: 0,
  packetsRx: 0,
  transport: null,
  keys: new Set(),
  latchedUntil: new Map(), // code -> timestampMs
  webgpuReady: false,
  gpuAdapterName: "Verificando...",
  wasmAvailable: false,
  serverManifest: null,
  catalog: DEFAULT_CATALOG,
  enhanced: loadEnhancedPrefs(),
  roomRules: { csLoadout: true, fixedBudget: false, budgetCap: 8000 },
  lastRebuyKit: null,
  lastLoadoutSnapshotKey: "",
  lastBuyHeaderKey: "",
  pendingBuyCheck: null,
};

const engine = {
  bootStarted: false,
  bootState: 0,
  bootMessage: "Inativo",
  module: null,
  renderer: null,
  vfs: null,
  audioCtx: null,
  audioNode: null,
  audioPtr: 0,
  statusPtr: 0,
  weaponsPtr: 0,
  netPeerPtr: 0,
  netChPtr: 0,
  netKindPtr: 0,
  netBufPtr: 0,
  netInBufPtr: 0,
  status: null,
  mouseButtons: 0,
  pendingDx: 0,
  pendingDy: 0,
  lastMouseX: 0,
  lastMouseY: 0,
  hasLastMousePos: false,
  wheelPulseUntil: 0,
  wasmClientConfiguredPeer: 0,
  lastHelloRetryMs: 0,
  rafId: 0,
  audioTimerId: 0,
  statusTimerId: 0,
  saveSyncTimerId: 0,
  idbfsMounted: false,
  saveSyncInFlight: false,
  lastSaveFingerprint: "",
  lastHitmarkerSeq: 0,
  lastHitmarkerSoundMs: 0,
  hitmarkerTimer: 0,
  toastTimer: 0,
  activeOverlay: "launcher", // "none" | "launcher" | "options" | "buy"
  scoreboardOpen: false,
  wasInLevel: false,
};

function getHeap(mod, Ctor, field) {
  if (!mod) return null;
  if (mod.wasmMemory && (!mod[field] || mod[field].buffer !== mod.wasmMemory.buffer)) {
    return new Ctor(mod.wasmMemory.buffer);
  }
  return mod[field];
}
const getHeapU8 = (m) => getHeap(m, Uint8Array, "HEAPU8");
const getHeapU32 = (m) => getHeap(m, Uint32Array, "HEAPU32");
const getHeap32 = (m) => getHeap(m, Int32Array, "HEAP32");
const getHeapF32 = (m) => getHeap(m, Float32Array, "HEAPF32");

function logLine(tag, text) {
  const logEl = document.getElementById("event-log");
  if (!logEl) return;
  const now = new Date().toLocaleTimeString("pt-BR", { hour12: false });
  const row = document.createElement("div");
  row.className = "log-entry";
  const sTime = document.createElement("span");
  sTime.className = "log-time";
  sTime.textContent = now;
  const sTag = document.createElement("span");
  sTag.className = "log-tag";
  sTag.textContent = `[${tag}]`;
  const sMsg = document.createElement("span");
  sMsg.className = "log-msg";
  sMsg.textContent = String(text ?? "");
  row.append(sTime, sTag, sMsg);
  logEl.prepend(row);
  while (logEl.children.length > 160) logEl.removeChild(logEl.lastChild);
}

function showToast(msg, durationMs = 2600) {
  const el = document.getElementById("stage-toast");
  if (!el) return;
  el.textContent = msg;
  el.classList.remove("hidden");
  if (engine.toastTimer) clearTimeout(engine.toastTimer);
  engine.toastTimer = setTimeout(() => el.classList.add("hidden"), durationMs);
}

function weaponNameById(id) {
  if (id === NO_WEAPON || id === undefined || id === null) return "Nenhuma";
  const found = state.catalog.weapons.find((w) => w.id === id);
  return found ? found.name : `#${id}`;
}

function setBadge(id, text, level) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  if (level) el.dataset.level = level;
}

function isMultiplayerActive() {
  return state.role === "host" || state.role === "client";
}

function isInLevelNow() {
  return Boolean(engine.status?.inLevel);
}

function releaseInputFocus(focusCanvas = false) {
  if (document.activeElement && document.activeElement.tagName === "BUTTON") {
    document.activeElement.blur();
  }
  if (focusCanvas && engine.bootStarted && engine.activeOverlay === "none") {
    document.getElementById("game-canvas")?.focus();
  }
}

function pulseKey(code, durationMs = 140) {
  state.latchedUntil.set(code, performance.now() + durationMs);
  pushKeyMaskToWasm();
  if (engine.audioCtx?.state === "suspended") engine.audioCtx.resume().catch(() => {});
  document.getElementById("game-canvas")?.focus();
}

function updateLoadingOverlay() {
  const scrim = document.getElementById("overlay-loading");
  if (!scrim) return;
  const st = engine.status;
  const shouldShow =
    engine.bootStarted &&
    engine.activeOverlay === "none" &&
    (!st || st.bootState < 2 || (st.presents || 0) === 0);
  scrim.classList.toggle("hidden", !shouldShow);
  const sub = document.getElementById("loading-sub");
  if (sub && st?.bootMessage) {
    sub.textContent = st.bootMessage;
  }
}

function setOverlay(name) {
  if (name === "buy" && (!isMultiplayerActive() || !isInLevelNow())) {
    if (!isMultiplayerActive()) {
      showToast("Loadout CS está disponível apenas no modo Multiplayer Online.");
    } else {
      showToast("Entre em uma missão no menu do BLACK (ex.: Missão 1) para usar o Loadout [B].");
    }
    return;
  }

  engine.activeOverlay = name;
  const launcher = document.getElementById("overlay-launcher");
  const options = document.getElementById("overlay-options");
  const buy = document.getElementById("overlay-buy");

  launcher?.classList.toggle("hidden", name !== "launcher");
  options?.classList.toggle("hidden", name !== "options");
  buy?.classList.toggle("hidden", name !== "buy");

  if (name !== "none") {
    state.keys.clear();
    state.latchedUntil.clear();
    engine.mouseButtons = 0;
    pushKeyMaskToWasm();
    engine.module?._BlackWebPushMouseState(0, 0, 0);
    if (document.pointerLockElement) {
      document.exitPointerLock?.();
    }
  }
  if (name === "buy") {
    refreshCatalogFromWasm();
    renderBuyMenu(true);
  }
  if (name === "options") {
    syncEnhancedControlsUi();
    syncRoomFieldsToUi();
  }
  updateViewportChrome();
  updateLoadingOverlay();
}

function setScoreboardVisible(show) {
  if (!isMultiplayerActive()) {
    engine.scoreboardOpen = false;
    document.getElementById("overlay-scoreboard")?.classList.add("hidden");
    return;
  }
  engine.scoreboardOpen = show && engine.bootStarted && engine.activeOverlay === "none";
  const sb = document.getElementById("overlay-scoreboard");
  sb?.classList.toggle("hidden", !engine.scoreboardOpen);
  if (engine.scoreboardOpen) updateScoreboard();
}

function switchLauncherTab(tab) {
  state.launchTab = tab === "mp" ? "mp" : "solo";
  const tabSolo = document.getElementById("tab-solo");
  const tabMp = document.getElementById("tab-multiplayer");
  const pSolo = document.getElementById("panel-solo");
  const pMp = document.getElementById("panel-mp");

  tabSolo?.classList.toggle("active", state.launchTab === "solo");
  tabMp?.classList.toggle("active", state.launchTab === "mp");
  tabSolo?.setAttribute("aria-selected", String(state.launchTab === "solo"));
  tabMp?.setAttribute("aria-selected", String(state.launchTab === "mp"));
  pSolo?.classList.toggle("hidden", state.launchTab !== "solo");
  pMp?.classList.toggle("hidden", state.launchTab !== "mp");
}

function updateViewportChrome() {
  const st = engine.status;
  const mp = isMultiplayerActive();
  const inLvl = Boolean(st?.inLevel);
  const presenting = engine.bootStarted && Boolean(st && st.bootState >= 2 && (st.presents || 0) > 0);
  const gameCanvas = document.getElementById("game-canvas");
  const locked = document.pointerLockElement === gameCanvas;

  // Loadout e placar só existem quando Multiplayer Online está ativo
  document.getElementById("btn-open-buy")?.classList.toggle("hidden", !(mp && inLvl));
  document.getElementById("btn-open-buy-from-opt")?.classList.toggle("hidden", !(mp && inLvl));
  document.getElementById("hud-strip")?.classList.toggle("hidden", !(mp && inLvl && engine.activeOverlay === "none"));
  document.getElementById("menu-assist-bar")?.classList.toggle(
    "hidden",
    !(presenting && !inLvl && engine.activeOverlay === "none")
  );
  document.getElementById("btn-assist-mp-hint")?.classList.toggle("hidden", !mp);

  const ptrHint = document.getElementById("ptr-hint");
  if (ptrHint) {
    const showPtr = presenting && inLvl && engine.activeOverlay === "none" && !locked;
    ptrHint.classList.toggle("hidden", !showPtr);
  }

  // Rodapé muda dinamicamente conforme Solo vs Multiplayer e Menu vs Fase
  const footerKeys = document.getElementById("controls-footer-keys");
  if (footerKeys) {
    const item = (k, lbl) => `<span class="hint-item"><span class="key-hint">${k}</span> ${lbl}</span>`;
    if (engine.bootStarted && !inLvl) {
      footerKeys.innerHTML = [
        item("Enter", "Start / Pular Intro"),
        item("Espaço", "Confirmar (A)"),
        item("Setas / WASD", "Navegar"),
        item("Backspace", "Voltar (B)"),
        item("F10", "Opções"),
        mp ? item("B", "Loadout (na missão)") : "",
      ].join("");
    } else if (mp) {
      footerKeys.innerHTML = [
        item("WASD", "Mover"),
        item("Mouse", "Mirar"),
        item("Esq/Dir", "Atirar / Zoom"),
        item("1 / 2 / Roda", "Arma"),
        item("B", "Loja CS"),
        item("Tab", "Placar"),
        item("Esc", "Cursor"),
        item("F10", "Opções"),
      ].join("");
    } else {
      footerKeys.innerHTML = [
        item("WASD", "Mover"),
        item("Mouse", "Mirar"),
        item("Esq/Dir", "Atirar / Zoom"),
        item("R", "Recarregar"),
        item("E / Q / G", "Arma / Granada / Modo"),
        item("Esc", "Cursor"),
        item("F10", "Opções"),
      ].join("");
    }
  }
}

async function detectBrowserCapabilities() {
  let gpuOk = false;
  let gpuLabel = "Indisponível";
  if (navigator.gpu) {
    try {
      const adapter = await navigator.gpu.requestAdapter();
      if (adapter) {
        gpuOk = true;
        const info = adapter.info || {};
        gpuLabel = info.description || info.vendor || "WebGPU Ativo";
      }
    } catch {
      gpuOk = false;
    }
  }
  state.webgpuReady = gpuOk;
  state.gpuAdapterName = gpuLabel;
  setBadge("cap-webgpu", gpuOk ? "WEBGPU OK" : "SEM WEBGPU", gpuOk ? "ok" : "warn");
  setBadge("cap-webrtc", typeof RTCPeerConnection !== "undefined" ? "P2P OK" : "SEM P2P", typeof RTCPeerConnection !== "undefined" ? "ok" : "warn");
}

function readRoomConfigFromUi() {
  const ecoMode = document.getElementById("mp-economy-mode")?.value || "dynamic";
  const capVal = Number(document.getElementById("mp-budget-cap")?.value || 8000) || 8000;
  state.roomRules = {
    csLoadout: true,
    fixedBudget: ecoMode === "fixed",
    budgetCap: Math.max(450, Math.min(30000, capVal)),
  };
  return state.roomRules;
}

function syncRoomFieldsToUi() {
  const setVal = (id, val) => {
    const el = document.getElementById(id);
    if (el && el.value !== val) el.value = val;
  };
  setVal("mp-room-code", state.roomCode);
  setVal("opt-room-code", state.roomCode);
  setVal("mp-player-name", state.playerName);
  setVal("opt-player-name", state.playerName);
}

function readIdentityFromInputs(preferOptionsModal = false) {
  const codeId = preferOptionsModal ? "opt-room-code" : "mp-room-code";
  const nameId = preferOptionsModal ? "opt-player-name" : "mp-player-name";
  const rawCode = document.getElementById(codeId)?.value || state.roomCode;
  const rawName = document.getElementById(nameId)?.value || state.playerName;
  state.roomCode = rawCode.trim().toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 6) || "VEX401";
  state.playerName = rawName.trim().slice(0, 16) || "KELLAR";
  syncRoomFieldsToUi();
}

function buildInviteUrl() {
  const url = new URL(window.location.href);
  url.searchParams.set("room", state.roomCode);
  return url.toString();
}

async function copyInviteLink(preferOptionsModal = false) {
  readIdentityFromInputs(preferOptionsModal);
  const link = buildInviteUrl();
  try {
    await navigator.clipboard.writeText(link);
    showToast(`Link de convite copiado (${state.roomCode}): ${link}`);
  } catch {
    showToast(`Código da sala: ${state.roomCode} (?room=${state.roomCode})`);
  }
}

function pushEnhancedConfigToWasm() {
  saveEnhancedPrefs(state.enhanced);
  const mod = engine.module;
  if (!mod) return;
    if (mod._BlackWebSetEnhancedConfig) {
    mod._BlackWebSetEnhancedConfig(
      state.enhanced.mouseDirect ? 1 : 0,
      Number(state.enhanced.mouseSens),
      Number(state.enhanced.adsSensMult),
      state.enhanced.invertY ? 1 : 0,
      Number(state.enhanced.fovDeg),
      state.enhanced.hitmarker ? 1 : 0,
      state.enhanced.skipIntro !== false ? 1 : 0
    );
  }
  if (mod._BlackWebSetRenderPercent) {
    mod._BlackWebSetRenderPercent(Math.round(Number(state.enhanced.renderPercent || 100)));
  }
  if (mod._BlackWebSetOverhaulConfig) {
    mod._BlackWebSetOverhaulConfig(
      state.enhanced.overhaulEnabled ? 1 : 0,
      Number(state.enhanced.playerDamageMult || 1.35),
      Number(state.enhanced.enemyDamageMult || 1.0),
      Number(state.enhanced.recoilMult || 0.6),
      state.enhanced.ammoBonusEnabled ? 1 : 0
    );
  }
}

function syncEnhancedControlsUi() {
  const c = state.enhanced;
  const launchEnhanced = document.getElementById("launch-enhanced-toggle");
  const md = document.getElementById("opt-mouse-direct");
  const ms = document.getElementById("opt-mouse-sens");
  const msv = document.getElementById("opt-mouse-sens-val");
  const ads = document.getElementById("opt-ads-sens");
  const adsv = document.getElementById("opt-ads-sens-val");
  const fov = document.getElementById("opt-fov");
  const fovv = document.getElementById("opt-fov-val");
  const rp = document.getElementById("opt-render-percent");
  const rpv = document.getElementById("opt-render-percent-val");
  const iy = document.getElementById("opt-invert-y");
  const si = document.getElementById("opt-skip-intro");
  const hm = document.getElementById("opt-hitmarker");
  const ov = document.getElementById("opt-overhaul-enabled");
  const pd = document.getElementById("opt-player-dmg");
  const pdv = document.getElementById("opt-player-dmg-val");
  const ed = document.getElementById("opt-enemy-dmg");
  const edv = document.getElementById("opt-enemy-dmg-val");
  const rm = document.getElementById("opt-recoil-mult");
  const rmv = document.getElementById("opt-recoil-mult-val");
  const ab = document.getElementById("opt-ammo-bonus");

  if (launchEnhanced) launchEnhanced.checked = Boolean(c.mouseDirect);
  if (md) md.checked = Boolean(c.mouseDirect);
  if (ms) ms.value = String(c.mouseSens);
  if (msv) msv.textContent = `${Number(c.mouseSens).toFixed(3)}°/px`;
  if (ads) ads.value = String(c.adsSensMult);
  if (adsv) adsv.textContent = `${Number(c.adsSensMult).toFixed(2)}x`;
  if (fov) fov.value = String(c.fovDeg);
  if (fovv) fovv.textContent = `${Math.round(c.fovDeg)}°`;
  if (rp) rp.value = String(c.renderPercent || 100);
  if (rpv) rpv.textContent = `${Math.round(Number(c.renderPercent || 100))}%`;
  if (iy) iy.checked = Boolean(c.invertY);
  if (si) si.checked = Boolean(c.skipIntro !== false);
  if (hm) hm.checked = Boolean(c.hitmarker);
  if (ov) ov.checked = Boolean(c.overhaulEnabled);
  if (pd) pd.value = String(c.playerDamageMult ?? 1.35);
  if (pdv) pdv.textContent = `${Number(c.playerDamageMult ?? 1.35).toFixed(2)}x`;
  if (ed) ed.value = String(c.enemyDamageMult ?? 1.0);
  if (edv) edv.textContent = `${Number(c.enemyDamageMult ?? 1.0).toFixed(2)}x`;
  if (rm) rm.value = String(c.recoilMult ?? 0.6);
  if (rmv) rmv.textContent = `${Number(c.recoilMult ?? 0.6).toFixed(2)}x`;
  if (ab) ab.checked = Boolean(c.ammoBonusEnabled);
}

function refreshCatalogFromWasm() {
  const mod = engine.module;
  if (!mod || !mod._BlackWebGetWeaponsJson || !engine.weaponsPtr) return;
  try {
    const len = mod._BlackWebGetWeaponsJson(engine.weaponsPtr, 4096);
    if (len <= 0) return;
    const parsed = JSON.parse(mod.UTF8ToString(engine.weaponsPtr, len));
    if (Array.isArray(parsed.weapons) && parsed.weapons.length > 0) {
      state.catalog = parsed;
    }
  } catch {}
}

function syncWasmRole(roleName) {
  const mod = engine.module;
  if (!mod || !mod.ccall) return;
  engine.wasmClientConfiguredPeer = 0;
  const rules = state.roomRules;
  if (mod._BlackWebSetRoleEx) {
    if (roleName === "offline") {
      mod.ccall(
        "BlackWebSetRoleEx",
        "number",
        ["number", "string", "number", "number", "number", "number"],
        [0, state.playerName, 0, 0, 0, 8000]
      );
    } else if (roleName === "host") {
      mod.ccall(
        "BlackWebSetRoleEx",
        "number",
        ["number", "string", "number", "number", "number", "number"],
        [1, state.playerName, 0, 1, rules.fixedBudget ? 1 : 0, rules.budgetCap]
      );
      if (state.transport) {
        for (const peerId of state.transport.peers.keys()) {
          mod._BlackWebPushIncomingNet(0, Number(peerId) >>> 0, 0, 0, 0);
        }
      }
    } else if (roleName === "client") {
      const hostPeer = Number(state.transport?.hostPeerId || 0) >>> 0;
      if (hostPeer > 0) {
        engine.wasmClientConfiguredPeer = hostPeer;
        mod.ccall(
          "BlackWebSetRoleEx",
          "number",
          ["number", "string", "number", "number", "number", "number"],
          [2, state.playerName, hostPeer, 1, rules.fixedBudget ? 1 : 0, rules.budgetCap]
        );
        mod._BlackWebPushIncomingNet(0, hostPeer, 0, 0, 0);
      }
    }
    return;
  }

  if (roleName === "offline") {
    mod.ccall("BlackWebSetRole", "number", ["number", "string", "number"], [0, state.playerName, 0]);
  } else if (roleName === "host") {
    mod.ccall("BlackWebSetRole", "number", ["number", "string", "number"], [1, state.playerName, 0]);
  } else if (roleName === "client") {
    const hostPeer = Number(state.transport?.hostPeerId || 0) >>> 0;
    if (hostPeer > 0) {
      engine.wasmClientConfiguredPeer = hostPeer;
      mod.ccall("BlackWebSetRole", "number", ["number", "string", "number"], [2, state.playerName, hostPeer]);
    }
  }
}

function startSession(preferredRole, fromOptionsModal = false) {
  if (state.transport) {
    state.transport.close();
    state.transport = null;
  }
  readIdentityFromInputs(fromOptionsModal);
  const signalingUrl = (document.getElementById("signaling-url-input")?.value || "").trim();
  const roomRules = readRoomConfigFromUi();

  state.packetsTx = 0;
  state.packetsRx = 0;

  if (preferredRole === "offline") {
    state.role = "offline";
    state.localPlayerId = 0;
    if (engine.activeOverlay === "buy") setOverlay("none");
    setScoreboardVisible(false);
    syncWasmRole("offline");
    logLine("MODO", "Campanha Solo selecionada (OfflineCampaign intacto).");
    updateTelemetryUi();
    updateViewportChrome();
    return;
  }

  state.role = preferredRole === "host" ? "host" : "client";
  state.localPlayerId = state.role === "host" ? 0 : 1;

  state.transport = new WebRtcTransport({
    roomCode: state.roomCode,
    signalingUrl,
    roomMeta: roomRules,
    onLog: (msg) => logLine("WEBRTC", msg),
    onEvent: (ev) => handleTransportEvent(ev),
  });

  state.transport.start(preferredRole);
  syncWasmRole(preferredRole);
  updateTelemetryUi();
  updateViewportChrome();
}

function handleTransportEvent(ev) {
  if (ev.type === "room_meta" && ev.meta) {
    state.roomRules = {
      csLoadout: Boolean(ev.meta.csLoadout ?? true),
      fixedBudget: Boolean(ev.meta.fixedBudget),
      budgetCap: Number(ev.meta.budgetCap || 8000),
    };
    const ecoEl = document.getElementById("mp-economy-mode");
    const capEl = document.getElementById("mp-budget-cap");
    if (ecoEl) ecoEl.value = state.roomRules.fixedBudget ? "fixed" : "dynamic";
    if (capEl) capEl.value = String(state.roomRules.budgetCap);
    logLine("WEBRTC", `Regras da sala ${state.roomCode} recebidas (teto $${state.roomRules.budgetCap}).`);
    updateTelemetryUi();
    return;
  }

  if (ev.type === "role") {
    state.role = ev.role;
    syncWasmRole(ev.role);
    updateTelemetryUi();
    updateViewportChrome();
    return;
  }

  if (ev.type === "connected") {
    const peerId = Number(ev.peer) >>> 0;
    logLine("WEBRTC", `DataChannel P2P ativo com par #${peerId} na sala ${state.roomCode}.`);
    if (engine.module && state.role !== "offline") {
      if (state.role === "client" && engine.wasmClientConfiguredPeer !== peerId) {
        syncWasmRole("client");
      } else {
        engine.module._BlackWebPushIncomingNet(0, peerId, 0, 0, 0);
      }
    }
    updateTelemetryUi();
    return;
  }

  if (ev.type === "disconnected") {
    const peerId = Number(ev.peer) >>> 0;
    if (engine.module && state.role !== "offline") {
      engine.module._BlackWebPushIncomingNet(1, peerId, 0, 0, 0);
    }
    updateTelemetryUi();
    return;
  }

  if (ev.type === "packet" && engine.module && state.role !== "offline" && engine.netInBufPtr) {
    state.packetsRx += 1;
    const copyLen = Math.min(2048, ev.payload.byteLength);
    getHeapU8(engine.module).set(ev.payload.subarray(0, copyLen), engine.netInBufPtr);
    const ch = ev.channel === "unreliable" ? 1 : 0;
    engine.module._BlackWebPushIncomingNet(2, Number(ev.peer) >>> 0, ch, engine.netInBufPtr, copyLen);
  }
}

// ---- Loja CS (exclusiva de Multiplayer dentro de fase) ----

function currentLoadoutNumbers() {
  const st = engine.status;
  if (st && engine.module) {
    return {
      primary: st.primaryWeapon ?? NO_WEAPON,
      secondary: st.secondaryWeapon ?? 0,
      tactical: st.tacticalFlags ?? 0,
      credits: st.credits ?? 0,
      maxCredits: st.maxCredits || state.catalog.maxCredits || 8000,
      kitCost: st.kitCost ?? 0,
      buyTicksLeft: st.buyTicksLeft ?? 0,
      fixedBudget: Boolean(st.fixedBudget),
      lastResult: st.lastLoadoutResult ?? 0,
    };
  }
  return {
    primary: NO_WEAPON,
    secondary: 0,
    tactical: 0,
    credits: 0,
    maxCredits: state.roomRules.budgetCap,
    kitCost: 0,
    buyTicksLeft: 0,
    fixedBudget: state.roomRules.fixedBudget,
    lastResult: 0,
  };
}

function submitLoadoutPurchase(nextPrimary, nextSecondary, nextTactical) {
  if (!isMultiplayerActive() || !isInLevelNow()) return;
  const feedback = document.getElementById("buy-feedback");
  if (nextPrimary !== NO_WEAPON || nextTactical !== 0 || nextSecondary !== 0) {
    state.lastRebuyKit = { primary: nextPrimary, secondary: nextSecondary, tactical: nextTactical };
  }
  if (!engine.module?._BlackWebBuyLoadout) return;

  const before = currentLoadoutNumbers();
  const ok = engine.module._BlackWebBuyLoadout(nextPrimary & 0xff, nextSecondary & 0xff, nextTactical & 0xff);
  if (state.role === "client") {
    if (!ok) {
      state.pendingBuyCheck = null;
      if (feedback) {
        feedback.textContent = "Falha ao enviar pedido ao host.";
        feedback.dataset.tone = "err";
      }
      return;
    }
    state.pendingBuyCheck = {
      credits: before.credits,
      primary: before.primary,
      secondary: before.secondary,
      tactical: before.tactical,
      sentAt: performance.now(),
    };
    if (feedback) {
      feedback.textContent = "Pedido enviado ao host...";
      feedback.dataset.tone = "warn";
    }
    setTimeout(() => {
      pollWasmStatus();
      renderBuyMenu(false);
    }, 90);
    return;
  }

  setTimeout(() => {
    pollWasmStatus();
    const cur = currentLoadoutNumbers();
    if (feedback) {
      feedback.textContent = BUY_RESULT_LABELS[cur.lastResult] || (ok ? BUY_RESULT_LABELS[0] : "Compra recusada.");
      feedback.dataset.tone = ok && cur.lastResult === 0 ? "ok" : "err";
    }
    renderBuyMenu(true);
  }, 50);
}

function updateBuyMenuHeader(cur) {
  const creditsEl = document.getElementById("buy-credits-val");
  const capEl = document.getElementById("buy-cap-val");
  const kitEl = document.getElementById("buy-kit-cost-val");
  const winEl = document.getElementById("buy-window-val");
  const sumEl = document.getElementById("buy-current-kit");

  if (creditsEl) creditsEl.textContent = `$${cur.credits}`;
  if (capEl) capEl.textContent = `$${cur.maxCredits}`;
  if (kitEl) kitEl.textContent = `$${cur.kitCost}`;
  if (winEl) {
    winEl.textContent =
      cur.buyTicksLeft === 0xffff ? "LIVRE" : cur.buyTicksLeft > 0 ? `${Math.ceil(cur.buyTicksLeft / 30)}s` : "FECHADA";
  }

  const extras = [];
  if (cur.tactical & TACTICAL_ARMOR) extras.push("Colete +25 HP");
  if (cur.tactical & TACTICAL_PACK) extras.push("Kit Tático/Silenciador");
  if (sumEl) {
    sumEl.innerHTML = `Primária: <strong>${weaponNameById(cur.primary)}</strong><br />Secundária: <strong>${weaponNameById(cur.secondary)}</strong><br />Proteção: <strong>${extras.length ? extras.join(" + ") : "Padrão"}</strong>`;
  }
}

const BLACK_LEVEL_NAMES = {
  0: "Missão 1 · Streets",
  1: "Missão 2 · Border",
  2: "Missão 3 · Foundry",
  3: "Missão 4 · Asylum",
  4: "Missão 5 · Dockyard",
  5: "Missão 6 · Bridge",
  6: "Missão 7 · Gulag",
  7: "Missão 8 · Spetriniv",
};

function levelLabel(id) {
  return BLACK_LEVEL_NAMES[Number(id)] || `Fase #${Number(id) || 0}`;
}

function buildBuyButton({ hotkey, name, meta, price, equipped, affordable, available = true, onClick }) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = `buy-item${equipped ? " equipped" : ""}${available ? "" : " unavailable"}`;
  btn.disabled = !available || (!equipped && !affordable);
  if (hotkey && available) btn.dataset.buyHotkey = String(hotkey);

  const left = document.createElement("div");
  left.className = "buy-item-left";
  const title = document.createElement("div");
  title.className = "buy-item-name";
  if (hotkey) {
    const tag = document.createElement("span");
    tag.className = "hotkey-tag";
    tag.textContent = String(hotkey);
    title.appendChild(tag);
  }
  title.append(name);

  const sub = document.createElement("div");
  sub.className = "buy-item-meta";
  sub.textContent = meta;
  left.append(title, sub);

  const right = document.createElement("span");
  right.className = "buy-item-price";
  right.textContent = !available ? "N/D" : equipped ? "EQUIPADO" : price === 0 ? "GRÁTIS" : `$${price}`;

  btn.append(left, right);
  btn.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (available) onClick();
  });
  return btn;
}

function renderBuyMenu(forceButtons = false) {
  if (!isMultiplayerActive()) return;
  const colSec = document.getElementById("buy-col-secondary");
  const colPri = document.getElementById("buy-col-primary");
  const colTac = document.getElementById("buy-col-tactical");
  if (!colSec || !colPri || !colTac) return;

  const cur = currentLoadoutNumbers();
  updateBuyMenuHeader(cur);

  const windowOpen = cur.buyTicksLeft > 0;
  const availMask = state.catalog.availableMask ?? 0xffffffff;
  const engCnt = state.catalog.engineWeaponCount ?? 0;
  const btnKey = `${cur.credits}-${cur.primary}-${cur.secondary}-${cur.tactical}-${cur.lastResult}-${windowOpen ? 1 : 0}-${cur.fixedBudget ? 1 : 0}-${state.catalog.weapons.length}-${availMask}-${engCnt}`;
  state.lastBuyHeaderKey = `${btnKey}-${Math.floor((cur.buyTicksLeft || 0) / 30)}`;
  if (!forceButtons && btnKey === state.lastLoadoutSnapshotKey && colPri.childElementCount > 0) {
    return;
  }
  state.lastLoadoutSnapshotKey = btnKey;
  colSec.replaceChildren();
  colPri.replaceChildren();
  colTac.replaceChildren();

  const secondaries = state.catalog.weapons.filter((w) => w.slot === 0);
  const primaries = state.catalog.weapons.filter((w) => w.slot !== 0);

  secondaries.forEach((w, idx) => {
    const equipped = cur.secondary === w.id;
    const available = w.available !== false;
    const curSecPrice = state.catalog.weapons.find((x) => x.id === cur.secondary)?.price || 0;
    const affordable = available && windowOpen && (cur.fixedBudget ? cur.kitCost - curSecPrice + w.price <= cur.maxCredits : w.price <= cur.credits);
    colSec.appendChild(
      buildBuyButton({
        hotkey: idx < 3 ? String(idx + 1) : "",
        name: w.name,
        meta: available ? `DMG ${w.damage} · MAG ${w.mag}${w.tag ? ` · ${w.tag}` : ""}` : "Indisponível nesta fase",
        price: w.price,
        equipped,
        affordable,
        available,
        onClick: () => submitLoadoutPurchase(cur.primary, w.id, cur.tactical),
      })
    );
  });

  primaries.forEach((w, idx) => {
    const equipped = cur.primary === w.id;
    const available = w.available !== false;
    const curPrimPrice = cur.primary !== NO_WEAPON ? state.catalog.weapons.find((x) => x.id === cur.primary)?.price || 0 : 0;
    const affordable = available && windowOpen && (cur.fixedBudget ? cur.kitCost - curPrimPrice + w.price <= cur.maxCredits : w.price <= cur.credits);
    colPri.appendChild(
      buildBuyButton({
        hotkey: idx < 6 ? String(idx + 4) : "",
        name: w.name,
        meta: available ? `DMG ${w.damage} · ${w.mag} balas · ${w.range}m${w.tag ? ` · ${w.tag}` : ""}` : "Indisponível nesta fase",
        price: w.price,
        equipped,
        affordable,
        available,
        onClick: () => submitLoadoutPurchase(w.id, cur.secondary, cur.tactical),
      })
    );
  });

  const hasArmor = (cur.tactical & TACTICAL_ARMOR) !== 0;
  const armorPrice = state.catalog.armorPrice || 650;
  colTac.appendChild(
    buildBuyButton({
      hotkey: "",
      name: `Colete Balístico (+${state.catalog.armorBonus || 25} HP)`,
      meta: "Proteção extra ao equipar e spawnar",
      price: armorPrice,
      equipped: hasArmor,
      affordable: windowOpen && armorPrice <= cur.credits,
      onClick: () => submitLoadoutPurchase(cur.primary, cur.secondary, cur.tactical | TACTICAL_ARMOR),
    })
  );

  const hasPack = (cur.tactical & TACTICAL_PACK) !== 0;
  const tacPrice = state.catalog.tacticalPrice || 300;
  colTac.appendChild(
    buildBuyButton({
      hotkey: "",
      name: "Pacote Tático (Silenciador / Granada)",
      meta: "Acopla silenciador na arma equipada",
      price: tacPrice,
      equipped: hasPack,
      affordable: windowOpen && tacPrice <= cur.credits,
      onClick: () => submitLoadoutPurchase(cur.primary, cur.secondary, cur.tactical | TACTICAL_PACK),
    })
  );
}

// ---- Loop de entrada e status WASM ----

function computeKeyMask() {
  const now = performance.now();
  let mask = now < engine.wheelPulseUntil ? WEB_KEY_Y : 0;
  for (const code of state.keys) {
    const bit = WEB_KEY_MAP[code];
    if (bit) mask |= bit;
  }
  for (const [code, until] of state.latchedUntil.entries()) {
    if (now < until) {
      const bit = WEB_KEY_MAP[code];
      if (bit) mask |= bit;
    } else {
      state.latchedUntil.delete(code);
    }
  }
  return mask >>> 0;
}

function pushKeyMaskToWasm() {
  if (!engine.module) return;
  engine.module._BlackWebPushKeyState(computeKeyMask());
}

function pollBrowserGamepad() {
  const mod = engine.module;
  if (!mod || typeof navigator.getGamepads !== "function") return;
  const pads = navigator.getGamepads();
  const pad = pads ? Array.from(pads).find((p) => p && p.connected) : null;
  if (!pad) {
    mod._BlackWebPushGamepadState(0, 0, 0, 0, 0, 0, 0, 0);
    return;
  }
  let btns = 0;
  const b = pad.buttons || [];
  if (b[12]?.pressed) btns |= 0x01;
  if (b[13]?.pressed) btns |= 0x02;
  if (b[14]?.pressed) btns |= 0x04;
  if (b[15]?.pressed) btns |= 0x08;
  if (b[9]?.pressed) btns |= 0x10;
  if (b[8]?.pressed) btns |= 0x20;
  if (b[10]?.pressed) btns |= 0x40;
  if (b[11]?.pressed) btns |= 0x80;

  const byteVal = (idx) => Math.max(0, Math.min(255, Math.round((b[idx]?.value || 0) * 255))) & 0xff;
  const aLow = (byteVal(0) | (byteVal(1) << 8) | (byteVal(2) << 16) | (byteVal(3) << 24)) >>> 0;
  const aHigh = (byteVal(4) | (byteVal(5) << 8) | (byteVal(6) << 16) | (byteVal(7) << 24)) >>> 0;
  const axisI16 = (val, invert = false) => {
    const v = Math.abs(val || 0) < 0.08 ? 0 : invert ? -val : val;
    return Math.max(-32767, Math.min(32767, Math.round(v * 32767))) | 0;
  };
  mod._BlackWebPushGamepadState(
    1,
    btns,
    aLow,
    aHigh,
    axisI16(pad.axes?.[0] || 0, false),
    axisI16(pad.axes?.[1] || 0, true),
    axisI16(pad.axes?.[2] || 0, false),
    axisI16(pad.axes?.[3] || 0, true)
  );
}

function pumpWebAudio() {
  const mod = engine.module;
  if (!mod || !engine.audioNode || !engine.audioPtr) return;
  for (let i = 0; i < 4; i++) {
    const frames = mod._BlackWebDrainAudio(engine.audioPtr, 2048);
    if (!frames || frames <= 0) break;
    const f32 = getHeapF32(mod);
    if (!f32) break;
    const base = engine.audioPtr >> 2;
    const chunk = f32.slice(base, base + frames * 2);
    engine.audioNode.port.postMessage(chunk, [chunk.buffer]);
  }
}

function pumpOutgoingWasmNet() {
  const mod = engine.module;
  if (!mod || !state.transport || state.role === "offline" || !engine.netBufPtr) return;
  for (let i = 0; i < 32; i++) {
    const len = mod._BlackWebPopOutgoingNet(
      engine.netPeerPtr,
      engine.netChPtr,
      engine.netBufPtr,
      2048,
      engine.netKindPtr
    );
    if (len < 0) break;
    const h32 = getHeap32(mod);
    const hu32 = getHeapU32(mod);
    if (!h32 || !hu32) break;
    const peer = hu32[engine.netPeerPtr >> 2];
    const ch = hu32[engine.netChPtr >> 2];
    const kind = h32[engine.netKindPtr >> 2];
    if (kind === 1) continue;
    if (len > 0) {
      const payload = getHeapU8(mod).slice(engine.netBufPtr, engine.netBufPtr + len);
      if (state.transport.send(peer, ch === 1 ? "unreliable" : "reliable", payload)) {
        state.packetsTx += 1;
      }
    }
  }
}

function playHitmarkerSound() {
  const ctx = engine.audioCtx;
  if (!ctx || ctx.state !== "running") return;
  const nowMs = performance.now();
  if (nowMs - engine.lastHitmarkerSoundMs < 45) return;
  engine.lastHitmarkerSoundMs = nowMs;

  try {
    const t = ctx.currentTime;
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = "triangle";
    osc.frequency.setValueAtTime(1950, t);
    osc.frequency.exponentialRampToValueAtTime(1100, t + 0.032);
    gain.gain.setValueAtTime(0.08, t);
    gain.gain.exponentialRampToValueAtTime(0.001, t + 0.036);
    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.start(t);
    osc.stop(t + 0.038);
  } catch {}
}

function triggerHitmarkerFlash() {
  const hm = document.getElementById("hitmarker-el");
  if (!hm || !state.enhanced.hitmarker) return;
  hm.dataset.hitSeq = String(engine.lastHitmarkerSeq);
  hm.classList.add("active");
  playHitmarkerSound();
  if (engine.lastHitmarkerSeq <= 5) {
    logLine("COMBAT", `Hitmarker #${engine.lastHitmarkerSeq} (WebAudio=${engine.audioCtx?.state || "none"}).`);
  }
  if (engine.hitmarkerTimer) clearTimeout(engine.hitmarkerTimer);
  engine.hitmarkerTimer = setTimeout(() => hm.classList.remove("active"), 170);
}

function getSaveTreeStats(mod, rootDir = "/black_assets/_save") {
  const empty = { files: 0, totalBytes: 0, maxMtime: 0, fingerprint: "" };
  if (!mod?.FS) return empty;
  let files = 0;
  let totalBytes = 0;
  let maxMtime = 0;
  const walk = (dir) => {
    let entries;
    try {
      entries = mod.FS.readdir(dir);
    } catch {
      return;
    }
    for (const name of entries) {
      if (name === "." || name === "..") continue;
      const full = `${dir}/${name}`;
      try {
        const st = mod.FS.stat(full);
        if (mod.FS.isDir(st.mode)) {
          walk(full);
        } else {
          files += 1;
          totalBytes += st.size || 0;
          const m = st.mtime ? Number(st.mtime) : 0;
          if (m > maxMtime) maxMtime = m;
        }
      } catch {}
    }
  };
  walk(rootDir);
  return { files, totalBytes, maxMtime, fingerprint: `${files}:${totalBytes}:${maxMtime}` };
}

async function mountCampaignSaveIdbfs(mod) {
  if (!mod?.FS || engine.idbfsMounted) return;
  try {
    try {
      mod.FS.mkdir("/black_assets");
    } catch {}
    try {
      mod.FS.mkdir("/black_assets/_save");
    } catch {}
    if (!mod.FS) {
      logLine("SAVE", "FS indisponível neste runtime Emscripten (usando MEMFS temporário).");
      return;
    }
    const idbfs = mod.IDBFS || mod.FS.filesystems?.IDBFS;
    if (!idbfs) {
      logLine("SAVE", "IDBFS não exposto no runtime Emscripten atual (usando MEMFS temporário).");
      return;
    }
    mod.FS.mount(idbfs, {}, "/black_assets/_save");
    const syncErr = await new Promise((resolve) => mod.FS.syncfs(true, (err) => resolve(err || null)));
    if (syncErr) throw syncErr;
    engine.idbfsMounted = true;
    const stats = getSaveTreeStats(mod);
    engine.lastSaveFingerprint = stats.fingerprint;
    logLine(
      "SAVE",
      `Persistência de Campanha (IDBFS) pronta em /black_assets/_save (${stats.files} arquivo(s), ${stats.totalBytes} B restaurados).`
    );
  } catch (err) {
    logLine("SAVE", `IDBFS indisponível (usando MEMFS): ${err?.message || err}`);
  }
}

function flushCampaignSaveIdbfs(force = false) {
  const mod = engine.module;
  if (!mod?.FS || !engine.idbfsMounted || engine.saveSyncInFlight) return;
  const stats = getSaveTreeStats(mod);
  const changed = stats.fingerprint !== engine.lastSaveFingerprint;
  if (!force && !changed) return;
  engine.lastSaveFingerprint = stats.fingerprint;
  engine.saveSyncInFlight = true;
  try {
    mod.FS.syncfs(false, (err) => {
      engine.saveSyncInFlight = false;
      if (!err && changed && stats.files > 0) {
        logLine("SAVE", `IndexedDB atualizado (${stats.files} arquivo(s), ${stats.totalBytes} B).`);
      }
    });
  } catch {
    engine.saveSyncInFlight = false;
  }
}

function pollWasmStatus() {
  const mod = engine.module;
  if (!mod || !engine.statusPtr) return;
  try {
    const written = mod._BlackWebGetStatusJson(engine.statusPtr, 4096);
    if (written <= 0) return;
    const st = JSON.parse(mod.UTF8ToString(engine.statusPtr, written));
    const prevState = engine.status?.bootState;
    engine.status = st;
    engine.bootState = st.bootState;
    engine.bootMessage = st.bootMessage || "";

    if (prevState !== st.bootState && st.bootMessage) {
      logLine("BOOT", st.bootMessage);
    }
    if (st.bootState < 0 && engine.bootStarted) {
      engine.bootStarted = false;
      setOverlay("launcher");
      showToast(st.bootMessage || "Erro ao iniciar BLACK.");
    }
    if (st.bootState >= 2) {
      setBadge("cap-webgpu", st.gpuReady ? "WEBGPU ATIVO" : "WASM ATIVO", st.gpuReady ? "ok" : "warn");
    }
    if ((st.hitmarkerSeq || 0) > engine.lastHitmarkerSeq) {
      engine.lastHitmarkerSeq = st.hitmarkerSeq;
      triggerHitmarkerFlash();
    }

    // Se Host iniciou WASM depois do Cliente já estar na sala P2P, re-envia Hello até unir.
    if (
      engine.bootStarted &&
      state.role === "client" &&
      !st.clientJoined &&
      state.transport?.peers?.size > 0 &&
      performance.now() - engine.lastHelloRetryMs > 1200
    ) {
      engine.lastHelloRetryMs = performance.now();
      syncWasmRole("client");
    }

    // Ao sair da fase para o menu do jogo, garante que pointer lock e Loja CS fechem limpos.
    if (engine.wasInLevel && !st.inLevel) {
      if (document.pointerLockElement) document.exitPointerLock?.();
      if (engine.activeOverlay === "buy") setOverlay("none");
    }
    engine.wasInLevel = Boolean(st.inLevel);

    if (state.pendingBuyCheck) {
      const p = state.pendingBuyCheck;
      const cur = currentLoadoutNumbers();
      const changed =
        cur.credits !== p.credits ||
        cur.primary !== p.primary ||
        cur.secondary !== p.secondary ||
        cur.tactical !== p.tactical ||
        cur.lastResult !== 0;
      const feedback = document.getElementById("buy-feedback");
      if (changed) {
        if (feedback) {
          feedback.textContent = BUY_RESULT_LABELS[cur.lastResult] || BUY_RESULT_LABELS[0];
          feedback.dataset.tone = cur.lastResult === 0 ? "ok" : "err";
        }
        state.pendingBuyCheck = null;
      } else if (performance.now() - p.sentAt > 1500) {
        if (feedback) {
          feedback.textContent = "Sem resposta do host para compra (verifique conexão P2P).";
          feedback.dataset.tone = "err";
        }
        state.pendingBuyCheck = null;
      }
    }

    const maskVal = st.availableWeaponMask ?? 0xffffffff;
    const engCnt = st.engineWeaponCount ?? 0;
    const inLvl = Boolean(st.inLevel);
    if (
      state.lastCatalogMask !== maskVal ||
      state.lastCatalogEngineCount !== engCnt ||
      state.lastCatalogInLevel !== inLvl
    ) {
      state.lastCatalogMask = maskVal;
      state.lastCatalogEngineCount = engCnt;
      state.lastCatalogInLevel = inLvl;
      refreshCatalogFromWasm();
      if (engine.activeOverlay === "buy") {
        renderBuyMenu(true);
      }
    }

    if (engine.activeOverlay === "buy") {
      const buyOpen = (st.buyTicksLeft || 0) > 0 ? 1 : 0;
      const btnKey = `${st.credits}-${st.primaryWeapon}-${st.secondaryWeapon}-${st.tacticalFlags}-${st.lastLoadoutResult}-${buyOpen}-${st.fixedBudget ? 1 : 0}-${state.catalog.weapons.length}-${maskVal}-${engCnt}`;
      const hdrKey = `${btnKey}-${Math.floor((st.buyTicksLeft || 0) / 30)}`;
      if (btnKey !== state.lastLoadoutSnapshotKey) {
        renderBuyMenu(false);
      } else if (hdrKey !== state.lastBuyHeaderKey) {
        state.lastBuyHeaderKey = hdrKey;
        updateBuyMenuHeader(currentLoadoutNumbers());
      }
    }

    updateLoadingOverlay();
    updateViewportChrome();
    updateTelemetryUi();
    if (engine.scoreboardOpen) updateScoreboard();
  } catch {}
}

function startEngineTickLoop() {
  if (!engine.rafId) {
    const tickFrame = () => {
      if (engine.module) {
        if (
          (engine.wheelPulseUntil > 0 && performance.now() >= engine.wheelPulseUntil) ||
          state.latchedUntil.size > 0
        ) {
          if (performance.now() >= engine.wheelPulseUntil) engine.wheelPulseUntil = 0;
          pushKeyMaskToWasm();
        }
        pollBrowserGamepad();
        pumpOutgoingWasmNet();
        if (engine.pendingDx !== 0 || engine.pendingDy !== 0) {
          engine.module._BlackWebPushMouseState(engine.mouseButtons, engine.pendingDx, engine.pendingDy);
          engine.pendingDx = 0;
          engine.pendingDy = 0;
        }
      }
      engine.rafId = requestAnimationFrame(tickFrame);
    };
    engine.rafId = requestAnimationFrame(tickFrame);
  }
  if (!engine.audioTimerId) engine.audioTimerId = setInterval(pumpWebAudio, 10);
  if (!engine.statusTimerId) engine.statusTimerId = setInterval(pollWasmStatus, 150);
  if (!engine.saveSyncTimerId) engine.saveSyncTimerId = setInterval(() => flushCampaignSaveIdbfs(false), 2500);
}

function updateTelemetryUi() {
  const st = engine.status;
  const topPill = document.getElementById("top-role-pill");
  const peers = st?.peers ?? (state.transport?.peers?.size || 0);

  if (topPill) {
    if (state.role === "offline") {
      topPill.textContent = "CAMPANHA SOLO";
    } else if (state.role === "host") {
      topPill.textContent = st?.inLevel
        ? `HOST · ${state.roomCode} · ${levelLabel(st.arenaId)} · ${peers + 1} jog.`
        : `HOST · ${state.roomCode} · ${peers + 1} jog.`;
    } else {
      topPill.textContent = st?.clientJoined
        ? `ONLINE · ${state.roomCode} · ${levelLabel(st.arenaId)}`
        : `ENTRANDO · ${state.roomCode}`;
    }
    topPill.dataset.mode = state.role;
  }

  // Lobby & botão Iniciar Multiplayer no Launcher
  const lobbyBox = document.getElementById("mp-lobby-status");
  const lobbyText = document.getElementById("mp-lobby-text");
  const lobbyPeers = document.getElementById("mp-lobby-peers");
  const playMpBtn = document.getElementById("btn-play-mp");
  const joinedOrHosting = state.role === "host" || (state.role === "client" && (peers > 0 || st?.clientJoined));
  if (lobbyBox) lobbyBox.dataset.ready = String(joinedOrHosting);
  if (lobbyPeers) lobbyPeers.textContent = state.role === "offline" ? "0 par(es)" : `${peers} par(es) · $${state.roomRules.budgetCap}`;
  if (lobbyText) {
    if (state.role === "offline") {
      lobbyText.textContent = "Crie uma sala como Host ou entre com o código do amigo para liberar a partida.";
    } else if (state.role === "host") {
      lobbyText.textContent =
        peers > 0
          ? `Sala ${state.roomCode} pronta com ${peers} amigo(s) conectado(s)! Todos podem clicar em Iniciar e escolher a Missão 1.`
          : `Sala ${state.roomCode} aberta (Host). Envie o link de convite (?room=${state.roomCode}) ou inicie enquanto aguarda.`;
    } else {
      const arenaTxt = st?.clientJoined ? levelLabel(st.arenaId) : "Missão 1 · Streets";
      lobbyText.textContent =
        peers > 0 || st?.clientJoined
          ? `Conectado à sala ${state.roomCode}! Fase ativa do Host: ${arenaTxt}. Clique em Iniciar e selecione a mesma missão.`
          : `Procurando host da sala ${state.roomCode}...`;
    }
  }
  if (playMpBtn) {
    const canLaunchMp = state.role === "host" || (state.role === "client" && (peers > 0 || Boolean(st?.clientJoined)));
    playMpBtn.disabled = !canLaunchMp;
    playMpBtn.textContent = engine.bootStarted ? "▶ Voltar para o Jogo" : "▶ Iniciar na Sala";
  }

  const optMpStatus = document.getElementById("opt-mp-status");
  if (optMpStatus) {
    optMpStatus.textContent =
      state.role === "offline"
        ? "Modo atual: Campanha Solo (sem multiplayer ou loja)."
        : state.role === "host"
          ? `Modo atual: Host da sala ${state.roomCode} · ${peers} par(es) · Teto $${state.roomRules.budgetCap}.`
          : st?.clientJoined || peers > 0
            ? `Modo atual: Conectado à sala ${state.roomCode} (${peers} par).`
            : `Modo atual: Procurando host da sala ${state.roomCode}...`;
  }

  // Barra HUD in-level (apenas em Multiplayer)
  if (isMultiplayerActive()) {
    const cur = currentLoadoutNumbers();
    const hudHp = document.getElementById("hud-hp");
    const hudCr = document.getElementById("hud-credits");
    const hudBw = document.getElementById("hud-buy-window");
    const hudPri = document.getElementById("hud-slot-primary");
    const hudSec = document.getElementById("hud-slot-secondary");
    const hudTac = document.getElementById("hud-tactical");
    if (hudHp) hudHp.textContent = String(st?.hp ?? 100);
    if (hudCr) hudCr.textContent = `$${cur.credits} / $${cur.maxCredits}`;
    if (hudBw) {
      hudBw.textContent =
        cur.buyTicksLeft === 0xffff ? "LOJA LIVRE" : cur.buyTicksLeft > 0 ? `COMPRA ${Math.ceil(cur.buyTicksLeft / 30)}s` : "COMPRA FECHADA";
    }
    if (hudPri) {
      hudPri.textContent = `[1] ${weaponNameById(cur.primary)}`;
      hudPri.classList.toggle("active", (st?.activeSlot ?? 0) === 0 && cur.primary !== NO_WEAPON);
    }
    if (hudSec) {
      hudSec.textContent = `[2] ${weaponNameById(cur.secondary)}`;
      hudSec.classList.toggle("active", (st?.activeSlot ?? 0) === 1 || cur.primary === NO_WEAPON);
    }
    if (hudTac) {
      const tList = [];
      if (cur.tactical & TACTICAL_ARMOR) tList.push("COLETE");
      if (cur.tactical & TACTICAL_PACK) tList.push("SILENCIADOR");
      hudTac.textContent = tList.join(" · ");
    }
  }

  // Gaveta Técnica
  const el = (id) => document.getElementById(id);
  const roleLabel = st && engine.bootStarted ? ["SOLO", "HOST", "CLIENTE"][st.role] || "SOLO" : state.role.toUpperCase();
  if (el("tel-role")) el("tel-role").textContent = roleLabel;
  if (el("tel-slot")) el("tel-slot").textContent = `#${st?.localSlot ?? state.localPlayerId} · Tick ${st?.tick ?? 0}`;
  if (el("tel-packets")) el("tel-packets").textContent = `TX ${state.packetsTx} / RX ${state.packetsRx}`;
  if (el("tel-engine")) {
    el("tel-engine").textContent = !engine.bootStarted
      ? state.wasmAvailable
        ? "PRONTO"
        : "AGUARDANDO WASM"
      : st
        ? `Estado ${st.bootState} · F${st.frames || 0}${st.inLevel ? " (IN-LEVEL)" : " (MENU)"}`
        : "INICIANDO";
  }
  if (el("tel-render")) {
    const gp = window.__blackGpuPerf;
    const fpsTxt = gp ? `${gp.fps} FPS (${gp.frameMs}ms · ${gp.drawsPerFrame}d/f · wb=${gp.writeBufCallsPerFrame})` : "";
    el("tel-render").textContent = st
      ? `${fpsTxt ? fpsTxt + " · " : ""}${st.presents || 0} pres · ${st.draws || 0} drw · ${st.shaders || 0} shd · ${st.audioFrames || 0} aud`
      : "Aguardando início";
  }
  if (el("dev-summary-metrics")) {
    const gp = window.__blackGpuPerf;
    el("dev-summary-metrics").textContent = `${gp ? gp.fps + " FPS · " : ""}${st?.presents || 0} quadros · TX ${state.packetsTx} / RX ${state.packetsRx}`;
  }
}

function updateScoreboard() {
  const tbody = document.getElementById("overlay-score-body");
  if (!tbody || !isMultiplayerActive()) return;
  const st = engine.status;
  const localId = st?.localSlot ?? state.localPlayerId;
  const rows = Array.isArray(st?.players)
    ? [...st.players].sort((a, b) => b.kills - a.kills || a.id - b.id)
    : [];
  tbody.innerHTML = "";
  for (const op of rows) {
    const loadoutTxt =
      op.primary !== undefined && op.primary !== NO_WEAPON
        ? `${weaponNameById(op.primary)} + ${weaponNameById(op.secondary)}`
        : weaponNameById(op.secondary ?? op.weapon ?? 0);
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td class="mono">#${op.id}</td>
      <td>${op.name}${op.id === localId ? " (VOCÊ)" : ""}</td>
      <td class="mono">${op.alive ? `${op.hp} HP` : "MORTO"}</td>
      <td class="mono">${loadoutTxt}</td>
      <td class="mono">${op.kills}</td>
      <td class="mono">${op.deaths}</td>
    `;
    tbody.appendChild(tr);
  }
}

// ---- Boot & Pipeline de Assets ----

function ensureVfsBridge() {
  if (!engine.vfs) {
    engine.vfs = createBrowserVfsBridge({ onLog: (msg) => logLine("VFS", msg) });
    engine.vfs.installGlobal();
  }
  return engine.vfs;
}

async function initAssetPipeline() {
  const statusEl = document.getElementById("opfs-status");
  const barEl = document.getElementById("asset-progress-bar");
  const playSoloBtn = document.getElementById("btn-play-solo");
  const vfs = ensureVfsBridge();

  const [cachedCheck, opfsScan] = await Promise.all([
    checkOpfsCached().catch(() => ({ cached: false, sizeBytes: 0 })),
    listOpfsAssetFiles().catch(() => ({ available: false, mode: "none", files: [] })),
  ]);

  let manifest = null;
  try {
    const res = await fetch("/api/manifest");
    if (res.ok) {
      manifest = await res.json();
      state.serverManifest = manifest;
      state.wasmAvailable = Boolean(manifest.wasm_ready);
    }
  } catch {}

  if (!vfs.source || vfs.source !== "local") {
    vfs.setFromOpfsAndManifest({ opfsScan, manifest, baseUrl: "" });
  }

  if (vfs.hasCatalog() && state.wasmAvailable) {
    const totalMb = (
      (vfs.source === "opfs" ? opfsScan.totalBytes : manifest?.total_bytes || opfsScan.totalBytes || 0) /
      (1024 * 1024)
    ).toFixed(1);
    if (barEl) barEl.style.width = "100%";
    if (statusEl) {
      statusEl.textContent =
        vfs.source === "opfs"
          ? `Arquivos em cache offline (${vfs.entries.length} itens, ${totalMb} MB).`
          : `Jogo pronto para stream direto (${vfs.entries.length} arquivos, ${totalMb} MB).`;
    }
    if (playSoloBtn) {
      playSoloBtn.disabled = false;
      playSoloBtn.textContent = "▶ Jogar Campanha";
    }
    updateTelemetryUi();
    return;
  }

  if (statusEl) {
    statusEl.textContent = "Selecione a pasta com os arquivos de BLACK ou um .ISO/.XISO abaixo para jogar.";
  }
  if (playSoloBtn) playSoloBtn.textContent = "▶ Selecionar Pasta de BLACK";
  updateTelemetryUi();
}

async function initAudioWorklet() {
  if (engine.audioNode && engine.audioCtx) {
    if (engine.audioCtx.state === "suspended") await engine.audioCtx.resume();
    return true;
  }
  const AudioCtx = window.AudioContext || window.webkitAudioContext;
  if (!AudioCtx || !AudioWorkletNode) return false;
  const ctx = new AudioCtx({ sampleRate: 48000, latencyHint: "interactive" });
  await ctx.audioWorklet.addModule("./audio_worklet.js");
  const node = new AudioWorkletNode(ctx, "black-audio-processor", {
    numberOfInputs: 0,
    numberOfOutputs: 1,
    outputChannelCount: [2],
  });
  node.connect(ctx.destination);
  if (ctx.state === "suspended") await ctx.resume();
  engine.audioCtx = ctx;
  engine.audioNode = node;
  return true;
}

async function startWasmEngine(targetMode) {
  if (targetMode === "solo") {
    startSession("offline");
  }
  const statusEl = document.getElementById("opfs-status");
  const gameCanvas = document.getElementById("game-canvas");
  const vfs = ensureVfsBridge();

  if (engine.bootStarted && engine.module) {
    setOverlay("none");
    gameCanvas?.focus();
    if (engine.audioCtx?.state === "suspended") await engine.audioCtx.resume().catch(() => {});
    return;
  }

  if (!vfs.hasCatalog()) {
    await initAssetPipeline();
    if (!vfs.hasCatalog()) {
      document.getElementById("xbe-file-input")?.click();
      return;
    }
  }

  engine.bootStarted = true;
  setOverlay("none");
  updateLoadingOverlay();
  if (statusEl) statusEl.textContent = "Carregando WebGPU e iniciando BLACK...";

  try {
    if (!engine.renderer) {
      engine.renderer = new BlackWebGpuRenderer(gameCanvas, (tag, msg) => logLine(tag, msg));
      engine.renderer.installGlobal();
    }
    if (!engine.module) {
      let dsoundSpamCount = 0;
      let wasmOtherErrCount = 0;
      const { default: createBlackEnhancedModule } = await import("/wasm/black_enhanced.js");
      const mod = await createBlackEnhancedModule({
        locateFile: (p) => `/wasm/${p}`,
        print: (txt) => {
          const line = String(txt || "").trim();
          if (line) logLine("WASM", line);
        },
        printErr: (txt) => {
          const line = String(txt || "").trim();
          if (!line) return;
          const isMpOrHooks =
            line.includes("mp_bridge:") ||
            line.includes("hooks:") ||
            line.includes("boot_wasm:") ||
            line.includes("web_main:");
          const isErr = line.includes("[error]") || line.includes("error:") || line.includes("TRAP");
          if (line.includes("dsound:") && !isErr) {
            if (dsoundSpamCount >= 12) return;
            dsoundSpamCount++;
          }
          if (isMpOrHooks) {
            logLine("WASM", line);
            return;
          }
          if (isErr) {
            if (wasmOtherErrCount < 80) {
              wasmOtherErrCount++;
              logLine("WASM-ERR", line);
            }
            return;
          }
          if (wasmOtherErrCount < 50) {
            wasmOtherErrCount++;
            logLine("WASM", line);
          }
        },
      });
      engine.module = mod;
      engine.renderer.setModule(mod);
      engine.audioPtr = mod._malloc(2048 * 2 * 4);
      engine.statusPtr = mod._malloc(4096);
      engine.weaponsPtr = mod._malloc(4096);
      engine.netPeerPtr = mod._malloc(4);
      engine.netChPtr = mod._malloc(4);
      engine.netKindPtr = mod._malloc(4);
      engine.netBufPtr = mod._malloc(2048);
      engine.netInBufPtr = mod._malloc(2048);
    }

    const gpuInfo = await engine.renderer.ensureDevice();
    setBadge("cap-webgpu", gpuInfo.ready ? "WEBGPU ATIVO" : "SEM WEBGPU", gpuInfo.ready ? "ok" : "warn");

    try {
      await initAudioWorklet();
    } catch {}

    const mod = engine.module;
    vfs.writeManifestToMemfs(mod, "/black_assets");
    await mountCampaignSaveIdbfs(mod);
    mod._BlackWebNotifyCanvasReady(gameCanvas.width || 1280, gameCanvas.height || 720, gpuInfo.ready ? 1 : 0);

    pushEnhancedConfigToWasm();
    refreshCatalogFromWasm();
    syncWasmRole(state.role);

    const rc = mod.ccall("BlackWebRequestBoot", "number", ["string"], ["/black_assets"]);
    logLine("BOOT", `BlackWebRequestBoot('/black_assets') -> rc=${rc}`);

    startEngineTickLoop();
    pollWasmStatus();
    gameCanvas?.focus();
  } catch (err) {
    engine.bootStarted = false;
    setOverlay("launcher");
    logLine("ERRO", `Falha ao iniciar motor WASM: ${err.message}`);
    if (statusEl) statusEl.textContent = `Erro no boot WASM: ${err.message}`;
  }
}

async function syncOfflineOpfs() {
  const statusEl = document.getElementById("opfs-status");
  const barEl = document.getElementById("asset-progress-bar");
  const btn = document.getElementById("btn-sync-opfs");
  if (btn) btn.disabled = true;
  try {
    const result = await streamAssetsFromRemote("", ({ count, totalFiles, downloadedBytes, totalBytes, percent, currentFile }) => {
      if (barEl) barEl.style.width = `${Math.min(100, percent).toFixed(1)}%`;
      if (statusEl) {
        statusEl.textContent = `OPFS [${count}/${totalFiles}] ${currentFile} (${(downloadedBytes / 1048576).toFixed(1)}/${(totalBytes / 1048576).toFixed(1)} MB)`;
      }
    });
    logLine("OPFS", `Sincronização concluída (${result.count} arquivos).`);
    await initAssetPipeline();
  } catch (err) {
    if (statusEl) statusEl.textContent = `Falha no cache offline: ${err.message}`;
  } finally {
    if (btn) btn.disabled = false;
  }
}

async function handlePickedLocalFiles(fileList) {
  const statusEl = document.getElementById("opfs-status");
  const barEl = document.getElementById("asset-progress-bar");
  try {
    const vfs = ensureVfsBridge();
    const prep = await normalizePickedFiles(fileList, (p) => {
      if (barEl && p.totalBytes) barEl.style.width = `${((p.writtenBytes / p.totalBytes) * 100).toFixed(1)}%`;
      if (statusEl) statusEl.textContent = `Lendo arquivo local: ${p.currentFile}`;
    });
    vfs.setLocalEntries(prep.entries);
    if (statusEl) statusEl.textContent = `Pasta local pronta (${prep.entries.length} arquivos, zero download).`;
    await initAssetPipeline();
  } catch (err) {
    if (statusEl) statusEl.textContent = `Arquivo inválido: ${err.message}`;
  }
}

function applyRoomFromUrl() {
  try {
    const param = new URLSearchParams(window.location.search).get("room");
    if (!param) return;
    const clean = param.trim().toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 6);
    if (!clean) return;
    state.roomCode = clean;
    syncRoomFieldsToUi();
    switchLauncherTab("mp");
    showToast(`Convite detectado para sala ${clean}. Clique em "Entrar na Sala"!`, 3800);
  } catch {}
}

function bindEvents() {
  const gameCanvas = document.getElementById("game-canvas");
  const wrap = document.getElementById("viewport-wrap");

  // Launcher Tabs & Actions
  document.getElementById("tab-solo")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    switchLauncherTab("solo");
  });
  document.getElementById("tab-multiplayer")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    switchLauncherTab("mp");
  });

  document.getElementById("btn-play-solo")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startWasmEngine("solo");
  });
  document.getElementById("btn-play-mp")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (!isMultiplayerActive()) {
      showToast('Primeiro clique em "Criar Sala (Host)" ou "Entrar na Sala".');
      return;
    }
    startWasmEngine("mp");
  });

  document.getElementById("launch-enhanced-toggle")?.addEventListener("input", (e) => {
    const enabled = Boolean(e.target?.checked);
    state.enhanced.mouseDirect = enabled;
    state.enhanced.hitmarker = enabled;
    state.enhanced.fovDeg = enabled ? Math.max(state.enhanced.fovDeg, 88) : 70;
    syncEnhancedControlsUi();
    pushEnhancedConfigToWasm();
  });

  document.getElementById("btn-launcher-options")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setOverlay("options");
  });
  document.getElementById("btn-open-options")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setOverlay(engine.activeOverlay === "options" ? (engine.bootStarted ? "none" : "launcher") : "options");
  });
  document.getElementById("btn-close-options")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setOverlay(engine.bootStarted ? "none" : "launcher");
    releaseInputFocus(true);
  });

  // Assist bar botões (em menus do jogo !inLevel)
  document.getElementById("btn-assist-start")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    pulseKey("Enter", 150);
  });
  document.getElementById("btn-assist-a")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    pulseKey("Space", 150);
  });
  document.getElementById("btn-assist-mp-hint")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    showToast("Em Multiplayer: escolha a Missão 1 no menu com seu amigo e aperte B dentro da fase para comprar armas!", 4500);
    releaseInputFocus(true);
  });

  // Loadout & Placar (Multiplayer)
  const toggleBuyOverlay = () => {
    if (!isMultiplayerActive()) {
      showToast("Loja de Loadout está disponível apenas no modo Multiplayer Online.");
      return;
    }
    if (engine.activeOverlay === "buy") {
      setOverlay("none");
      releaseInputFocus(true);
    } else {
      setOverlay("buy");
    }
  };
  document.getElementById("btn-open-buy")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    toggleBuyOverlay();
  });
  document.getElementById("hud-btn-buy")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    toggleBuyOverlay();
  });
  document.getElementById("btn-close-buy")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    toggleBuyOverlay();
  });
  document.getElementById("btn-open-buy-from-opt")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setOverlay("buy");
  });
  document.getElementById("hud-btn-score")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setScoreboardVisible(!engine.scoreboardOpen);
    releaseInputFocus(true);
  });

  document.getElementById("btn-rebuy-kit")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (state.lastRebuyKit) {
      submitLoadoutPurchase(state.lastRebuyKit.primary, state.lastRebuyKit.secondary, state.lastRebuyKit.tactical);
    }
  });
  document.getElementById("hud-slot-primary")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    engine.module?._BlackWebEquipSlot?.(0);
    releaseInputFocus(true);
  });
  document.getElementById("hud-slot-secondary")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    engine.module?._BlackWebEquipSlot?.(1);
    releaseInputFocus(true);
  });

  // Multiplayer Sala & Convite (Launcher + Modal Opções)
  document.getElementById("btn-mp-host")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startSession("host", false);
  });
  document.getElementById("btn-mp-join")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startSession("client", false);
  });
  document.getElementById("btn-copy-invite")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    copyInviteLink(false);
  });
  document.getElementById("btn-rand-code")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    let code = "";
    for (let i = 0; i < 6; i++) code += alphabet[Math.floor(Math.random() * alphabet.length)];
    state.roomCode = code;
    syncRoomFieldsToUi();
  });

  document.getElementById("btn-opt-switch-solo")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startSession("offline", true);
  });
  document.getElementById("btn-opt-host")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startSession("host", true);
  });
  document.getElementById("btn-opt-join")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startSession("client", true);
  });
  document.getElementById("btn-opt-copy-link")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    copyInviteLink(true);
  });

  document.getElementById("btn-fullscreen")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    (wrap || gameCanvas)?.requestFullscreen?.();
    releaseInputFocus(true);
  });

  // Enhanced Controls
  const bindEnhanced = (id, fn) => {
    document.getElementById(id)?.addEventListener("input", () => {
      fn();
      syncEnhancedControlsUi();
      pushEnhancedConfigToWasm();
    });
  };
  bindEnhanced("opt-mouse-direct", () => {
    state.enhanced.mouseDirect = Boolean(document.getElementById("opt-mouse-direct")?.checked);
  });
  bindEnhanced("opt-mouse-sens", () => {
    state.enhanced.mouseSens = Number(document.getElementById("opt-mouse-sens")?.value || 0.085);
  });
  bindEnhanced("opt-ads-sens", () => {
    state.enhanced.adsSensMult = Number(document.getElementById("opt-ads-sens")?.value || 0.7);
  });
  bindEnhanced("opt-fov", () => {
    state.enhanced.fovDeg = Number(document.getElementById("opt-fov")?.value || 88);
  });
  bindEnhanced("opt-invert-y", () => {
    state.enhanced.invertY = Boolean(document.getElementById("opt-invert-y")?.checked);
  });
  bindEnhanced("opt-skip-intro", () => {
    state.enhanced.skipIntro = Boolean(document.getElementById("opt-skip-intro")?.checked);
  });
  bindEnhanced("opt-hitmarker", () => {
    state.enhanced.hitmarker = Boolean(document.getElementById("opt-hitmarker")?.checked);
  });
  bindEnhanced("opt-render-percent", () => {
    state.enhanced.renderPercent = Number(document.getElementById("opt-render-percent")?.value || 100);
  });
  bindEnhanced("opt-overhaul-enabled", () => {
    state.enhanced.overhaulEnabled = Boolean(document.getElementById("opt-overhaul-enabled")?.checked);
  });
  bindEnhanced("opt-player-dmg", () => {
    state.enhanced.playerDamageMult = Number(document.getElementById("opt-player-dmg")?.value || 1.35);
  });
  bindEnhanced("opt-enemy-dmg", () => {
    state.enhanced.enemyDamageMult = Number(document.getElementById("opt-enemy-dmg")?.value || 1.0);
  });
  bindEnhanced("opt-recoil-mult", () => {
    state.enhanced.recoilMult = Number(document.getElementById("opt-recoil-mult")?.value || 0.6);
  });
  bindEnhanced("opt-ammo-bonus", () => {
    state.enhanced.ammoBonusEnabled = Boolean(document.getElementById("opt-ammo-bonus")?.checked);
  });

  document.getElementById("btn-vanilla-preset")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    state.enhanced = {
      ...state.enhanced,
      mouseDirect: false,
      mouseSens: 0.085,
      adsSensMult: 0.7,
      invertY: false,
      fovDeg: 70,
      hitmarker: false,
      renderPercent: 100,
      overhaulEnabled: false,
    };
    syncEnhancedControlsUi();
    pushEnhancedConfigToWasm();
  });

  // Assets & Drag/Drop
  const folderInput = document.getElementById("xbe-file-input");
  const isoInput = document.getElementById("iso-file-input");
  document.getElementById("btn-sync-opfs")?.addEventListener("click", syncOfflineOpfs);
  document.getElementById("btn-pick-files")?.addEventListener("click", () => folderInput?.click());
  document.getElementById("btn-pick-iso")?.addEventListener("click", () => isoInput?.click());
  folderInput?.addEventListener("change", (e) => handlePickedLocalFiles(e.target.files));
  isoInput?.addEventListener("change", (e) => handlePickedLocalFiles(e.target.files));

  if (wrap) {
    wrap.addEventListener("dragover", (e) => {
      e.preventDefault();
      wrap.classList.add("drag-over");
    });
    wrap.addEventListener("dragleave", () => wrap.classList.remove("drag-over"));
    wrap.addEventListener("drop", (e) => {
      e.preventDefault();
      wrap.classList.remove("drag-over");
      if (e.dataTransfer?.files?.length) handlePickedLocalFiles(e.dataTransfer.files);
    });
  }

  // Pointer Lock tracking: Esc libera cursor suavemente SEM abrir modal bloqueante sobre o jogo!
  document.addEventListener("pointerlockchange", () => {
    updateViewportChrome();
  });

  // Teclado unificado (F10 Opções, B Loja APENAS em MP, Tab Placar APENAS em MP, Gameplay/Menus)
  window.addEventListener("keydown", (e) => {
    if (["INPUT", "TEXTAREA", "SELECT"].includes(document.activeElement?.tagName)) return;

    if (e.code === "F10") {
      e.preventDefault();
      setOverlay(engine.activeOverlay === "options" ? (engine.bootStarted ? "none" : "launcher") : "options");
      return;
    }

    if (e.code === "Escape") {
      if (engine.activeOverlay === "buy" || engine.activeOverlay === "options") {
        e.preventDefault();
        setOverlay(engine.bootStarted ? "none" : "launcher");
        releaseInputFocus(true);
      } else if (engine.scoreboardOpen) {
        e.preventDefault();
        setScoreboardVisible(false);
      }
      return;
    }

    if (e.code === "KeyB") {
      if (isMultiplayerActive() && engine.bootStarted) {
        e.preventDefault();
        toggleBuyOverlay();
      }
      return;
    }

    if (e.code === "Tab") {
      if (isMultiplayerActive() && engine.bootStarted) {
        e.preventDefault();
        setScoreboardVisible(true);
      }
      return;
    }

    if (engine.activeOverlay === "buy") {
      if (e.code === "F1" && state.lastRebuyKit) {
        e.preventDefault();
        submitLoadoutPurchase(state.lastRebuyKit.primary, state.lastRebuyKit.secondary, state.lastRebuyKit.tactical);
        return;
      }
      const m = /^Digit([1-9])$/.exec(e.code);
      if (m) {
        e.preventDefault();
        const btn = document.querySelector(`#overlay-buy button[data-buy-hotkey="${m[1]}"]`);
        if (btn && !btn.disabled) btn.click();
      }
      return;
    }

    if (engine.activeOverlay !== "none") return;

    if (
      engine.bootStarted &&
      ["Enter", "Space", "Backspace", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Tab"].includes(e.code)
    ) {
      e.preventDefault();
    }

    if (isMultiplayerActive() && isInLevelNow()) {
      if (e.code === "Digit1") engine.module?._BlackWebEquipSlot?.(0);
      else if (e.code === "Digit2") engine.module?._BlackWebEquipSlot?.(1);
    }

    if (DISCRETE_LATCH_CODES.has(e.code) && !e.repeat) {
      state.latchedUntil.set(e.code, performance.now() + 120);
    }
    state.keys.add(e.code);
    pushKeyMaskToWasm();
    if (engine.audioCtx?.state === "suspended") engine.audioCtx.resume().catch(() => {});
  });

  window.addEventListener("keyup", (e) => {
    if (e.code === "Tab" && isMultiplayerActive()) {
      setScoreboardVisible(false);
    }
    state.keys.delete(e.code);
    pushKeyMaskToWasm();
  });

  window.addEventListener("blur", () => {
    state.keys.clear();
    state.latchedUntil.clear();
    engine.mouseButtons = 0;
    setScoreboardVisible(false);
    pushKeyMaskToWasm();
    engine.module?._BlackWebPushMouseState(0, 0, 0);
  });
  const cleanupOnUnload = () => {
    flushCampaignSaveIdbfs(true);
    if (state.transport) {
      state.transport.close();
      state.transport = null;
    }
  };
  window.addEventListener("pagehide", cleanupOnUnload);
  window.addEventListener("beforeunload", cleanupOnUnload);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") flushCampaignSaveIdbfs(true);
  });

  gameCanvas?.addEventListener("contextmenu", (e) => e.preventDefault());
  gameCanvas?.addEventListener("auxclick", (e) => {
    if (e.button === 1) e.preventDefault();
  });
  gameCanvas?.addEventListener("mousedown", (e) => {
    if (engine.activeOverlay !== "none") return;
    gameCanvas.focus();
    if (engine.audioCtx?.state === "suspended") engine.audioCtx.resume().catch(() => {});

    if (e.button === 1) {
      // Botão do meio do mouse: coronhada / melee instantâneo clássico
      e.preventDefault();
      pulseKey("KeyV");
    }

    // Pointer Lock APENAS quando estiver em gameplay 3D real (inLevel === true)
    if (isInLevelNow()) {
      if (document.pointerLockElement !== gameCanvas) {
        try {
          Promise.resolve(gameCanvas.requestPointerLock?.()).catch(() => {});
        } catch {}
      }
      engine.lastMouseX = e.clientX || 0;
      engine.lastMouseY = e.clientY || 0;
      engine.hasLastMousePos = true;
      if (e.button === 0) engine.mouseButtons |= 1;
      if (e.button === 2) engine.mouseButtons |= 2;
      engine.module?._BlackWebPushMouseState(engine.mouseButtons, 0, 0);
    } else {
      // Fora de fase (menu ou vídeo intro): clique avança ou pula
      pulseKey("Enter", 120);
    }
  });
  gameCanvas?.addEventListener(
    "wheel",
    (e) => {
      if (engine.activeOverlay !== "none" || !isInLevelNow()) return;
      if (e.deltaY !== 0) {
        engine.wheelPulseUntil = performance.now() + 90;
        pushKeyMaskToWasm();
      }
    },
    { passive: true }
  );
  window.addEventListener("mouseup", (e) => {
    if (e.button === 0) engine.mouseButtons &= ~1;
    if (e.button === 2) engine.mouseButtons &= ~2;
    if ((engine.mouseButtons & 3) === 0) engine.hasLastMousePos = false;
    engine.module?._BlackWebPushMouseState(engine.mouseButtons, 0, 0);
  });
  gameCanvas?.addEventListener("mousemove", (e) => {
    if (!engine.module || engine.activeOverlay !== "none" || !isInLevelNow()) return;
    const locked = document.pointerLockElement === gameCanvas;
    if (locked) {
      engine.pendingDx += e.movementX || 0;
      engine.pendingDy += e.movementY || 0;
    } else if ((engine.mouseButtons & 3) !== 0) {
      let dx = e.movementX || 0;
      let dy = e.movementY || 0;
      if (dx === 0 && dy === 0 && engine.hasLastMousePos) {
        dx = (e.clientX || 0) - engine.lastMouseX;
        dy = (e.clientY || 0) - engine.lastMouseY;
      }
      engine.lastMouseX = e.clientX || 0;
      engine.lastMouseY = e.clientY || 0;
      engine.hasLastMousePos = true;
      engine.pendingDx += dx;
      engine.pendingDy += dy;
    }
  });
}

window.addEventListener("DOMContentLoaded", async () => {
  ensureVfsBridge();
  syncEnhancedControlsUi();
  bindEvents();
  await detectBrowserCapabilities();
  await initAssetPipeline();
  startSession("offline");
  applyRoomFromUrl();
  setOverlay("launcher");
});
