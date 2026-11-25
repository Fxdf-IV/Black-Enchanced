import {
  checkOpfsCached,
  streamAssetsFromRemote,
  listOpfsAssetFiles,
  normalizePickedFiles,
  createBrowserVfsBridge,
} from "./opfs_loader.js";
import { BlackWebGpuRenderer } from "./webgpu_renderer.js";
import { WebRtcTransport } from "./netplay_webrtc.js";
import { createTouchControls } from "./touch_controls.js";

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
  maxCredits: 15,
  armorPrice: 3,
  armorBonus: 25,
  tacticalPrice: 2,
  buyWindowTicks: 0,
  weapons: [
    { id: 0, name: "Glock 19", slot: 0, price: 0, damage: 22, mag: 19, range: 80, fireTicks: 5 },
    { id: 1, name: "DC3 Elite", slot: 0, price: 1, damage: 28, mag: 12, range: 85, fireTicks: 5 },
    { id: 2, name: "Magnum", slot: 0, price: 2, damage: 55, mag: 6, range: 100, fireTicks: 12 },
    { id: 3, name: "MP5", slot: 1, price: 4, damage: 20, mag: 30, range: 90, fireTicks: 2 },
    { id: 4, name: "MAC-10", slot: 1, price: 3, damage: 18, mag: 32, range: 70, fireTicks: 2 },
    { id: 5, name: "Uzi", slot: 1, price: 3, damage: 19, mag: 32, range: 75, fireTicks: 2 },
    { id: 6, name: "P90", slot: 1, price: 5, damage: 21, mag: 50, range: 95, fireTicks: 2 },
    { id: 7, name: "AK-47", slot: 1, price: 7, damage: 34, mag: 30, range: 160, fireTicks: 3 },
    { id: 8, name: "M16A2", slot: 1, price: 7, damage: 30, mag: 30, range: 175, fireTicks: 3 },
    { id: 9, name: "G36C", slot: 1, price: 6, damage: 29, mag: 30, range: 165, fireTicks: 3 },
    { id: 10, name: "SPAS-12", slot: 1, price: 5, damage: 75, mag: 8, range: 35, fireTicks: 15 },
    { id: 11, name: "Remington 870", slot: 1, price: 4, damage: 85, mag: 8, range: 32, fireTicks: 18 },
    { id: 12, name: "M249 SAW", slot: 1, price: 9, damage: 32, mag: 100, range: 170, fireTicks: 2 },
    { id: 13, name: "WA2000", slot: 1, price: 9, damage: 95, mag: 6, range: 300, fireTicks: 20 },
    { id: 14, name: "M203", slot: 1, price: 8, damage: 100, mag: 1, range: 120, fireTicks: 45 },
    { id: 15, name: "RPG-7", slot: 1, price: 10, damage: 100, mag: 1, range: 200, fireTicks: 60 },
  ],
};

const BLACK_MAPS = [
  { index: 0, internalId: 0, name: "Veblensk City Streets" },
  { index: 1, internalId: 1, name: "Treneska Border Crossing" },
  { index: 2, internalId: 5, name: "Naszran Foundry" },
  { index: 3, internalId: 3, name: "Tivliz Asylum" },
  { index: 4, internalId: 4, name: "Veblensk Dockyard" },
  { index: 5, internalId: 6, name: "Graznei Bridge" },
  { index: 6, internalId: 7, name: "Spetriniv Gulag" },
  { index: 7, internalId: 8, name: "Spetriniv (Final)" },
];

function mapNameByIndex(idx) {
  return BLACK_MAPS[Number(idx) & 7]?.name || "Veblensk City Streets";
}

const BUY_RESULT_LABELS = [
  "Loadout equipado na partida.",
  "Créditos insuficientes para este item.",
  "Kit ultrapassa o teto de orçamento da sala.",
  "Item inválido para este slot.",
  "Janela de compra encerrada (tente no próximo spawn).",
  "Indisponível enquanto estiver abatido ou no fim da partida.",
];

const TA_BUY_RESULT_LABELS = [
  "Kit tático (15 SP) atualizado.",
  "Supply Points (SP) insuficientes para este item.",
  "Kit ultrapassa o limite fixo de 15 Supply Points (SP).",
  "Item inválido — no Territorial Assault a arma primária é obrigatória.",
  "Loadout bloqueado durante o combate (edite na Preparação ou Intervalo).",
  "Indisponível enquanto estiver abatido ou no fim da partida.",
];

const TA_PHASE_LABELS = [
  "LOBBY",
  "PREPARAÇÃO",
  "RODADA 1",
  "INTERVALO",
  "RODADA 2",
  "RESULTADO",
];

function loadoutResultLabel(code, isTA) {
  const list = isTA ? TA_BUY_RESULT_LABELS : BUY_RESULT_LABELS;
  return list[code] || list[0];
}

function formatBudget(val, isTA) {
  return isTA ? `${val} SP` : `$${val}`;
}

function formatClock(totalSeconds) {
  const s = Math.max(0, Math.floor(Number(totalSeconds) || 0));
  const m = Math.floor(s / 60);
  const r = s % 60;
  return `${String(m).padStart(2, "0")}:${String(r).padStart(2, "0")}`;
}

function gameModeShortLabel(mode) {
  if (mode === 2) return "TERRITORIAL";
  if (mode === 1) return "CO-OP";
  return "PVP ARENA";
}

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
  roomRules: { gameMode: 2, csLoadout: true, fixedBudget: true, budgetCap: 15, mapIndex: 0 },
  lobby: {
    mapIndex: 0,
    localTeam: 0,
    primary: 7,     // AK-47 (7 SP)
    secondary: 0,   // Glock 19 (0 SP)
    tactical: 0,
    ready: false,
    matchLaunching: false,
    remotePeers: new Map(), // peerId -> { peerId, name, team, primary, secondary, tactical, ready }
    rosterSnapshot: [],
  },
  lastRebuyKit: null,
  lastLoadoutSnapshotKey: "",
  lastBuyHeaderKey: "",
  lastCatalogGameMode: -1,
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

function activeGameMode() {
  if (!isMultiplayerActive()) return 0;
  if (engine.status && typeof engine.status.gameMode === "number") {
    return engine.status.gameMode;
  }
  return Number(state.roomRules.gameMode || 0);
}

function isTerritorialMode() {
  return isMultiplayerActive() && activeGameMode() === 2;
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
  const mp = isMultiplayerActive();
  // No Multiplayer, cobre o canvas com a tela de carregamento do mapa até entrar direto na fase (sem menu do Xbox).
  const shouldShow =
    engine.bootStarted &&
    engine.activeOverlay === "none" &&
    (mp ? !st?.inLevel : (!st || st.bootState < 2 || (st.presents || 0) === 0));
  scrim.classList.toggle("hidden", !shouldShow);
  const title = document.getElementById("loading-title");
  const sub = document.getElementById("loading-sub");
  if (mp && shouldShow) {
    if (title) title.textContent = `CARREGANDO MAPA · ${mapNameByIndex(state.lobby.mapIndex).toUpperCase()}`;
    if (sub) {
      const side = state.lobby.localTeam === 0 ? "Time A (Ataque R1)" : "Time B (Defesa R1)";
      const kit = `${weaponNameById(state.lobby.primary)} + ${weaponNameById(state.lobby.secondary)}`;
      sub.textContent = `${side} · Kit: ${kit} · Entrando direto na partida...`;
    }
  } else {
    if (title) title.textContent = "Inicializando BLACK no Navegador...";
    if (sub && st?.bootMessage) sub.textContent = st.bootMessage;
  }
}

function setOverlay(name) {
  if (name === "buy" && (!isMultiplayerActive() || !isInLevelNow())) {
    if (!isMultiplayerActive()) {
      showToast("Armaria 15 SP está disponível apenas no modo Multiplayer Online.");
    } else {
      showToast("A Armaria [B] abre durante o Lobby Pré-Partida ou no Intervalo entre rodadas.");
    }
    return;
  }

  engine.activeOverlay = name;
  const launcher = document.getElementById("overlay-launcher");
  const lobby = document.getElementById("overlay-lobby");
  const options = document.getElementById("overlay-options");
  const buy = document.getElementById("overlay-buy");
  const results = document.getElementById("overlay-results");

  launcher?.classList.toggle("hidden", name !== "launcher");
  lobby?.classList.toggle("hidden", name !== "lobby");
  options?.classList.toggle("hidden", name !== "options");
  buy?.classList.toggle("hidden", name !== "buy");
  results?.classList.toggle("hidden", name !== "results");

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
  if (name === "lobby") {
    renderPreMatchLobby();
  }
  if (name === "buy") {
    refreshCatalogFromWasm();
    renderBuyMenu(true);
  }
  if (name === "options") {
    syncEnhancedControlsUi();
    syncRoomFieldsToUi();
  }
  if (name === "results") {
    renderResultsModal(engine.status);
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
  const isTA = isTerritorialMode();
  const inLvl = Boolean(st?.inLevel);
  const presenting = engine.bootStarted && Boolean(st && st.bootState >= 2 && (st.presents || 0) > 0);
  const gameCanvas = document.getElementById("game-canvas");
  const wrap = document.getElementById("viewport-wrap");
  const locked = document.pointerLockElement === gameCanvas;

  // Loadout e placar só existem quando Multiplayer Online está ativo
  const buyTopBtn = document.getElementById("btn-open-buy");
  if (buyTopBtn) {
    buyTopBtn.classList.toggle("hidden", !(mp && inLvl));
    buyTopBtn.textContent = isTA ? "Armaria 15 SP (B)" : "Loja Loadout (B)";
  }
  document.getElementById("btn-open-buy-from-opt")?.classList.toggle("hidden", !(mp && inLvl));
  document.getElementById("hud-strip")?.classList.toggle("hidden", !(mp && inLvl && engine.activeOverlay === "none"));
  const showTaHud = isTA && engine.bootStarted && inLvl && engine.activeOverlay === "none" && Boolean(st?.ta);
  document.getElementById("ta-hud")?.classList.toggle("hidden", !showTaHud);
  document.getElementById("ta-radar-wrap")?.classList.toggle("hidden", !showTaHud);
  document.getElementById("ta-killfeed")?.classList.toggle("hidden", !showTaHud);
  if (!showTaHud) {
    document.getElementById("ta-waypoint")?.classList.add("hidden");
    document.getElementById("ta-event-banner")?.classList.add("hidden");
  }
  wrap?.classList.toggle("ta-active", showTaHud);
  // Barra de assist de menu do Xbox aparece APENAS na Campanha Solo (no Multiplayer vai direto ao mapa).
  document.getElementById("menu-assist-bar")?.classList.toggle(
    "hidden",
    !(!mp && presenting && !inLvl && engine.activeOverlay === "none")
  );
  document.getElementById("btn-assist-mp-hint")?.classList.add("hidden");

  touchControls?.update({
    bootStarted: engine.bootStarted,
    presenting,
    inLevel: inLvl,
    overlay: engine.activeOverlay,
    multiplayer: mp,
  });

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
        item("B", "Armaria 15 SP (na missão)"),
      ].join("");
    } else if (mp) {
      footerKeys.innerHTML = [
        item("WASD", "Mover"),
        item("Mouse", "Mirar"),
        item("Esq/Dir", "Atirar / Zoom"),
        item("1 / 2 / Roda", "Arma"),
        item("B", "Armaria 15 SP"),
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
  const mapEl = document.getElementById("mp-map-select");
  const rawMap = mapEl ? Number(mapEl.value) : state.lobby.mapIndex;
  const mapIndex = Number.isFinite(rawMap) ? Math.max(0, Math.min(7, rawMap | 0)) : 0;
  state.lobby.mapIndex = mapIndex;
  state.roomRules = {
    gameMode: 2, // Multiplayer é exclusivamente Territorial Assault (15 SP, 2 rodadas, mapas de BLACK)
    csLoadout: true,
    fixedBudget: true,
    budgetCap: 15,
    mapIndex,
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
  setVal("mp-map-select", String(state.lobby.mapIndex ?? 0));
  setVal("lobby-map-select", String(state.lobby.mapIndex ?? 0));
}

function readIdentityFromInputs(preferOptionsModal = false) {
  const codeId = preferOptionsModal ? "opt-room-code" : "mp-room-code";
  const nameId = preferOptionsModal ? "opt-player-name" : "mp-player-name";
  const rawCode = document.getElementById(codeId)?.value || state.roomCode;
  const rawName = document.getElementById(nameId)?.value || state.playerName;
  const cleanCode = rawCode.trim().toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 6);
  // O servidor de sinalização só aceita salas com exatamente 6 letras/números.
  const valid = cleanCode.length === 6;
  state.roomCode = valid ? cleanCode : state.roomCode;
  state.playerName = rawName.trim().slice(0, 16) || "KELLAR";
  syncRoomFieldsToUi();
  if (!valid) showToast("O código da sala precisa ter exatamente 6 letras ou números (ex.: ABC123).", 4200);
  return valid;
}

function buildInviteUrl() {
  const url = new URL(window.location.href);
  url.searchParams.set("room", state.roomCode);
  url.searchParams.set("map", String(state.lobby.mapIndex ?? 0));
  url.searchParams.delete("mode");
  return url.toString();
}

// ---- Pré-Lobby (Escolha de Mapa, Equipes e Armaria 15 SP ANTES de iniciar o 3D) ----

function computeLobbyKitCost(primary, secondary, tactical) {
  let cost = 0;
  const p = DEFAULT_CATALOG.weapons.find((w) => w.id === primary);
  const s = DEFAULT_CATALOG.weapons.find((w) => w.id === secondary);
  if (p) cost += p.price;
  if (s) cost += s.price;
  if (tactical & TACTICAL_ARMOR) cost += DEFAULT_CATALOG.armorPrice;
  if (tactical & TACTICAL_PACK) cost += DEFAULT_CATALOG.tacticalPrice;
  return cost;
}

function buildHostLobbyRoster() {
  const list = [
    {
      peerId: 0,
      isHost: true,
      name: state.playerName,
      team: state.lobby.localTeam,
      primary: state.lobby.primary,
      secondary: state.lobby.secondary,
      tactical: state.lobby.tactical,
      ready: state.lobby.ready,
    },
  ];
  for (const peer of state.lobby.remotePeers.values()) {
    list.push({ ...peer, isHost: false });
  }
  return list;
}

function currentLobbyRoster() {
  if (state.role === "host") return buildHostLobbyRoster();
  if (state.lobby.rosterSnapshot.length > 0) return state.lobby.rosterSnapshot;
  return [
    {
      peerId: state.transport?.localSignalingId || 1,
      isHost: false,
      isLocalFallback: true,
      name: state.playerName,
      team: state.lobby.localTeam,
      primary: state.lobby.primary,
      secondary: state.lobby.secondary,
      tactical: state.lobby.tactical,
      ready: state.lobby.ready,
    },
  ];
}

function broadcastHostLobbyState() {
  if (state.role !== "host" || !state.transport) return;
  const players = buildHostLobbyRoster();
  state.transport.roomMeta = { ...state.roomRules, mapIndex: state.lobby.mapIndex };
  state.transport.broadcastLobby({
    op: "lobby_state",
    mapIndex: state.lobby.mapIndex,
    started: state.lobby.matchLaunching,
    players,
  });
  renderPreMatchLobby();
}

function sendClientLobbyProfile() {
  if (state.role !== "client" || !state.transport) return;
  const hostPeer = state.transport.hostPeerId;
  if (!hostPeer) return;
  state.transport.sendLobby(hostPeer, {
    op: "join_profile",
    name: state.playerName,
    team: state.lobby.localTeam,
    primary: state.lobby.primary,
    secondary: state.lobby.secondary,
    tactical: state.lobby.tactical,
    ready: state.lobby.ready,
  });
  renderPreMatchLobby();
}

function syncLocalLobbyChange() {
  if (state.role === "host") broadcastHostLobbyState();
  else if (state.role === "client") sendClientLobbyProfile();
  else renderPreMatchLobby();
}

function handleLobbyMessage(peerId, data) {
  if (!data || typeof data !== "object") return;
  if (state.role === "host" && data.op === "join_profile") {
    const existing = state.lobby.remotePeers.get(peerId);
    let team = data.team === 0 || data.team === 1 ? data.team : 1;
    if (!existing && data.team === undefined) {
      const roster = buildHostLobbyRoster();
      const cntA = roster.filter((p) => p.team === 0).length;
      const cntB = roster.filter((p) => p.team === 1).length;
      team = cntA <= cntB ? 0 : 1;
    }
    state.lobby.remotePeers.set(peerId, {
      peerId,
      name: String(data.name || `OP-${peerId}`).slice(0, 16),
      team,
      primary: Number.isFinite(data.primary) ? data.primary : 7,
      secondary: Number.isFinite(data.secondary) ? data.secondary : 0,
      tactical: Number.isFinite(data.tactical) ? data.tactical : 0,
      ready: Boolean(data.ready),
    });
    broadcastHostLobbyState();
    return;
  }

  if (state.role === "client") {
    if (data.op === "lobby_state") {
      if (Number.isFinite(data.mapIndex)) {
        state.lobby.mapIndex = Math.max(0, Math.min(7, data.mapIndex | 0));
        state.roomRules.mapIndex = state.lobby.mapIndex;
        syncRoomFieldsToUi();
      }
      if (Array.isArray(data.players)) {
        state.lobby.rosterSnapshot = data.players;
      }
      renderPreMatchLobby();
      if (data.started && !state.lobby.matchLaunching) {
        launchMultiplayerMatch();
      }
      return;
    }
    if (data.op === "launch_match") {
      if (Number.isFinite(data.mapIndex)) {
        state.lobby.mapIndex = Math.max(0, Math.min(7, data.mapIndex | 0));
        state.roomRules.mapIndex = state.lobby.mapIndex;
        syncRoomFieldsToUi();
      }
      showToast(`Host iniciou a partida em ${mapNameByIndex(state.lobby.mapIndex)}!`, 3200);
      launchMultiplayerMatch();
    }
  }
}

function launchMultiplayerMatch() {
  if (!isMultiplayerActive()) return;
  if (state.lobby.primary === NO_WEAPON) {
    showToast("Escolha uma arma primária na Armaria (15 SP) antes de iniciar.");
    return;
  }
  state.lobby.matchLaunching = true;
  if (state.role === "host" && state.transport) {
    state.transport.broadcastLobby({
      op: "launch_match",
      mapIndex: state.lobby.mapIndex,
    });
  }
  if (engine.module?._BlackWebConfigurePreMatch) {
    engine.module._BlackWebConfigurePreMatch(
      state.lobby.mapIndex & 7,
      state.lobby.localTeam & 1,
      state.lobby.primary & 0xff,
      state.lobby.secondary & 0xff,
      state.lobby.tactical & 0xff
    );
  }
  startWasmEngine("mp");
}

function renderPreMatchLobby() {
  const isHost = state.role === "host";
  const roster = currentLobbyRoster();
  const teamA = roster.filter((p) => p.team === 0);
  const teamB = roster.filter((p) => p.team === 1);

  const titleEl = document.getElementById("lobby-title");
  if (titleEl) titleEl.textContent = `SALA ${state.roomCode} · ${mapNameByIndex(state.lobby.mapIndex).toUpperCase()}`;

  const connPill = document.getElementById("lobby-conn-pill");
  if (connPill) {
    const peers = state.transport?.peers?.size || 0;
    connPill.dataset.mode = state.role;
    connPill.textContent = isHost
      ? `HOST · ${roster.length}/4 OPERADORES`
      : peers > 0
        ? `CONECTADO · ${roster.length}/4 OPERADORES`
        : "PROCURANDO HOST...";
  }

  const mapSel = document.getElementById("lobby-map-select");
  if (mapSel) {
    mapSel.value = String(state.lobby.mapIndex);
    mapSel.disabled = !isHost;
  }

  const renderRoster = (containerId, list) => {
    const box = document.getElementById(containerId);
    if (!box) return;
    box.replaceChildren();
    if (list.length === 0) {
      const empty = document.createElement("div");
      empty.className = "team-player-sub";
      empty.textContent = "Nenhum operador neste time.";
      box.appendChild(empty);
      return;
    }
    for (const p of list) {
      const isSelf =
        (isHost && p.isHost) ||
        (!isHost && (p.isLocalFallback || p.peerId === state.transport?.localSignalingId || p.name === state.playerName));
      const row = document.createElement("div");
      row.className = `team-player${isSelf ? " is-self" : ""}`;
      const left = document.createElement("div");
      const nameDiv = document.createElement("div");
      nameDiv.textContent = `${p.name}${p.isHost ? " [HOST]" : ""}${isSelf ? " (VOCÊ)" : ""}`;
      const subDiv = document.createElement("div");
      subDiv.className = "team-player-sub";
      subDiv.textContent = `${weaponNameById(p.primary)} + ${weaponNameById(p.secondary)}`;
      left.append(nameDiv, subDiv);
      const right = document.createElement("span");
      right.className = "ready-dot";
      right.textContent = p.ready ? "PRONTO" : "MONTANDO";
      if (!p.ready) right.style.color = "var(--text-muted)";
      row.append(left, right);
      box.appendChild(row);
    }
  };
  renderRoster("lobby-team-a-list", teamA);
  renderRoster("lobby-team-b-list", teamB);

  const btnJoinA = document.getElementById("btn-lobby-join-a");
  const btnJoinB = document.getElementById("btn-lobby-join-b");
  if (btnJoinA) {
    btnJoinA.classList.toggle("active", state.lobby.localTeam === 0);
    btnJoinA.textContent = state.lobby.localTeam === 0 ? "Seu Time" : "Entrar";
  }
  if (btnJoinB) {
    btnJoinB.classList.toggle("active", state.lobby.localTeam === 1);
    btnJoinB.textContent = state.lobby.localTeam === 1 ? "Seu Time" : "Entrar";
  }

  // Armaria 15 SP
  const used = computeLobbyKitCost(state.lobby.primary, state.lobby.secondary, state.lobby.tactical);
  const free = Math.max(0, 15 - used);
  const usedEl = document.getElementById("lobby-sp-used");
  const freeEl = document.getElementById("lobby-sp-free");
  const fillEl = document.getElementById("lobby-sp-fill");
  const sumEl = document.getElementById("lobby-kit-summary");
  if (usedEl) usedEl.textContent = `${used} / 15 SP`;
  if (freeEl) freeEl.textContent = `${free} SP`;
  if (fillEl) fillEl.style.width = `${Math.min(100, Math.round((used / 15) * 100))}%`;
  if (sumEl) {
    const extras = [];
    if (state.lobby.tactical & TACTICAL_ARMOR) extras.push("Colete +25 HP");
    if (state.lobby.tactical & TACTICAL_PACK) extras.push("Silenciador/Tático");
    sumEl.innerHTML = `Primária: <strong>${weaponNameById(state.lobby.primary)}</strong> · Secundária: <strong>${weaponNameById(state.lobby.secondary)}</strong> · Proteção: <strong>${extras.length ? extras.join(" + ") : "Padrão"}</strong>`;
  }

  const primList = document.getElementById("lobby-primary-list");
  if (primList) {
    primList.replaceChildren();
    const curPrimPrice = DEFAULT_CATALOG.weapons.find((x) => x.id === state.lobby.primary)?.price || 0;
    for (const w of DEFAULT_CATALOG.weapons.filter((x) => x.slot !== 0)) {
      const equipped = state.lobby.primary === w.id;
      const affordable = used - curPrimPrice + w.price <= 15;
      primList.appendChild(
        buildBuyButton({
          name: w.name,
          meta: `DMG ${w.damage} · ${w.mag} balas · ${w.range}m`,
          price: w.price,
          isTA: true,
          equipped,
          affordable,
          available: true,
          onClick: () => {
            state.lobby.primary = w.id;
            syncLocalLobbyChange();
          },
        })
      );
    }
  }

  const secList = document.getElementById("lobby-secondary-list");
  if (secList) {
    secList.replaceChildren();
    const curSecPrice = DEFAULT_CATALOG.weapons.find((x) => x.id === state.lobby.secondary)?.price || 0;
    for (const w of DEFAULT_CATALOG.weapons.filter((x) => x.slot === 0)) {
      const equipped = state.lobby.secondary === w.id;
      const affordable = used - curSecPrice + w.price <= 15;
      secList.appendChild(
        buildBuyButton({
          name: w.name,
          meta: `DMG ${w.damage} · MAG ${w.mag}`,
          price: w.price,
          isTA: true,
          equipped,
          affordable,
          available: true,
          onClick: () => {
            state.lobby.secondary = w.id;
            syncLocalLobbyChange();
          },
        })
      );
    }

    const hasArmor = (state.lobby.tactical & TACTICAL_ARMOR) !== 0;
    secList.appendChild(
      buildBuyButton({
        name: "Colete Balístico (+25 HP)",
        meta: hasArmor ? "Clique p/ remover (+3 SP)" : "Proteção extra ao spawnar",
        price: DEFAULT_CATALOG.armorPrice,
        isTA: true,
        equipped: hasArmor,
        affordable: hasArmor || used + DEFAULT_CATALOG.armorPrice <= 15,
        available: true,
        onClick: () => {
          state.lobby.tactical = hasArmor ? (state.lobby.tactical & ~TACTICAL_ARMOR) : (state.lobby.tactical | TACTICAL_ARMOR);
          syncLocalLobbyChange();
        },
      })
    );

    const hasPack = (state.lobby.tactical & TACTICAL_PACK) !== 0;
    secList.appendChild(
      buildBuyButton({
        name: "Silenciador / Pacote Tático",
        meta: hasPack ? "Clique p/ remover (+2 SP)" : "Acopla silenciador na arma",
        price: DEFAULT_CATALOG.tacticalPrice,
        isTA: true,
        equipped: hasPack,
        affordable: hasPack || used + DEFAULT_CATALOG.tacticalPrice <= 15,
        available: true,
        onClick: () => {
          state.lobby.tactical = hasPack ? (state.lobby.tactical & ~TACTICAL_PACK) : (state.lobby.tactical | TACTICAL_PACK);
          syncLocalLobbyChange();
        },
      })
    );
  }

  const readyBtn = document.getElementById("btn-lobby-ready");
  if (readyBtn) {
    readyBtn.textContent = state.lobby.ready ? "✓ Pronto (Confirmado)" : "✓ Marcar Pronto";
    readyBtn.classList.toggle("btn-primary", state.lobby.ready);
  }

  const startBtn = document.getElementById("btn-lobby-start");
  const hintEl = document.getElementById("lobby-launch-hint");
  const canStart = isHost && teamA.length >= 1 && teamB.length >= 1 && state.lobby.primary !== NO_WEAPON;
  if (startBtn) {
    startBtn.classList.toggle("hidden", !isHost);
    startBtn.disabled = !canStart;
    startBtn.textContent = `▶ Iniciar em ${mapNameByIndex(state.lobby.mapIndex)}`;
  }
  if (hintEl) {
    if (!isHost) {
      const peers = state.transport?.peers?.size || 0;
      hintEl.textContent =
        peers > 0
          ? `Conectado à sala ${state.roomCode}. Escolha seu time e kit de 15 SP — aguardando o Host iniciar a partida em ${mapNameByIndex(state.lobby.mapIndex)}.`
          : `Procurando host da sala ${state.roomCode}...`;
    } else if (canStart) {
      hintEl.textContent = `Sala pronta (${teamA.length} vs ${teamB.length})! Clique em Iniciar para carregar ${mapNameByIndex(state.lobby.mapIndex)} direto para todos.`;
    } else {
      hintEl.textContent = `Aguardando pelo menos 1 operador em cada time (Time A: ${teamA.length} · Time B: ${teamB.length}). Copie o link de convite acima!`;
    }
  }
}

async function copyInviteLink(preferOptionsModal = false) {
  if (!readIdentityFromInputs(preferOptionsModal)) return;
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
  const mode = Number(rules.gameMode || 0);
  const callRole = (roleInt, hostPeer) => {
    if (mod._BlackWebSetRoleMode) {
      mod.ccall(
        "BlackWebSetRoleMode",
        "number",
        ["number", "string", "number", "number", "number", "number", "number"],
        [roleInt, state.playerName, hostPeer, roleInt === 0 ? 0 : 1, rules.fixedBudget ? 1 : 0, rules.budgetCap, roleInt === 0 ? 0 : mode]
      );
      return;
    }
    if (mod._BlackWebSetRoleEx) {
      mod.ccall(
        "BlackWebSetRoleEx",
        "number",
        ["number", "string", "number", "number", "number", "number"],
        [roleInt, state.playerName, hostPeer, roleInt === 0 ? 0 : 1, rules.fixedBudget ? 1 : 0, rules.budgetCap]
      );
      return;
    }
    mod.ccall("BlackWebSetRole", "number", ["number", "string", "number"], [roleInt, state.playerName, hostPeer]);
  };

  if (roleName === "offline") {
    callRole(0, 0);
  } else if (roleName === "host") {
    callRole(1, 0);
    if (state.transport) {
      for (const peerId of state.transport.peers.keys()) {
        mod._BlackWebPushIncomingNet(0, Number(peerId) >>> 0, 0, 0, 0);
      }
    }
  } else if (roleName === "client") {
    const hostPeer = Number(state.transport?.hostPeerId || 0) >>> 0;
    if (hostPeer > 0) {
      engine.wasmClientConfiguredPeer = hostPeer;
      callRole(2, hostPeer);
      mod._BlackWebPushIncomingNet(0, hostPeer, 0, 0, 0);
    }
  }
  if ((roleName === "host" || roleName === "client") && state.lobby.matchLaunching && mod._BlackWebConfigurePreMatch) {
    mod._BlackWebConfigurePreMatch(
      state.lobby.mapIndex | 0,
      state.lobby.localTeam | 0,
      state.lobby.primary | 0,
      state.lobby.secondary | 0,
      state.lobby.tactical | 0,
    );
  }
  refreshCatalogFromWasm();
}

function startSession(preferredRole, fromOptionsModal = false) {
  if (state.transport) {
    state.transport.close();
    state.transport = null;
  }
  const codeOk = readIdentityFromInputs(fromOptionsModal);
  if (!codeOk && preferredRole !== "offline") return;
  const signalingUrl = (document.getElementById("signaling-url-input")?.value || "").trim();
  const roomRules = readRoomConfigFromUi();

  state.packetsTx = 0;
  state.packetsRx = 0;
  state.lobby.remotePeers.clear();
  state.lobby.rosterSnapshot = [];
  state.lobby.ready = false;
  state.lobby.matchLaunching = false;

  if (preferredRole === "offline") {
    state.role = "offline";
    state.localPlayerId = 0;
    if (engine.activeOverlay === "buy" || engine.activeOverlay === "lobby") {
      setOverlay(engine.bootStarted ? "none" : "launcher");
    }
    setScoreboardVisible(false);
    syncWasmRole("offline");
    logLine("MODO", "Campanha Solo selecionada (OfflineCampaign intacto).");
    updateTelemetryUi();
    updateViewportChrome();
    return;
  }

  state.role = preferredRole === "host" ? "host" : "client";
  state.localPlayerId = state.role === "host" ? 0 : 1;
  state.lobby.localTeam = state.role === "host" ? 0 : 1;

  state.transport = new WebRtcTransport({
    roomCode: state.roomCode,
    signalingUrl,
    roomMeta: roomRules,
    onLog: (msg) => logLine("WEBRTC", msg),
    onEvent: (ev) => handleTransportEvent(ev),
  });

  state.transport.start(preferredRole);
  syncWasmRole(preferredRole);
  if (!engine.bootStarted) {
    setOverlay("lobby");
  }
  updateTelemetryUi();
  updateViewportChrome();
}

function handleTransportEvent(ev) {
  if (ev.type === "lobby_msg") {
    handleLobbyMessage(Number(ev.peer) >>> 0, ev.data);
    return;
  }

  if (ev.type === "room_meta" && ev.meta) {
    const mapIdx = Number.isFinite(ev.meta.mapIndex) ? Math.max(0, Math.min(7, ev.meta.mapIndex | 0)) : state.lobby.mapIndex;
    state.lobby.mapIndex = mapIdx;
    state.roomRules = {
      gameMode: 2,
      csLoadout: true,
      fixedBudget: true,
      budgetCap: 15,
      mapIndex: mapIdx,
    };
    syncRoomFieldsToUi();
    logLine("WEBRTC", `Sala ${state.roomCode} sincronizada (Territorial Assault · ${mapNameByIndex(mapIdx)} · 15 SP).`);
    renderPreMatchLobby();
    updateTelemetryUi();
    updateViewportChrome();
    return;
  }

  if (ev.type === "role") {
    state.role = ev.role;
    syncWasmRole(ev.role);
    renderPreMatchLobby();
    updateTelemetryUi();
    updateViewportChrome();
    return;
  }

  if (ev.type === "connected") {
    const peerId = Number(ev.peer) >>> 0;
    logLine("WEBRTC", `DataChannel P2P ativo com par #${peerId} na sala ${state.roomCode}.`);
    if (state.role === "host") {
      if (!state.lobby.remotePeers.has(peerId)) {
        const roster = buildHostLobbyRoster();
        const cntA = roster.filter((p) => p.team === 0).length;
        const cntB = roster.filter((p) => p.team === 1).length;
        state.lobby.remotePeers.set(peerId, {
          peerId,
          name: `OP-${peerId % 100}`,
          team: cntA <= cntB ? 0 : 1,
          primary: 7,
          secondary: 0,
          tactical: 0,
          ready: false,
        });
      }
      broadcastHostLobbyState();
    } else if (state.role === "client") {
      sendClientLobbyProfile();
    }
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
    state.lobby.remotePeers.delete(peerId);
    if (state.role === "host") broadcastHostLobbyState();
    else renderPreMatchLobby();
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
  const isTA = isTerritorialMode();
  if (isTA && nextPrimary === NO_WEAPON) {
    if (feedback) {
      feedback.textContent = "Selecione primeiro uma arma primária (obrigatória no Territorial Assault, limite de 15 SP).";
      feedback.dataset.tone = "err";
    }
    return;
  }
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
      feedback.textContent = ok && cur.lastResult === 0
        ? loadoutResultLabel(0, isTA)
        : loadoutResultLabel(cur.lastResult, isTA);
      feedback.dataset.tone = ok && cur.lastResult === 0 ? "ok" : "err";
    }
    renderBuyMenu(true);
  }, 50);
}

function updateBuyMenuHeader(cur) {
  const isTA = isTerritorialMode();
  const titleEl = document.getElementById("buy-modal-title");
  const creditsEl = document.getElementById("buy-credits-val");
  const capEl = document.getElementById("buy-cap-val");
  const kitEl = document.getElementById("buy-kit-cost-val");
  const winEl = document.getElementById("buy-window-val");
  const sumEl = document.getElementById("buy-current-kit");

  if (titleEl) {
    titleEl.textContent = isTA
      ? "Armaria Tática · Territorial Assault (15 Supply Points)"
      : "Loja de Loadout CS · Equipamento com Limite";
  }
  const remaining = cur.fixedBudget ? Math.max(0, cur.maxCredits - cur.kitCost) : cur.credits;
  if (creditsEl) creditsEl.textContent = formatBudget(remaining, isTA);
  if (capEl) capEl.textContent = formatBudget(cur.maxCredits, isTA);
  if (kitEl) kitEl.textContent = formatBudget(cur.kitCost, isTA);
  if (winEl) {
    if (isTA) {
      winEl.textContent = cur.buyTicksLeft > 0 ? "PREP / INTERVALO" : "BLOQUEADO EM COMBATE";
    } else {
      winEl.textContent =
        cur.buyTicksLeft === 0xffff ? "LIVRE" : cur.buyTicksLeft > 0 ? `${Math.ceil(cur.buyTicksLeft / 30)}s` : "FECHADA";
    }
  }

  const extras = [];
  if (cur.tactical & TACTICAL_ARMOR) extras.push("Colete +25 HP");
  if (cur.tactical & TACTICAL_PACK) extras.push("Kit Tático/Silenciador");
  if (sumEl) {
    sumEl.innerHTML = `Primária: <strong>${weaponNameById(cur.primary)}</strong><br />Secundária: <strong>${weaponNameById(cur.secondary)}</strong><br />Proteção: <strong>${extras.length ? extras.join(" + ") : "Padrão"}</strong>`;
  }
}

const BLACK_LEVEL_NAMES = {
  0: "01 · Veblensk City Streets",
  1: "02 · Treneska Border Crossing",
  3: "04 · Tivliz Asylum",
  4: "05 · Veblensk Dockyard",
  5: "03 · Naszran Foundry",
  6: "06 · Graznei Bridge",
  7: "07 · Spetriniv Gulag",
  8: "08 · Spetriniv (Final)",
};

function levelLabel(id) {
  return BLACK_LEVEL_NAMES[Number(id)] || `Fase #${Number(id) || 0}`;
}

function buildBuyButton({ hotkey, name, meta, price, isTA = false, equipped, affordable, available = true, onClick }) {
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
  right.textContent = !available ? "N/D" : equipped ? "EQUIPADO" : price === 0 ? "GRÁTIS" : formatBudget(price, isTA);

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

  const isTA = isTerritorialMode();
  const cur = currentLoadoutNumbers();
  updateBuyMenuHeader(cur);

  const windowOpen = cur.buyTicksLeft > 0;
  const availMask = state.catalog.availableMask ?? 0xffffffff;
  const engCnt = state.catalog.engineWeaponCount ?? 0;
  const btnKey = `${cur.credits}-${cur.primary}-${cur.secondary}-${cur.tactical}-${cur.lastResult}-${windowOpen ? 1 : 0}-${cur.fixedBudget ? 1 : 0}-${isTA ? 1 : 0}-${state.catalog.weapons.length}-${availMask}-${engCnt}`;
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
        isTA,
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
        isTA,
        equipped,
        affordable,
        available,
        onClick: () => submitLoadoutPurchase(w.id, cur.secondary, cur.tactical),
      })
    );
  });

  const hasArmor = (cur.tactical & TACTICAL_ARMOR) !== 0;
  const armorPrice = state.catalog.armorPrice ?? (isTA ? 2 : 650);
  const armorAffordable = windowOpen && (cur.fixedBudget ? (hasArmor || cur.kitCost + armorPrice <= cur.maxCredits) : armorPrice <= cur.credits);
  colTac.appendChild(
    buildBuyButton({
      hotkey: "",
      name: `Colete Balístico (+${state.catalog.armorBonus || 25} HP)`,
      meta: cur.fixedBudget && hasArmor ? "Clique novamente para remover e liberar SP" : "Proteção extra ao equipar e spawnar",
      price: armorPrice,
      isTA,
      equipped: hasArmor,
      affordable: armorAffordable,
      onClick: () =>
        submitLoadoutPurchase(
          cur.primary,
          cur.secondary,
          cur.fixedBudget && hasArmor ? (cur.tactical & ~TACTICAL_ARMOR) : (cur.tactical | TACTICAL_ARMOR)
        ),
    })
  );

  const hasPack = (cur.tactical & TACTICAL_PACK) !== 0;
  const tacPrice = state.catalog.tacticalPrice ?? (isTA ? 1 : 300);
  const tacAffordable = windowOpen && (cur.fixedBudget ? (hasPack || cur.kitCost + tacPrice <= cur.maxCredits) : tacPrice <= cur.credits);
  colTac.appendChild(
    buildBuyButton({
      hotkey: "",
      name: "Pacote Tático (Silenciador / Granada)",
      meta: cur.fixedBudget && hasPack ? "Clique novamente para remover e liberar SP" : "Acopla silenciador na arma equipada",
      price: tacPrice,
      isTA,
      equipped: hasPack,
      affordable: tacAffordable,
      onClick: () =>
        submitLoadoutPurchase(
          cur.primary,
          cur.secondary,
          cur.fixedBudget && hasPack ? (cur.tactical & ~TACTICAL_PACK) : (cur.tactical | TACTICAL_PACK)
        ),
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
    const tp = engine.touchPad;
    if (tp?.active) {
      // Analógico virtual da tela de toque: entra como gamepad com só o stick esquerdo.
      mod._BlackWebPushGamepadState(1, 0, 0, 0, tp.lx | 0, tp.ly | 0, 0, 0);
    } else {
      mod._BlackWebPushGamepadState(0, 0, 0, 0, 0, 0, 0, 0);
    }
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

    // Se o cliente ainda não entrou no papel WASM=2 (ex.: hostPeerId descoberto após o boot), configura uma única vez.
    // Quando st.role já é 2, o próprio mp_bridge reenvia Hello a cada 30 quadros sem destruir a sessão nem cancelar o direct launch.
    if (
      engine.bootStarted &&
      state.role === "client" &&
      Number(st.role || 0) !== 2 &&
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
          feedback.textContent = loadoutResultLabel(cur.lastResult, isTerritorialMode());
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
    const gmVal = st.gameMode ?? 0;
    if (
      state.lastCatalogMask !== maskVal ||
      state.lastCatalogEngineCount !== engCnt ||
      state.lastCatalogInLevel !== inLvl ||
      state.lastCatalogGameMode !== gmVal
    ) {
      state.lastCatalogMask = maskVal;
      state.lastCatalogEngineCount = engCnt;
      state.lastCatalogInLevel = inLvl;
      state.lastCatalogGameMode = gmVal;
      refreshCatalogFromWasm();
      if (engine.activeOverlay === "buy") {
        renderBuyMenu(true);
      }
    }

    if (engine.activeOverlay === "buy") {
      const buyOpen = (st.buyTicksLeft || 0) > 0 ? 1 : 0;
      const isTA = gmVal === 2;
      const btnKey = `${st.credits}-${st.primaryWeapon}-${st.secondaryWeapon}-${st.tacticalFlags}-${st.lastLoadoutResult}-${buyOpen}-${st.fixedBudget ? 1 : 0}-${isTA ? 1 : 0}-${state.catalog.weapons.length}-${maskVal}-${engCnt}`;
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

const taHudFx = {
  prevPhase: -1,
  prevCaptured: -1,
  prevOvertime: false,
  prevAtkWaves: -1,
  prevDefWaves: -1,
  prevPlayers: new Map(),
  killFeed: [],
  bannerUntil: 0,
  resultsOpened: false,
};

function pushTaKillFeed(html, tone = "info") {
  const now = performance.now();
  taHudFx.killFeed.unshift({ html, tone, expiresAt: now + 5500 });
  if (taHudFx.killFeed.length > 4) taHudFx.killFeed.length = 4;
  renderTaKillFeed();
}

function renderTaKillFeed() {
  const el = document.getElementById("ta-killfeed");
  if (!el) return;
  const now = performance.now();
  taHudFx.killFeed = taHudFx.killFeed.filter((item) => item.expiresAt > now);
  if (taHudFx.killFeed.length === 0) {
    if (el.childElementCount > 0) el.innerHTML = "";
    return;
  }
  el.innerHTML = taHudFx.killFeed
    .map((item) => `<div class="ta-kf-row" data-tone="${item.tone}">${item.html}</div>`)
    .join("");
}

function showTaEventBanner(title, sub, tone = "atk", durationMs = 3400) {
  const el = document.getElementById("ta-event-banner");
  if (!el) return;
  const tEl = document.getElementById("ta-event-title");
  const sEl = document.getElementById("ta-event-sub");
  el.dataset.tone = tone;
  if (tEl) tEl.textContent = title;
  if (sEl) sEl.textContent = sub;
  taHudFx.bannerUntil = performance.now() + durationMs;
  el.classList.remove("hidden");
}

function renderTaWaypoint(st, ta) {
  const wp = document.getElementById("ta-waypoint");
  if (!wp) return;
  const phase = Number(ta.state || 0);
  const inActiveRound = phase === 2 || phase === 4;
  if (!inActiveRound || !ta.hasBeacon || !st?.inLevel || engine.activeOverlay !== "none") {
    wp.classList.add("hidden");
    return;
  }
  const objLetter = "ABCDEFGH"[Number(ta.currentObj || 0)] || "A";
  const contested = Number(ta.objState || 0) === 2;
  const inZone = Boolean(ta.inZone);
  const distM = Math.max(0, Math.round(Number(ta.distM || 0)));
  const bearingDeg = Number(ta.bearingDeg || 0);
  const elevDeg = Number(ta.elevDeg || 0);
  const hFov = Math.max(60, Math.min(115, Number(state.enhanced.fovDeg || 88)));
  const vFov = hFov * (9 / 16);

  const inFront = Math.abs(bearingDeg) < 88;
  let sx = 50;
  let sy = 50;
  if (inFront) {
    const tanHalfH = Math.tan(((hFov * 0.5) * Math.PI) / 180);
    const tanHalfV = Math.tan(((vFov * 0.5) * Math.PI) / 180);
    const bRad = (bearingDeg * Math.PI) / 180;
    const eRad = (elevDeg * Math.PI) / 180;
    const nx = Math.tan(bRad) / Math.max(0.15, tanHalfH);
    const ny = Math.tan(eRad) / Math.max(0.15, tanHalfV);
    sx = 50 + nx * 42;
    sy = 50 - ny * 40;
  } else {
    sx = bearingDeg >= 0 ? 92 : 8;
    sy = 56;
  }

  const clampedX = Math.max(7, Math.min(93, sx));
  const clampedY = Math.max(18, Math.min(82, sy));
  const offScreen = !inFront || sx < 7 || sx > 93 || sy < 18 || sy > 82;

  wp.style.left = `${clampedX.toFixed(1)}%`;
  wp.style.top = `${clampedY.toFixed(1)}%`;
  wp.dataset.state = inZone ? "inzone" : contested ? "contested" : "active";
  wp.classList.remove("hidden");

  const letterEl = document.getElementById("ta-waypoint-letter");
  const distEl = document.getElementById("ta-waypoint-dist");
  if (letterEl) letterEl.textContent = objLetter;
  if (distEl) {
    if (inZone) {
      distEl.textContent = contested ? `CONTESTADO` : `NA ZONA`;
    } else if (offScreen) {
      const dir = bearingDeg >= 0 ? "►" : "◄";
      distEl.textContent = `${dir} ${distM}m`;
    } else {
      distEl.textContent = `${distM}m`;
    }
  }
}

function renderTaRadar(st, ta) {
  const canvas = document.getElementById("ta-radar");
  if (!canvas || !st?.cam || !st?.inLevel || engine.activeOverlay !== "none") return;
  const ctx = canvas.getContext("2d");
  if (!ctx) return;

  const w = canvas.width;
  const h = canvas.height;
  const cx = w * 0.5;
  const cy = h * 0.5;
  const rangeM = 35.0;
  const scale = (w * 0.45) / rangeM;

  ctx.clearRect(0, 0, w, h);

  // Anéis táticos de alcance (15m / 30m) e cruz central
  ctx.strokeStyle = "rgba(148, 163, 184, 0.16)";
  ctx.lineWidth = 1;
  for (const rM of [15, 30]) {
    ctx.beginPath();
    ctx.arc(cx, cy, rM * scale, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.beginPath();
  ctx.moveTo(cx, 6);
  ctx.lineTo(cx, h - 6);
  ctx.moveTo(6, cy);
  ctx.lineTo(w - 6, cy);
  ctx.stroke();

  const cam = st.cam;
  const fLen = Math.hypot(cam.fx || 0, cam.fz || 1) || 1;
  const fx = (cam.fx || 0) / fLen;
  const fz = (cam.fz || 1) / fLen;
  const rx = fz;
  const rz = -fx;

  const toRadar = (wx, wz) => {
    const dx = wx - cam.x;
    const dz = wz - cam.z;
    const localR = dx * rx + dz * rz;
    const localF = dx * fx + dz * fz;
    let px = cx + localR * scale;
    let py = cy - localF * scale;
    const dist = Math.hypot(px - cx, py - cy);
    const maxR = w * 0.45;
    if (dist > maxR) {
      px = cx + ((px - cx) / dist) * maxR;
      py = cy + ((py - cy) / dist) * maxR;
    }
    return { px, py, clamped: dist > maxR };
  };

  // Norte (N) na borda do radar
  const nPos = toRadar(cam.x, cam.z + rangeM * 1.1);
  ctx.fillStyle = "rgba(245, 158, 11, 0.8)";
  ctx.font = "bold 9px monospace";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText("N", nPos.px, nPos.py);

  // Zona de captura ativa
  const phase = Number(ta.state || 0);
  if ((phase === 2 || phase === 4) && Number(ta.zr || 0) > 0.1) {
    const zp = toRadar(Number(ta.zx || 0), Number(ta.zz || 0));
    const zrPx = Math.max(6, Math.min(26, Number(ta.zr || 7.5) * scale));
    const contested = Number(ta.objState || 0) === 2;
    const inZone = Boolean(ta.inZone);
    ctx.fillStyle = inZone
      ? "rgba(16, 185, 129, 0.22)"
      : contested
        ? "rgba(239, 68, 68, 0.25)"
        : "rgba(245, 158, 11, 0.18)";
    ctx.strokeStyle = inZone ? "#10b981" : contested ? "#ef4444" : "#f59e0b";
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    ctx.arc(zp.px, zp.py, zrPx, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();

    const objLetter = "ABCDEFGH"[Number(ta.currentObj || 0)] || "A";
    ctx.fillStyle = "#f8fafc";
    ctx.font = "bold 10px monospace";
    ctx.fillText(objLetter, zp.px, zp.py + 0.5);
  }

  // Operadores aliados e adversários próximos
  const localId = st.localSlot ?? state.localPlayerId;
  const localTeam = Number(ta.localTeam || 0);
  if (Array.isArray(st.players)) {
    for (const p of st.players) {
      if (!p.alive || p.id === localId) continue;
      const sameTeam = Number(p.team || 0) === localTeam;
      const d = Math.hypot((p.x || 0) - cam.x, (p.z || 0) - cam.z);
      if (!sameTeam && d > 30) continue;
      const pt = toRadar(Number(p.x || 0), Number(p.z || 0));
      ctx.fillStyle = sameTeam ? "#38bdf8" : "#ef4444";
      ctx.beginPath();
      ctx.arc(pt.px, pt.py, sameTeam ? 3.5 : 3.2, 0, Math.PI * 2);
      ctx.fill();
    }
  }

  // Jogador local (seta branca no centro apontando para frente)
  ctx.fillStyle = "#f8fafc";
  ctx.beginPath();
  ctx.moveTo(cx, cy - 5);
  ctx.lineTo(cx + 4, cy + 4);
  ctx.lineTo(cx, cy + 2);
  ctx.lineTo(cx - 4, cy + 4);
  ctx.closePath();
  ctx.fill();
}

function renderResultsModal(st) {
  if (!st?.ta) return;
  const ta = st.ta;
  const localTeam = Number(ta.localTeam || 0);
  const winner = Number(ta.winner ?? 255);
  const outcome = winner === 255 ? "draw" : winner === localTeam ? "win" : "loss";

  const hero = document.getElementById("results-hero");
  const headline = document.getElementById("results-headline");
  const sub = document.getElementById("results-sub");
  const kda = document.getElementById("results-kda");
  const mapPill = document.getElementById("results-map-pill");
  const r1El = document.getElementById("results-r1-val");
  const r2El = document.getElementById("results-r2-val");
  const tbody = document.getElementById("results-score-body");

  if (hero) hero.dataset.outcome = outcome;
  if (headline) {
    headline.textContent =
      outcome === "win"
        ? `VITÓRIA · TIME ${localTeam === 0 ? "A" : "B"}`
        : outcome === "loss"
          ? `DERROTA · VENCEDOR TIME ${winner === 0 ? "A" : "B"}`
          : "EMPATE TÁTICO";
  }
  if (sub) {
    sub.textContent = `Sala ${state.roomCode} · ${levelLabel(st.arenaId)} · 2 rodadas com troca de lados`;
  }
  if (kda) kda.textContent = `${st.kills || 0} ABATES · ${st.deaths || 0} MORTES`;
  if (mapPill) mapPill.textContent = levelLabel(st.arenaId).toUpperCase();
  if (r1El) {
    r1El.textContent = `${ta.r1Cap || 0}/${ta.objCount || 2} objetivos (${Math.round((ta.r1Prog || 0) / 10)}%) · ${formatClock(ta.r1Time)}`;
  }
  if (r2El) {
    r2El.textContent = `${ta.r2Cap || 0}/${ta.objCount || 2} objetivos (${Math.round((ta.r2Prog || 0) / 10)}%) · ${formatClock(ta.r2Time)}`;
  }

  if (tbody) {
    const localId = st.localSlot ?? state.localPlayerId;
    const rows = Array.isArray(st.players)
      ? [...st.players].sort((a, b) => (a.team ?? 0) - (b.team ?? 0) || b.kills - a.kills)
      : [];
    tbody.innerHTML = "";
    for (const op of rows) {
      const kit =
        op.primary !== undefined && op.primary !== NO_WEAPON
          ? `${weaponNameById(op.primary)} + ${weaponNameById(op.secondary)}`
          : weaponNameById(op.secondary ?? op.weapon ?? 0);
      const tr = document.createElement("tr");
      tr.innerHTML = `
        <td>${op.name}${op.id === localId ? " (VOCÊ)" : ""}</td>
        <td><span class="team-tag ${(op.team ?? 0) === 0 ? "team-a" : "team-b"}">${(op.team ?? 0) === 0 ? "TIME A" : "TIME B"}</span></td>
        <td class="mono">${kit}</td>
        <td class="mono">${op.kills}</td>
        <td class="mono">${op.deaths}</td>
      `;
      tbody.appendChild(tr);
    }
  }
}

function updateTaDynamicEvents(st, ta) {
  const phase = Number(ta.state || 0);
  const localTeam = Number(ta.localTeam || 0);
  const atkTeam = Number(ta.attackerTeam || 0);
  const isAttacker = localTeam === atkTeam;

  // Transições de fase -> banner tático central
  if (taHudFx.prevPhase !== -1 && taHudFx.prevPhase !== phase) {
    if (phase === 2) {
      showTaEventBanner(
        `RODADA 1 · ${isAttacker ? "ATAQUE" : "DEFESA"}`,
        isAttacker ? "Avance e capture os objetivos em sequência." : "Segure a linha e esgote as ondas atacantes.",
        isAttacker ? "atk" : "def",
        3800,
      );
    } else if (phase === 3) {
      showTaEventBanner(
        "INTERVALO · TROCA DE LADOS",
        "Papéis invertidos na Rodada 2. Pressione [B] para ajustar seu kit de 15 SP.",
        "ok",
        4200,
      );
      pushTaKillFeed("INTERVALO — TROCA DE LADOS (RODADA 2)", "obj");
    } else if (phase === 4) {
      showTaEventBanner(
        `RODADA 2 · ${isAttacker ? "ATAQUE" : "DEFESA"}`,
        isAttacker ? `Bata o placar da R1 (${ta.r1Cap} obj em ${formatClock(ta.r1Time)}).` : "Defenda o território até o fim do tempo.",
        isAttacker ? "atk" : "def",
        3800,
      );
    } else if (phase === 5 && !taHudFx.resultsOpened) {
      taHudFx.resultsOpened = true;
      setOverlay("results");
    }
  }
  if (phase !== 5) taHudFx.resultsOpened = false;

  // Captura de objetivo
  const captured = Number(ta.captured || 0);
  if (taHudFx.prevCaptured !== -1 && captured > taHudFx.prevCaptured && (phase === 2 || phase === 4)) {
    const letter = "ABCDEFGH"[Math.max(0, captured - 1)] || "A";
    showTaEventBanner(`OBJETIVO ${letter} CAPTURADO!`, "Reforços concedidos aos atacantes · Avance ao próximo setor.", "ok", 3400);
    pushTaKillFeed(`OBJETIVO ${letter} CAPTURADO`, "obj");
  }

  // Overtime
  if (!taHudFx.prevOvertime && Boolean(ta.overtime)) {
    showTaEventBanner("OVERTIME!", "Dispute a zona ativa — sem atacantes na zona a rodada encerra.", "atk", 3200);
    pushTaKillFeed("OVERTIME ATIVADO", "obj");
  }

  // Detecção de abates/mortes para Kill Feed
  if (Array.isArray(st.players)) {
    const killers = [];
    const victims = [];
    for (const p of st.players) {
      const prev = taHudFx.prevPlayers.get(p.id);
      if (prev) {
        if (p.kills > prev.kills) killers.push(p);
        if (p.deaths > prev.deaths) victims.push(p);
      }
      taHudFx.prevPlayers.set(p.id, {
        kills: p.kills,
        deaths: p.deaths,
        alive: p.alive,
        name: p.name,
        weapon: p.weapon,
      });
    }
    for (let i = 0; i < victims.length; i++) {
      const v = victims[i];
      const k = killers[i] || killers[0];
      if (k && k.id !== v.id) {
        const wName = weaponNameById(k.weapon ?? k.primary ?? 0);
        pushTaKillFeed(`<strong>${k.name}</strong> <span class="ta-kf-wpn">${wName}</span> <strong>${v.name}</strong>`, "kill");
      } else {
        pushTaKillFeed(`<strong>${v.name}</strong> foi eliminado`, "kill");
      }
    }
  }

  // Ocultar banner após expirar
  const bannerEl = document.getElementById("ta-event-banner");
  if (bannerEl && !bannerEl.classList.contains("hidden") && performance.now() > taHudFx.bannerUntil) {
    bannerEl.classList.add("hidden");
  }
  renderTaKillFeed();

  taHudFx.prevPhase = phase;
  taHudFx.prevCaptured = captured;
  taHudFx.prevOvertime = Boolean(ta.overtime);
  taHudFx.prevAtkWaves = Number(ta.atkWaves ?? -1);
  taHudFx.prevDefWaves = Number(ta.defWaves ?? -1);
}

function renderTerritorialHud(st) {
  const hud = document.getElementById("ta-hud");
  if (!hud || !isTerritorialMode() || !st?.ta) return;

  const ta = st.ta;
  const isHost = state.role === "host";
  const phase = Number(ta.state || 0);
  const localTeam = Number(ta.localTeam || 0);
  const atkTeam = Number(ta.attackerTeam || 0);
  const isAttacker = localTeam === atkTeam;

  hud.dataset.role = phase === 2 || phase === 4 ? (isAttacker ? "atk" : "def") : "neutral";

  const phaseEl = document.getElementById("ta-phase-pill");
  if (phaseEl) phaseEl.textContent = TA_PHASE_LABELS[phase] || "TERRITORIAL";

  const roleEl = document.getElementById("ta-role-pill");
  if (roleEl) {
    const teamLabel = localTeam === 0 ? "TIME A" : "TIME B";
    const sideLabel = isAttacker ? "ATAQUE" : "DEFESA";
    roleEl.textContent = `${teamLabel} · ${sideLabel}`;
    roleEl.dataset.side = isAttacker ? "atk" : "def";
  }

  const clockEl = document.getElementById("ta-clock");
  if (clockEl) {
    if ((phase === 1 || phase === 3) && !ta.timeLeft) {
      clockEl.textContent = phase === 1 ? "PREP" : "INTERVALO";
    } else if (phase === 5) {
      clockEl.textContent = "FIM";
    } else {
      clockEl.textContent = ta.overtime ? `OT ${formatClock(ta.timeLeft)}` : formatClock(ta.timeLeft);
    }
    clockEl.classList.toggle("overtime", Boolean(ta.overtime));
  }

  const atkWavesEl = document.getElementById("ta-atk-waves");
  const defWavesEl = document.getElementById("ta-def-waves");
  if (atkWavesEl) atkWavesEl.textContent = String(ta.atkWaves ?? 0);
  if (defWavesEl) defWavesEl.textContent = String(ta.defWaves ?? 0);

  const objsEl = document.getElementById("ta-objs");
  if (objsEl) {
    const count = Math.max(0, Math.min(8, Number(ta.objCount || 0)));
    const curIdx = Number(ta.currentObj || 0);
    const capCount = Number(ta.captured || 0);
    const objState = Number(ta.objState || 0);
    const letters = "ABCDEFGH";
    let html = "";
    for (let i = 0; i < count; i++) {
      let cls = "ta-obj-chip";
      if (i < capCount) cls += " captured";
      else if (i === curIdx && (phase === 2 || phase === 4)) {
        cls += objState === 2 ? " contested" : " active";
      }
      html += `<span class="${cls}">${letters[i] || i + 1}</span>`;
    }
    if (objsEl.dataset.key !== html) {
      objsEl.dataset.key = html;
      objsEl.innerHTML = html;
    }
  }

  const progWrap = document.getElementById("ta-progress-wrap");
  const beaconEl = document.getElementById("ta-beacon");
  const beaconArrow = document.getElementById("ta-beacon-arrow");
  const beaconDist = document.getElementById("ta-beacon-dist");
  const progFill = document.getElementById("ta-progress-fill");
  const progPct = document.getElementById("ta-progress-pct");
  const zoneCountsEl = document.getElementById("ta-zone-counts");
  const waveBannerEl = document.getElementById("ta-wave-banner");
  const inActiveRound = phase === 2 || phase === 4;
  if (progWrap) progWrap.classList.toggle("hidden", !inActiveRound);
  if (inActiveRound) {
    const pct = Math.max(0, Math.min(100, Math.round(Number(ta.progress || 0) / 10)));
    const objLetter = "ABCDEFGH"[Number(ta.currentObj || 0)] || "A";
    const contested = Number(ta.objState || 0) === 2;
    const inZone = Boolean(ta.inZone);
    if (beaconEl) beaconEl.classList.toggle("in-zone", inZone);
    if (beaconArrow) {
      if (inZone || !ta.hasBeacon) {
        beaconArrow.textContent = inZone ? "●" : "◆";
        beaconArrow.style.transform = "rotate(0deg)";
      } else {
        beaconArrow.textContent = "▲";
        beaconArrow.style.transform = `rotate(${Math.round(Number(ta.bearingDeg || 0))}deg)`;
      }
    }
    if (beaconDist) {
      if (inZone) {
        beaconDist.textContent = contested ? `ZONA ${objLetter} · CONTESTADA` : `NA ZONA ${objLetter}`;
      } else if (ta.hasBeacon) {
        const distM = Math.max(0, Math.round(Number(ta.distM || 0)));
        beaconDist.textContent = contested ? `OBJ ${objLetter} · ${distM}m · CONTEST.` : `OBJ ${objLetter} · ${distM}m`;
      } else {
        beaconDist.textContent = contested ? `OBJ ${objLetter} · CONTESTADO` : `OBJ ${objLetter}`;
      }
    }
    if (progFill) {
      progFill.style.width = `${pct}%`;
      progFill.classList.toggle("contested", contested);
    }
    if (progPct) progPct.textContent = `${pct}%`;
    if (zoneCountsEl) {
      const zAtk = Number(ta.zoneAtk || 0);
      const zDef = Number(ta.zoneDef || 0);
      const hasAny = zAtk > 0 || zDef > 0;
      zoneCountsEl.classList.toggle("hidden", !hasAny);
      if (hasAny) zoneCountsEl.textContent = `ZONA ${zAtk}A/${zDef}D`;
    }
  }

  if (waveBannerEl) {
    const localId = st.localSlot ?? state.localPlayerId;
    const localRow = Array.isArray(st.players) ? st.players.find((p) => p.id === localId) : null;
    const localDead = inActiveRound && localRow && !localRow.alive;
    waveBannerEl.classList.toggle("hidden", !localDead);
    if (localDead) {
      const myWaves = isAttacker ? Number(ta.atkWaves || 0) : Number(ta.defWaves || 0);
      const myEta = isAttacker ? Number(ta.atkWaveEta || 0) : Number(ta.defWaveEta || 0);
      if (myWaves <= 0) {
        waveBannerEl.textContent = "ABATIDO · SEM ONDAS DE REFORÇO RESTANTES";
      } else if (myEta > 0) {
        waveBannerEl.textContent = `ABATIDO · PRÓXIMA ONDA DE REFORÇO EM ${myEta}s (${myWaves} restantes)`;
      } else {
        waveBannerEl.textContent = `ABATIDO · PREPARANDO ONDA DE REFORÇO (${myWaves} restantes)`;
      }
    }
  }

  const actionsRow = document.getElementById("ta-actions-row");
  const actionsInfo = document.getElementById("ta-actions-info");
  const btnTeamA = document.getElementById("btn-ta-team-a");
  const btnTeamB = document.getElementById("btn-ta-team-b");
  const btnBuy = document.getElementById("btn-ta-buy");
  const btnPrep = document.getElementById("btn-ta-prep");
  const btnStart = document.getElementById("btn-ta-start-round");

  const showActions = !inActiveRound;
  if (actionsRow) actionsRow.classList.toggle("hidden", !showActions);
  if (showActions) {
    if (btnTeamA) {
      btnTeamA.classList.toggle("hidden", phase !== 0);
      btnTeamA.classList.toggle("active", localTeam === 0);
    }
    if (btnTeamB) {
      btnTeamB.classList.toggle("hidden", phase !== 0);
      btnTeamB.classList.toggle("active", localTeam === 1);
    }
    if (btnBuy) {
      btnBuy.classList.toggle("hidden", !(phase === 1 || phase === 3));
    }
    if (btnPrep) {
      btnPrep.classList.toggle("hidden", !(isHost && phase === 0));
    }
    if (btnStart) {
      btnStart.classList.toggle("hidden", !(isHost && (phase === 1 || phase === 3)));
      btnStart.textContent = phase === 3 ? "Iniciar Rodada 2" : "Iniciar Rodada 1";
    }
    if (actionsInfo) {
      if (phase === 0) {
        actionsInfo.textContent = isHost
          ? "Lobby: escolha times (Esc libera cursor) e clique em Iniciar Preparação."
          : "Lobby: escolha Time A ou Time B (Esc libera cursor). Aguardando Host.";
      } else if (phase === 1) {
        actionsInfo.textContent = "Preparação R1: monte seu kit de 15 SP em [B] (primária obrigatória).";
      } else if (phase === 3) {
        actionsInfo.textContent = "Intervalo: papéis invertidos! Ajuste seu kit de 15 SP em [B] para a Rodada 2.";
      } else if (phase === 5) {
        const winTxt = ta.winner === 0 ? "VITÓRIA TIME A" : ta.winner === 1 ? "VITÓRIA TIME B" : "EMPATE";
        actionsInfo.textContent = `${winTxt} · R1: ${ta.r1Cap} obj (${Math.round((ta.r1Prog || 0) / 10)}%, ${formatClock(ta.r1Time)}) · R2: ${ta.r2Cap} obj (${Math.round((ta.r2Prog || 0) / 10)}%, ${formatClock(ta.r2Time)})`;
      }
    }
  }

  updateTaDynamicEvents(st, ta);
  renderTaWaypoint(st, ta);
  renderTaRadar(st, ta);
}

function updateTelemetryUi() {
  const st = engine.status;
  const topPill = document.getElementById("top-role-pill");
  const peers = st?.peers ?? (state.transport?.peers?.size || 0);
  const mode = activeGameMode();
  const isTA = mode === 2;

  if (topPill) {
    if (state.role === "offline") {
      topPill.textContent = "CAMPANHA SOLO";
    } else if (state.role === "host") {
      const mTag = mode === 0 ? "" : ` · ${gameModeShortLabel(mode)}`;
      topPill.textContent = st?.inLevel
        ? `HOST${mTag} · ${state.roomCode} · ${levelLabel(st.arenaId)} · ${peers + 1} jog.`
        : `HOST${mTag} · ${state.roomCode} · ${peers + 1} jog.`;
    } else {
      const mTag = mode === 0 ? "" : ` · ${gameModeShortLabel(mode)}`;
      topPill.textContent = st?.clientJoined
        ? `ONLINE${mTag} · ${state.roomCode} · ${levelLabel(st.arenaId)}`
        : `ENTRANDO · ${state.roomCode}`;
    }
    topPill.dataset.mode = state.role;
  }

  const optMpStatus = document.getElementById("opt-mp-status");
  if (optMpStatus) {
    optMpStatus.textContent =
      state.role === "offline"
        ? "Modo atual: Campanha Solo (vanilla intacta)."
        : state.role === "host"
          ? `Modo atual: Host (Territorial Assault) · Sala ${state.roomCode} · Mapa: ${mapNameByIndex(state.lobby.mapIndex)} · ${peers} par(es) · 15 SP.`
          : st?.clientJoined || peers > 0
            ? `Modo atual: Conectado (Territorial Assault) · Sala ${state.roomCode} · Mapa: ${mapNameByIndex(state.lobby.mapIndex)} (${peers} par).`
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
    if (hudCr) {
      hudCr.textContent = isTA
        ? `KIT ${cur.kitCost} / ${cur.maxCredits} SP`
        : `$${cur.credits} / $${cur.maxCredits}`;
    }
    if (hudBw) {
      if (isTA) {
        hudBw.textContent = cur.buyTicksLeft > 0 ? "ARMARIA ABERTA" : "KIT TRAVADO";
      } else {
        hudBw.textContent =
          cur.buyTicksLeft === 0xffff ? "LOJA LIVRE" : cur.buyTicksLeft > 0 ? `COMPRA ${Math.ceil(cur.buyTicksLeft / 30)}s` : "COMPRA FECHADA";
      }
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
    renderTerritorialHud(st);
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
  const isTA = isTerritorialMode();
  const modeTag = document.getElementById("score-mode-tag");
  if (modeTag) modeTag.textContent = `${gameModeShortLabel(activeGameMode())} · WEBRTC P2P`;

  const banner = document.getElementById("ta-score-banner");
  if (banner) {
    if (isTA && st?.ta) {
      const ta = st.ta;
      const phaseName = TA_PHASE_LABELS[Number(ta.state || 0)] || "TERRITORIAL";
      const winTxt =
        ta.state === 5
          ? ta.winner === 0
            ? " · VITÓRIA TIME A"
            : ta.winner === 1
              ? " · VITÓRIA TIME B"
              : " · EMPATE"
          : "";
      banner.classList.remove("hidden");
      banner.dataset.ready = String(ta.state === 5);
      banner.innerHTML = `<span>${phaseName}${winTxt} · ATK ${ta.atkWaves} ondas · DEF ${ta.defWaves} ondas</span><span class="mono">R1: ${ta.r1Cap} obj (${formatClock(ta.r1Time)}) | R2: ${ta.r2Cap} obj (${formatClock(ta.r2Time)})</span>`;
    } else {
      banner.classList.add("hidden");
    }
  }

  const localId = st?.localSlot ?? state.localPlayerId;
  const rows = Array.isArray(st?.players)
    ? [...st.players].sort((a, b) => {
        if (isTA && (a.team ?? 0) !== (b.team ?? 0)) return (a.team ?? 0) - (b.team ?? 0);
        return b.kills - a.kills || a.id - b.id;
      })
    : [];
  tbody.innerHTML = "";
  for (const op of rows) {
    const loadoutTxt =
      op.primary !== undefined && op.primary !== NO_WEAPON
        ? `${weaponNameById(op.primary)} + ${weaponNameById(op.secondary)}`
        : weaponNameById(op.secondary ?? op.weapon ?? 0);
    const teamTag = isTA
      ? `<span class="team-tag ${(op.team ?? 0) === 0 ? "team-a" : "team-b"}">${(op.team ?? 0) === 0 ? "TIME A" : "TIME B"}</span>`
      : "";
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td class="mono">#${op.id}</td>
      <td>${teamTag}${op.name}${op.id === localId ? " (VOCÊ)" : ""}</td>
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
    if (engine.audioCtx.state === "suspended") engine.audioCtx.resume().catch(() => {});
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
  // Não aguardar: resume() só resolve após um gesto do usuário na aba e não pode bloquear o boot do motor.
  if (ctx.state === "suspended") ctx.resume().catch(() => {});
  engine.audioCtx = ctx;
  engine.audioNode = node;
  return true;
}

async function startWasmEngine(targetMode) {
  touchControls?.enterPlayMode();
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
    if (isMultiplayerActive() && mod._BlackWebConfigurePreMatch) {
      mod._BlackWebConfigurePreMatch(
        state.lobby.mapIndex & 7,
        state.lobby.localTeam & 1,
        state.lobby.primary & 0xff,
        state.lobby.secondary & 0xff,
        state.lobby.tactical & 0xff
      );
    }

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
    const qs = new URLSearchParams(window.location.search);
    const mapParam = qs.get("map");
    if (mapParam !== null && mapParam !== "") {
      const parsed = Number(mapParam);
      if (Number.isFinite(parsed) && parsed >= 0 && parsed <= 7) {
        state.lobby.mapIndex = parsed | 0;
        state.roomRules.mapIndex = state.lobby.mapIndex;
      }
    }
    const asHost = qs.has("host");
    const param = qs.get("room") || qs.get("host");
    if (!param) {
      syncRoomFieldsToUi();
      return;
    }
    const clean = param.trim().toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 6);
    if (clean.length !== 6) {
      switchLauncherTab("mp");
      showToast(`Código "${param}" inválido: use exatamente 6 letras ou números (ex.: ABC123).`, 5000);
      return;
    }
    state.roomCode = clean;
    syncRoomFieldsToUi();
    switchLauncherTab("mp");
    showToast(
      asHost
        ? `Sala ${clean} (${mapNameByIndex(state.lobby.mapIndex)}) preenchida. Clique em "Criar Sala & Abrir Lobby"!`
        : `Convite para sala ${clean} (${mapNameByIndex(state.lobby.mapIndex)}). Clique em "Entrar na Sala"!`,
      3800,
    );
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
  const defaultClosedOverlay = () => (engine.bootStarted ? "none" : isMultiplayerActive() ? "lobby" : "launcher");
  document.getElementById("btn-open-options")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setOverlay(engine.activeOverlay === "options" ? defaultClosedOverlay() : "options");
  });
  document.getElementById("btn-close-options")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    setOverlay(defaultClosedOverlay());
    releaseInputFocus(true);
  });

  // Assist bar botões (em menus do jogo !inLevel)
  // "Pular Intro / Start":
  //  - clique: pula só a cutscene em reprodução (modo 2 do motor, consumido pelo primeiro filme). Se nenhum
  //    filme estiver tocando (ex.: PRESS START), aperta Start. Dificuldade e missão ficam por conta do jogador.
  //  - Shift+clique: pula TODAS (modo 1 do motor) e alterna, em ciclo de 3 s, (1) apertos curtos de Enter com soltura
  //    entre eles (a tela PRESS START só reconhece um aperto novo) e (2) Enter segurado (vídeos/créditos). Não há como
  //    saber qual tela está ativa, então os dois modos se revezam. Para ao entrar na fase, após 4 min,
  //    em novo clique ou ao pressionar qualquer tecla.
  let skipBurstTimer = null;
  const stopSkipBurst = () => {
    if (skipBurstTimer !== null) {
      clearInterval(skipBurstTimer);
      skipBurstTimer = null;
      engine.module?._BlackWebSetSkipCutscenes?.(0);
      window.removeEventListener("keydown", stopSkipBurst, true);
    }
  };
  document.getElementById("btn-assist-start")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (skipBurstTimer !== null) {
      stopSkipBurst();
      return;
    }
    if (!e.shiftKey) {
      // Arma o pulo de UMA cutscene. Se o motor consumiu (modo voltou a 0), havia filme tocando: só isso.
      // Senão não havia filme (ex.: PRESS START): desarma e aperta Start.
      const mod = engine.module;
      mod?._BlackWebSetSkipCutscenes?.(2);
      setTimeout(() => {
        if (mod?._BlackWebGetSkipCutscenes?.() === 2) {
          mod._BlackWebSetSkipCutscenes(0);
          pulseKey("Enter", 150);
        }
      }, 400);
      return;
    }
    const startedAt = performance.now();
    // Motor: encerra toda cutscene XMV em reprodução (hook sub_000C4570); o Enter cuida do PRESS START/menus.
    engine.module?._BlackWebSetSkipCutscenes?.(1);
    skipBurstTimer = setInterval(() => {
      const t = performance.now() - startedAt;
      if (isInLevelNow() || t > 240000) {
        stopSkipBurst();
        return;
      }
      const phase = t % 3000;
      if (phase < 1500) {
        // 70 ms apertado a cada 140 ms (soltura de 70 ms)
        if (Math.floor(phase / 70) % 2 === 0) pulseKey("Enter", 70);
      } else {
        pulseKey("Enter", 200);
      }
    }, 35);
    window.addEventListener("keydown", stopSkipBurst, true);
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

  // Multiplayer Sala, Pré-Lobby & Convite
  document.getElementById("mp-map-select")?.addEventListener("change", () => {
    readRoomConfigFromUi();
    syncRoomFieldsToUi();
  });
  document.getElementById("lobby-map-select")?.addEventListener("change", (e) => {
    if (state.role !== "host") return;
    const val = Math.max(0, Math.min(7, Number(e.target?.value || 0) | 0));
    state.lobby.mapIndex = val;
    state.roomRules.mapIndex = val;
    syncRoomFieldsToUi();
    broadcastHostLobbyState();
  });
  document.getElementById("btn-lobby-join-a")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    state.lobby.localTeam = 0;
    syncLocalLobbyChange();
  });
  document.getElementById("btn-lobby-join-b")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    state.lobby.localTeam = 1;
    syncLocalLobbyChange();
  });
  document.getElementById("btn-lobby-ready")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    state.lobby.ready = !state.lobby.ready;
    syncLocalLobbyChange();
  });
  document.getElementById("btn-lobby-start")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    launchMultiplayerMatch();
  });
  document.getElementById("btn-lobby-copy-link")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    copyInviteLink(false);
  });
  document.getElementById("btn-lobby-leave")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    startSession("offline");
    setOverlay("launcher");
    switchLauncherTab("mp");
  });
  document.getElementById("btn-results-rematch")?.addEventListener("click", () => {
    const url = new URL(window.location.href);
    url.searchParams.set("room", state.roomCode);
    url.searchParams.set("map", String(((state.lobby.mapIndex ?? 0) + 1) % BLACK_MAPS.length));
    window.location.href = url.toString();
  });
  document.getElementById("btn-results-menu")?.addEventListener("click", () => {
    const url = new URL(window.location.href);
    url.search = "";
    window.location.href = url.toString();
  });
  document.getElementById("btn-ta-team-a")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (engine.module?._BlackWebTerritorialAction?.(0, 0)) {
      showToast("Você entrou no Time A.");
      pollWasmStatus();
    }
  });
  document.getElementById("btn-ta-team-b")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (engine.module?._BlackWebTerritorialAction?.(0, 1)) {
      showToast("Você entrou no Time B.");
      pollWasmStatus();
    }
  });
  document.getElementById("btn-ta-buy")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    toggleBuyOverlay();
  });
  document.getElementById("btn-ta-prep")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (engine.module?._BlackWebTerritorialAction?.(1, 0)) {
      showToast("Preparação iniciada! Todos podem montar o kit de 15 SP em [B].");
      pollWasmStatus();
    } else {
      showToast("Necessário pelo menos 1 jogador em cada time (Time A e Time B) para iniciar.", 3600);
    }
  });
  document.getElementById("btn-ta-start-round")?.addEventListener("click", (e) => {
    e.currentTarget?.blur?.();
    if (engine.module?._BlackWebTerritorialAction?.(2, 0)) {
      showToast("Rodada Territorial Assault iniciada!");
      pollWasmStatus();
    } else {
      showToast("Não foi possível iniciar a rodada agora.", 3000);
    }
  });
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
      setOverlay(engine.activeOverlay === "options" ? defaultClosedOverlay() : "options");
      return;
    }

    if (e.code === "Escape") {
      if (engine.activeOverlay === "buy" || engine.activeOverlay === "options") {
        e.preventDefault();
        setOverlay(defaultClosedOverlay());
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

let touchControls = null;

window.addEventListener("DOMContentLoaded", async () => {
  touchControls = createTouchControls({
    viewport: document.getElementById("viewport-wrap"),
    engine,
    pushMouseButtons: () => engine.module?._BlackWebPushMouseState(engine.mouseButtons, 0, 0),
  });
  ensureVfsBridge();
  syncEnhancedControlsUi();
  bindEvents();
  applyRoomFromUrl();
  await detectBrowserCapabilities();
  await initAssetPipeline();
  startSession("offline");
  applyRoomFromUrl();
  setOverlay("launcher");
});
