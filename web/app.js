import { stageFilesToOpfs, checkOpfsCached, streamAssetsFromRemote } from "./opfs_loader.js";
import {
  MsgType,
  WebRtcTransport,
  decodePacket,
  encodeFireEvent,
  encodeHello,
  encodePing,
  encodePlayerState,
  encodePong,
  encodeWelcome,
  encodeWorldSnapshot,
} from "./netplay_webrtc.js";

const SPAWN_POINTS = [
  { x: -16, y: 0, z: -12, yaw: 0.65 },
  { x: 16, y: 0, z: -12, yaw: 2.45 },
  { x: -16, y: 0, z: 12, yaw: -0.65 },
  { x: 16, y: 0, z: 12, yaw: -2.45 },
];

const WEAPONS = [
  { id: 0, name: "M16A2", damage: 22, cooldownMs: 135, tracerColor: "#f59e0b" },
  { id: 1, name: "SPAS-12", damage: 48, cooldownMs: 520, tracerColor: "#fb923c" },
  { id: 2, name: "AWP .338", damage: 85, cooldownMs: 950, tracerColor: "#38bdf8" },
];

const state = {
  role: "offline", // "offline" | "host" | "client"
  roomCode: "VEX401",
  playerName: `OP-${Math.floor(10 + Math.random() * 89)}`,
  localPlayerId: 0,
  tick: 0,
  sequence: 0,
  timeLeftS: 600,
  rttMs: 0,
  packetsTx: 0,
  packetsRx: 0,
  lastPacketHex: "AC B1 01 ...",
  transport: null,
  peerToSlot: new Map(),
  players: new Map(), // playerId -> player object
  tracers: [],
  keys: new Set(),
  mouseAim: { x: 0, z: 1, worldX: 0, worldZ: 0 },
  selectedWeapon: 0,
  lastFireMs: 0,
  pingNonce: 1,
  webgpuReady: false,
  gpuAdapterName: "Verificando...",
};

function logLine(tag, text) {
  const logEl = document.getElementById("event-log");
  if (!logEl) return;
  const now = new Date().toLocaleTimeString("pt-BR", { hour12: false });
  const row = document.createElement("div");
  row.className = "log-entry";
  row.innerHTML = `<span class="log-time">${now}</span><span class="log-tag">[${tag}]</span><span class="log-msg">${text}</span>`;
  logEl.prepend(row);
  while (logEl.children.length > 60) {
    logEl.removeChild(logEl.lastChild);
  }
}

function toHexPreview(uint8) {
  const len = Math.min(14, uint8.byteLength);
  const parts = [];
  for (let i = 0; i < len; i++) {
    parts.push(uint8[i].toString(16).toUpperCase().padStart(2, "0"));
  }
  return parts.join(" ") + (uint8.byteLength > len ? ` (+${uint8.byteLength - len}B)` : "");
}

function makeOperator(playerId, name) {
  const sp = SPAWN_POINTS[playerId % SPAWN_POINTS.length];
  return {
    playerId,
    name: name || `OP-0${playerId + 1}`,
    x: sp.x,
    y: sp.y,
    z: sp.z,
    yaw: sp.yaw,
    pitch: 0,
    vx: 0,
    vy: 0,
    vz: 0,
    animState: 0,
    weapon: 0,
    health: 100,
    flags: 1, // bit0 = alive, bit1 = crouch
    kills: 0,
    deaths: 0,
    respawnAtTick: 0,
  };
}

async function detectBrowserCapabilities() {
  const sabOk = typeof SharedArrayBuffer !== "undefined" && window.crossOriginIsolated === true;
  const opfsOk = Boolean(navigator.storage && navigator.storage.getDirectory);
  const rtcOk = typeof RTCPeerConnection !== "undefined";

  let gpuOk = false;
  let gpuLabel = "Indisponível";
  if (navigator.gpu) {
    try {
      const adapter = await navigator.gpu.requestAdapter();
      if (adapter) {
        gpuOk = true;
        const info = adapter.info || {};
        gpuLabel = info.description || info.vendor || "WebGPU Adapter Ativo";
      }
    } catch {
      gpuOk = false;
    }
  }
  state.webgpuReady = gpuOk;
  state.gpuAdapterName = gpuLabel;

  setBadge("cap-webgpu", gpuOk ? "WEBGPU ATIVO" : "CANVAS 2D FALLBACK", gpuOk ? "ok" : "warn");
  setBadge("cap-sab", sabOk ? "COOP/COEP ISOLADO" : "SEM COOP/COEP", sabOk ? "ok" : "warn");
  setBadge("cap-opfs", opfsOk ? "OPFS PRONTO" : "OPFS INDISPONÍVEL", opfsOk ? "ok" : "warn");
  setBadge("cap-webrtc", rtcOk ? "WEBRTC DATACHANNEL" : "SEM WEBRTC", rtcOk ? "ok" : "warn");
}

function setBadge(id, text, level) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.dataset.level = level;
}

function startSession(preferredRole) {
  if (state.transport) {
    state.transport.close();
  }
  const codeInput = document.getElementById("room-code-input");
  const nameInput = document.getElementById("operator-name-input");
  const sigInput = document.getElementById("signaling-url-input");

  state.roomCode = (codeInput?.value || "VEX401").trim().toUpperCase().slice(0, 6) || "VEX401";
  state.playerName = (nameInput?.value || state.playerName).trim().slice(0, 16) || "OPERATOR";
  const signalingUrl = (sigInput?.value || "").trim();

  state.peerToSlot.clear();
  state.players.clear();
  state.tick = 0;
  state.sequence = 0;

  if (preferredRole === "host") {
    state.role = "host";
    state.localPlayerId = 0;
    state.players.set(0, makeOperator(0, state.playerName));
  } else {
    state.role = "client";
    state.localPlayerId = 1;
  }

  state.transport = new WebRtcTransport({
    roomCode: state.roomCode,
    signalingUrl,
    onLog: (msg) => logLine("WEBRTC", msg),
    onEvent: (ev) => handleTransportEvent(ev),
  });

  state.transport.start(preferredRole);
  updateTelemetryUi();
}

function handleTransportEvent(ev) {
  if (ev.type === "role") {
    state.role = ev.role;
    if (ev.role === "host" && !state.players.has(0)) {
      state.localPlayerId = 0;
      state.players.set(0, makeOperator(0, state.playerName));
    }
    updateTelemetryUi();
    return;
  }

  if (ev.type === "connected") {
    if (state.role === "client") {
      const pkt = encodeHello(state.playerName);
      state.transport.send(ev.peer, "reliable", pkt);
      recordTx(pkt);
      logLine("NETCORE", `Hello enviado (${pkt.byteLength}B, magic 0xB1AC).`);
    }
    return;
  }

  if (ev.type === "disconnected") {
    const slot = state.peerToSlot.get(ev.peer);
    if (slot !== undefined) {
      state.players.delete(slot);
      state.peerToSlot.delete(ev.peer);
      logLine("NETCORE", `Operador no slot #${slot} desconectado.`);
      updateScoreboard();
    }
    return;
  }

  if (ev.type === "packet") {
    recordRx(ev.payload);
    const decoded = decodePacket(ev.payload);
    if (!decoded) return;
    handleNetcorePacket(ev.peer, decoded);
  }
}

function handleNetcorePacket(peerId, pkt) {
  if (state.role === "host") {
    if (pkt.type === MsgType.Hello) {
      let assignedSlot = state.peerToSlot.get(peerId);
      if (assignedSlot === undefined) {
        for (let s = 1; s < 4; s++) {
          if (!state.players.has(s)) {
            assignedSlot = s;
            break;
          }
        }
      }
      if (assignedSlot === undefined) return;
      state.peerToSlot.set(peerId, assignedSlot);
      state.players.set(assignedSlot, makeOperator(assignedSlot, pkt.name));

      const welcome = encodeWelcome(assignedSlot, 4, 30, 15);
      state.transport.send(peerId, "reliable", welcome);
      recordTx(welcome);
      logLine("HOST", `Operador "${pkt.name}" autenticado no slot #${assignedSlot} (0x01C063EC + #${assignedSlot}).`);
      updateScoreboard();
      return;
    }

    if (pkt.type === MsgType.PlayerState) {
      const slot = state.peerToSlot.get(peerId);
      if (slot === undefined) return;
      const op = state.players.get(slot);
      if (!op || (op.flags & 1) === 0) return;
      op.x = Math.max(-28, Math.min(28, pkt.x));
      op.y = pkt.y;
      op.z = Math.max(-18, Math.min(18, pkt.z));
      op.yaw = pkt.yaw;
      op.pitch = pkt.pitch;
      op.vx = pkt.vx;
      op.vy = pkt.vy;
      op.vz = pkt.vz;
      op.animState = pkt.animState;
      op.weapon = pkt.weapon;
      op.flags = (op.flags & 1) | (pkt.flags & 2);
      return;
    }

    if (pkt.type === MsgType.FireEvent) {
      const slot = state.peerToSlot.get(peerId);
      if (slot === undefined) return;
      authoritativeResolveFire(slot, pkt);
      state.transport.broadcast("reliable", encodeFireEvent({ ...pkt, shooter: slot }));
      return;
    }

    if (pkt.type === MsgType.Ping) {
      const pong = encodePong(pkt.nonce, pkt.sentMs, state.tick);
      state.transport.send(peerId, "unreliable", pong);
      recordTx(pong);
    }
  } else if (state.role === "client") {
    if (pkt.type === MsgType.Welcome) {
      state.localPlayerId = pkt.playerId;
      state.players.set(pkt.playerId, makeOperator(pkt.playerId, state.playerName));
      logLine("CLIENT", `Welcome recebido: slot #${pkt.playerId}, tick ${pkt.tickRateHz}Hz, snapshot ${pkt.snapshotRateHz}Hz.`);
      updateTelemetryUi();
      return;
    }

    if (pkt.type === MsgType.WorldSnapshot) {
      state.tick = pkt.tick;
      state.timeLeftS = pkt.timeLeftS;
      for (const sp of pkt.players) {
        const existing = state.players.get(sp.playerId) || makeOperator(sp.playerId, `OP-0${sp.playerId + 1}`);
        if (sp.playerId === state.localPlayerId && (sp.flags & 1) !== 0 && (existing.flags & 1) !== 0) {
          // Mantém predição local suave e reconcilia se divergir muito
          const dist = Math.hypot(sp.x - existing.x, sp.z - existing.z);
          if (dist > 3.5) {
            existing.x = sp.x;
            existing.z = sp.z;
          }
        } else {
          existing.x = sp.x;
          existing.y = sp.y;
          existing.z = sp.z;
          existing.yaw = sp.yaw;
          existing.pitch = sp.pitch;
        }
        existing.health = sp.health;
        existing.flags = sp.flags;
        existing.weapon = sp.weapon;
        existing.kills = sp.kills;
        existing.deaths = sp.deaths;
        state.players.set(sp.playerId, existing);
      }
      updateScoreboard();
      return;
    }

    if (pkt.type === MsgType.FireEvent) {
      spawnVisualTracer(pkt.ox, pkt.oz, pkt.dx, pkt.dz, pkt.weapon);
      return;
    }

    if (pkt.type === MsgType.Pong) {
      const nowMs = Math.floor(performance.now()) >>> 0;
      state.rttMs = Math.max(0, nowMs - pkt.echoMs);
      updateTelemetryUi();
    }
  }
}

function authoritativeResolveFire(shooterId, ev) {
  const shooter = state.players.get(shooterId);
  if (!shooter || (shooter.flags & 1) === 0) return;

  const weaponSpec = WEAPONS[ev.weapon] || WEAPONS[0];
  spawnVisualTracer(ev.ox, ev.oz, ev.dx, ev.dz, ev.weapon);

  // Raycast 2D no plano XZ contra os demais jogadores vivos
  let bestTarget = null;
  let bestDist = 60.0;

  for (const [targetId, target] of state.players) {
    if (targetId === shooterId || (target.flags & 1) === 0) continue;
    const rx = target.x - ev.ox;
    const rz = target.z - ev.oz;
    const proj = rx * ev.dx + rz * ev.dz;
    if (proj <= 0 || proj > bestDist) continue;
    const perpSq = rx * rx + rz * rz - proj * proj;
    const hitRadius = (target.flags & 2) !== 0 ? 1.05 : 1.35;
    if (perpSq <= hitRadius * hitRadius) {
      bestDist = proj;
      bestTarget = target;
    }
  }

  if (bestTarget) {
    bestTarget.health = Math.max(0, bestTarget.health - weaponSpec.damage);
    if (bestTarget.health === 0) {
      bestTarget.flags &= ~1;
      bestTarget.deaths += 1;
      bestTarget.respawnAtTick = state.tick + 90; // 3s a 30Hz
      shooter.kills += 1;
      logLine("KILL", `${shooter.name} eliminou ${bestTarget.name} com ${weaponSpec.name} (${ shooter.kills }/15).`);
    }
    updateScoreboard();
  }
}

function spawnVisualTracer(ox, oz, dx, dz, weaponId) {
  const spec = WEAPONS[weaponId] || WEAPONS[0];
  state.tracers.push({
    x0: ox,
    z0: oz,
    x1: ox + dx * 34,
    z1: oz + dz * 34,
    color: spec.tracerColor,
    alpha: 1.0,
  });
}

function recordTx(uint8) {
  state.packetsTx += 1;
  state.lastPacketHex = `TX ${toHexPreview(uint8)}`;
}

function recordRx(uint8) {
  state.packetsRx += 1;
  state.lastPacketHex = `RX ${toHexPreview(uint8)}`;
}

function stepSimulationTick() {
  state.tick += 1;
  const local = state.players.get(state.localPlayerId);

  if (local && (local.flags & 1) !== 0) {
    const crouch = state.keys.has("ShiftLeft") || state.keys.has("ShiftRight") || state.keys.has("KeyC");
    const speed = crouch ? 0.24 : 0.42;
    let mx = 0;
    let mz = 0;
    if (state.keys.has("KeyW") || state.keys.has("ArrowUp")) mz -= 1;
    if (state.keys.has("KeyS") || state.keys.has("ArrowDown")) mz += 1;
    if (state.keys.has("KeyA") || state.keys.has("ArrowLeft")) mx -= 1;
    if (state.keys.has("KeyD") || state.keys.has("ArrowRight")) mx += 1;

    const len = Math.hypot(mx, mz);
    if (len > 0) {
      mx = (mx / len) * speed;
      mz = (mz / len) * speed;
    }

    local.x = Math.max(-28, Math.min(28, local.x + mx));
    local.z = Math.max(-18, Math.min(18, local.z + mz));
    local.vx = mx * 30;
    local.vz = mz * 30;
    local.yaw = Math.atan2(state.mouseAim.worldZ - local.z, state.mouseAim.worldX - local.x);
    local.weapon = state.selectedWeapon;
    local.animState = len > 0 ? (crouch ? 2 : 1) : 0;
    local.flags = 1 | (crouch ? 2 : 0);

    if (state.role === "client" && state.transport) {
      state.sequence += 1;
      const pkt = encodePlayerState({
        sequence: state.sequence,
        clientTick: state.tick,
        x: local.x,
        y: local.y,
        z: local.z,
        yaw: local.yaw,
        pitch: 0,
        vx: local.vx,
        vy: 0,
        vz: local.vz,
        animState: local.animState,
        weapon: local.weapon,
        flags: local.flags,
      });
      state.transport.broadcast("unreliable", pkt);
      recordTx(pkt);
    }
  }

  if (state.role === "host") {
    for (const [pid, op] of state.players) {
      if ((op.flags & 1) === 0 && state.tick >= op.respawnAtTick) {
        const sp = SPAWN_POINTS[(pid + state.tick) % SPAWN_POINTS.length];
        op.x = sp.x;
        op.z = sp.z;
        op.health = 100;
        op.flags = 1;
        logLine("RESPAWN", `${op.name} reposicionado em (${sp.x.toFixed(0)}, ${sp.z.toFixed(0)}) via RestoreAndTeleport.`);
      }
    }

    // Prevenção de colisão no mesmo ponto (espelha PreventSamePointCollisionCrash em mp_bridge.cpp)
    const list = Array.from(state.players.values());
    for (let i = 0; i < list.length; i++) {
      for (let j = i + 1; j < list.length; j++) {
        const a = list[i];
        const b = list[j];
        if ((a.flags & 1) === 0 || (b.flags & 1) === 0) continue;
        const dx = b.x - a.x;
        const dz = b.z - a.z;
        const d = Math.hypot(dx, dz);
        if (d < 1.2) {
          const push = (1.2 - d) * 0.5;
          const nx = d > 0.001 ? dx / d : 1;
          const nz = d > 0.001 ? dz / d : 0;
          a.x -= nx * push;
          a.z -= nz * push;
          b.x += nx * push;
          b.z += nz * push;
        }
      }
    }

    // Emite WorldSnapshot a 15 Hz (a cada 2 ticks de 30 Hz)
    if (state.tick % 2 === 0 && state.transport && state.transport.peers.size > 0) {
      const snap = encodeWorldSnapshot(state.tick, state.timeLeftS, 1, list);
      state.transport.broadcast("unreliable", snap);
      recordTx(snap);
    }
  }

  if (state.role === "client" && state.tick % 30 === 0 && state.transport) {
    const ping = encodePing(state.pingNonce++, Math.floor(performance.now()) >>> 0);
    state.transport.broadcast("unreliable", ping);
    recordTx(ping);
  }

  if (state.tick % 10 === 0) {
    updateTelemetryUi();
    updateScoreboard();
  }
}

function triggerFire() {
  const local = state.players.get(state.localPlayerId);
  if (!local || (local.flags & 1) === 0) return;

  const spec = WEAPONS[state.selectedWeapon] || WEAPONS[0];
  const now = performance.now();
  if (now - state.lastFireMs < spec.cooldownMs) return;
  state.lastFireMs = now;

  const dx = Math.cos(local.yaw);
  const dz = Math.sin(local.yaw);
  const ev = {
    clientTick: state.tick,
    shooter: state.localPlayerId,
    weapon: state.selectedWeapon,
    ox: local.x,
    oy: 1.5,
    oz: local.z,
    dx,
    dy: 0,
    dz,
    claimedTarget: 0xff,
  };

  if (state.role === "host" || state.role === "offline") {
    authoritativeResolveFire(state.localPlayerId, ev);
    if (state.transport) {
      const pkt = encodeFireEvent(ev);
      state.transport.broadcast("reliable", pkt);
      recordTx(pkt);
    }
  } else if (state.role === "client" && state.transport) {
    spawnVisualTracer(ev.ox, ev.oz, ev.dx, ev.dz, ev.weapon);
    const pkt = encodeFireEvent(ev);
    state.transport.broadcast("reliable", pkt);
    recordTx(pkt);
  }
}

function renderArena() {
  const canvas = document.getElementById("tactical-canvas");
  if (!canvas) return;

  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;

  ctx.fillStyle = "#0b0c0e";
  ctx.fillRect(0, 0, w, h);

  const scaleX = w / 64;
  const scaleZ = h / 40;
  const toScreenX = (wx) => w * 0.5 + wx * scaleX;
  const toScreenY = (wz) => h * 0.5 + wz * scaleZ;

  // Grade tática do mapa (Treneska Border Crossing)
  ctx.strokeStyle = "rgba(255, 255, 255, 0.045)";
  ctx.lineWidth = 1;
  for (let gx = -28; gx <= 28; gx += 4) {
    const sx = toScreenX(gx);
    ctx.beginPath();
    ctx.moveTo(sx, 0);
    ctx.lineTo(sx, h);
    ctx.stroke();
  }
  for (let gz = -18; gz <= 18; gz += 4) {
    const sy = toScreenY(gz);
    ctx.beginPath();
    ctx.moveTo(0, sy);
    ctx.lineTo(w, sy);
    ctx.stroke();
  }

  // Obstáculos / bunkers centrais da arena
  const bunkers = [
    { x: -10, z: -5, w: 5, h: 2.5 },
    { x: 5, z: 2.5, w: 5, h: 2.5 },
    { x: -2.5, z: -1.5, w: 5, h: 3 },
  ];
  ctx.fillStyle = "#161920";
  ctx.strokeStyle = "rgba(245, 158, 11, 0.28)";
  for (const b of bunkers) {
    const bx = toScreenX(b.x);
    const by = toScreenY(b.z);
    const bw = b.w * scaleX;
    const bh = b.h * scaleZ;
    ctx.fillRect(bx, by, bw, bh);
    ctx.strokeRect(bx, by, bw, bh);
  }

  // Tracers balísticos
  for (let i = state.tracers.length - 1; i >= 0; i--) {
    const t = state.tracers[i];
    ctx.strokeStyle = t.color;
    ctx.globalAlpha = t.alpha;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(toScreenX(t.x0), toScreenY(t.z0));
    ctx.lineTo(toScreenX(t.x1), toScreenY(t.z1));
    ctx.stroke();
    t.alpha -= 0.08;
    if (t.alpha <= 0) state.tracers.splice(i, 1);
  }
  ctx.globalAlpha = 1.0;

  // Operadores (slots 0..3 mapeados em kPool + 0x79B0)
  const palette = ["#f59e0b", "#38bdf8", "#34d399", "#f87171"];
  for (const [pid, op] of state.players) {
    const sx = toScreenX(op.x);
    const sy = toScreenY(op.z);
    const alive = (op.flags & 1) !== 0;
    const crouch = (op.flags & 2) !== 0;
    const color = palette[pid % palette.length];

    if (!alive) {
      ctx.strokeStyle = "rgba(248, 113, 113, 0.45)";
      ctx.beginPath();
      ctx.moveTo(sx - 7, sy - 7);
      ctx.lineTo(sx + 7, sy + 7);
      ctx.moveTo(sx + 7, sy - 7);
      ctx.lineTo(sx - 7, sy + 7);
      ctx.stroke();
      continue;
    }

    // Cone de visão (0x1AF880 SetControllerAngles)
    ctx.fillStyle = `${color}18`;
    ctx.beginPath();
    ctx.moveTo(sx, sy);
    ctx.arc(sx, sy, 46, op.yaw - 0.32, op.yaw + 0.32);
    ctx.closePath();
    ctx.fill();

    // Corpo do operador
    const radius = crouch ? 7.5 : 10;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(sx, sy, radius, 0, Math.PI * 2);
    ctx.fill();

    // Barra de HP (+0x2F0 float health)
    ctx.fillStyle = "rgba(0,0,0,0.75)";
    ctx.fillRect(sx - 16, sy - 18, 32, 4);
    ctx.fillStyle = op.health > 35 ? color : "#ef4444";
    ctx.fillRect(sx - 16, sy - 18, (32 * Math.max(0, op.health)) / 100, 4);

    // Identificador tático
    ctx.fillStyle = "#e5e7eb";
    ctx.font = "600 11px 'JetBrains Mono', monospace";
    ctx.fillText(`${op.name} [#${pid}]`, sx - 24, sy + 23);
  }

  requestAnimationFrame(renderArena);
}

function updateTelemetryUi() {
  const roleEl = document.getElementById("tel-role");
  const slotEl = document.getElementById("tel-slot");
  const tickEl = document.getElementById("tel-tick");
  const rttEl = document.getElementById("tel-rtt");
  const pktEl = document.getElementById("tel-packets");
  const hexEl = document.getElementById("tel-hex");

  if (roleEl) roleEl.textContent = state.role.toUpperCase();
  if (slotEl) slotEl.textContent = `#${state.localPlayerId} (0x01C063EC+${state.localPlayerId * 4})`;
  if (tickEl) tickEl.textContent = `${state.tick} (30 Hz)`;
  if (rttEl) rttEl.textContent = state.role === "host" ? "0 ms (Autoritativo)" : `${state.rttMs} ms`;
  if (pktEl) pktEl.textContent = `TX ${state.packetsTx} / RX ${state.packetsRx}`;
  if (hexEl) hexEl.textContent = state.lastPacketHex;
}

function updateScoreboard() {
  const tbody = document.getElementById("scoreboard-body");
  if (!tbody) return;
  tbody.innerHTML = "";

  const sorted = Array.from(state.players.values()).sort((a, b) => b.kills - a.kills || a.playerId - b.playerId);
  for (const op of sorted) {
    const weaponName = (WEAPONS[op.weapon] || WEAPONS[0]).name;
    const alive = (op.flags & 1) !== 0;
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td class="mono">#${op.playerId}</td>
      <td>${op.name}${op.playerId === state.localPlayerId ? " (VOCÊ)" : ""}</td>
      <td class="mono">${alive ? `${op.health} HP` : "MORTO"}</td>
      <td class="mono">${weaponName}</td>
      <td class="mono">${op.kills}</td>
      <td class="mono">${op.deaths}</td>
    `;
    tbody.appendChild(tr);
  }
}

async function initAssetPipeline() {
  const statusEl = document.getElementById("opfs-status");
  const barEl = document.getElementById("asset-progress-bar");
  const playBtn = document.getElementById("btn-auto-play");

  // 1. Verifica se um default.xbe real (> 1 MB, Title ID 0x45410083) já está em cache no OPFS
  const cachedCheck = await checkOpfsCached().catch(() => ({ cached: false, sizeBytes: 0, reason: "" }));
  if (cachedCheck.cached) {
    if (barEl) barEl.style.width = "100%";
    if (statusEl) {
      const mb = (cachedCheck.sizeBytes / (1024 * 1024)).toFixed(2);
      statusEl.textContent = `Pronto! default.xbe real verificado no OPFS (${mb} MB · Title ID 0x45410083 · Entry 0x25A3F).`;
    }
    if (playBtn) {
      playBtn.textContent = "▶ ENTRAR NA ARENA P2P (OPFS REAL PRONTO)";
    }
    logLine("RECOMP", "OPFS contém default.xbe real de BLACK (Title ID 0x45410083 / XDK 5849).");
    return;
  }

  // 2. Verifica se o servidor local possui o manifesto dos arquivos reais extraídos
  let manifest = null;
  try {
    const res = await fetch("/api/manifest");
    if (res.ok) {
      manifest = await res.json();
      if (manifest.available && manifest.files && manifest.files.length > 0) {
        const totalMb = (manifest.total_bytes / (1024 * 1024)).toFixed(1);
        if (statusEl) {
          statusEl.textContent = `${manifest.files.length} arquivos reais de BLACK (${totalMb} MB) prontos no servidor (Title ID 0x45410083 / XDK 5849). Clique abaixo para sincronizar no OPFS.`;
        }
        if (playBtn) {
          playBtn.textContent = `⟳ SINCRONIZAR ARQUIVOS (${totalMb} MB) VIA OPFS`;
        }
        return;
      }
    }
  } catch {}

  // 3. Sem arquivos reais no servidor/OPFS ainda: informa especificações do XboxXBE-Recomp
  const recompInfo = manifest?.recompiler || {
    sdk: "TGP482/XboxXBERecomp",
    title_id: "0x45410083",
    entry: "0x25A3F",
    xdk: 5849,
    partitions: 64,
  };
  if (statusEl) {
    if (manifest?.has_iso) {
      statusEl.textContent = `ISO detectada em assets/, sem default.xbe extraído ainda. Execute tools/xiso_extract.py ou reinicie tools/serve_web.py.`;
    } else if (cachedCheck.reason && cachedCheck.reason.includes("sintético")) {
      statusEl.textContent = `${cachedCheck.reason} Aguardando cópia real (${recompInfo.sdk}: Title ${recompInfo.title_id}, Entry ${recompInfo.entry}, XDK ${recompInfo.xdk}, ${recompInfo.partitions} partições).`;
    } else {
      statusEl.textContent = `Sem cópia real em assets/ (${recompInfo.sdk}: Title ${recompInfo.title_id} · Entry ${recompInfo.entry} · XDK ${recompInfo.xdk} · ${recompInfo.partitions} partições). Coloque default.xbe / .iso em assets/ ou selecione a pasta abaixo.`;
    }
  }
  if (playBtn) {
    playBtn.textContent = "▶ ENTRAR NA ARENA P2P (TESTE LIBS/NETCORE)";
  }
}

async function triggerAutoStream() {
  const statusEl = document.getElementById("opfs-status");
  const barEl = document.getElementById("asset-progress-bar");
  const playBtn = document.getElementById("btn-auto-play");

  const cachedCheck = await checkOpfsCached().catch(() => ({ cached: false }));
  if (cachedCheck.cached) {
    logLine("OPFS", "Iniciando arena P2P com assets reais de BLACK validados no OPFS.");
    document.getElementById("tactical-canvas")?.scrollIntoView({ behavior: "smooth" });
    return;
  }

  if (playBtn) {
    playBtn.disabled = true;
    playBtn.textContent = "SINCRONIZANDO DADOS REAIS DE BLACK...";
  }

  try {
    const result = await streamAssetsFromRemote("", ({ count, totalFiles, downloadedBytes, totalBytes, percent, currentFile }) => {
      const dlMb = (downloadedBytes / (1024 * 1024)).toFixed(1);
      const totMb = (totalBytes / (1024 * 1024)).toFixed(1);
      if (barEl) barEl.style.width = `${Math.min(100, percent).toFixed(1)}%`;
      if (statusEl) {
        statusEl.textContent = `[${count}/${totalFiles}] ${currentFile} (${dlMb}/${totMb} MB - ${percent.toFixed(0)}%)`;
      }
    });

    if (result.success) {
      if (barEl) barEl.style.width = "100%";
      if (statusEl) {
        statusEl.textContent = `Sincronização concluída! ${result.stagedFiles} arquivos salvos no OPFS (${result.xbeReport?.reason || "OK"}).`;
      }
      if (playBtn) {
        playBtn.disabled = false;
        playBtn.textContent = "▶ ENTRAR NA ARENA P2P (OPFS REAL PRONTO)";
      }
      logLine("OPFS", `Auto-stream concluído com sucesso: ${result.stagedFiles} arquivos gravados no OPFS.`);
    } else {
      if (statusEl) {
        statusEl.textContent = `${result.reason} Coloque sua cópia NTSC-U/ISO de BLACK em assets/ ou use a seleção manual.`;
      }
      if (playBtn) {
        playBtn.disabled = false;
        playBtn.textContent = "▶ ENTRAR NA ARENA P2P (TESTE LIBS/NETCORE)";
      }
    }
  } catch (err) {
    if (statusEl) statusEl.textContent = `Erro no download: ${err.message}`;
    if (playBtn) {
      playBtn.disabled = false;
      playBtn.textContent = "▶ ENTRAR NA ARENA P2P (TESTE LIBS/NETCORE)";
    }
  }
}

function bindEvents() {
  document.getElementById("btn-auto-play")?.addEventListener("click", triggerAutoStream);
  document.getElementById("btn-host-room")?.addEventListener("click", () => startSession("host"));
  document.getElementById("btn-join-room")?.addEventListener("click", () => startSession("client"));

  const fileInput = document.getElementById("xbe-file-input");
  document.getElementById("btn-pick-files")?.addEventListener("click", () => fileInput?.click());

  fileInput?.addEventListener("change", async (e) => {
    const files = e.target.files;
    if (!files || files.length === 0) return;
    const statusEl = document.getElementById("opfs-status");
    const barEl = document.getElementById("asset-progress-bar");
    if (statusEl) statusEl.textContent = `Processando ${files.length} arquivo(s) para o OPFS...`;

    const res = await stageFilesToOpfs(files, ({ count, totalFiles, currentFile }) => {
      if (barEl) barEl.style.width = `${((count / totalFiles) * 100).toFixed(1)}%`;
      if (statusEl) statusEl.textContent = `[${count}/${totalFiles}] ${currentFile}`;
    });

    const mb = (res.totalBytes / (1024 * 1024)).toFixed(2);
    if (res.xbeReport) {
      const r = res.xbeReport;
      const hashShort = r.sha256.slice(0, 20) + "...";
      if (statusEl) {
        statusEl.textContent = `${r.reason} | ${res.stagedFiles} arq (${mb} MB) | SHA-256: ${hashShort}`;
      }
      logLine("OPFS", `${r.reason} Base=0x${r.baseAddr.toString(16).toUpperCase()} SHA256=${hashShort}`);
      await initAssetPipeline();
    } else if (statusEl) {
      statusEl.textContent = `${res.stagedFiles} arquivo(s) (${mb} MB) copiados ao OPFS local. (Inclua default.xbe real para validar Title ID 0x45410083).`;
    }
  });

  window.addEventListener("keydown", (e) => {
    if (["INPUT", "TEXTAREA"].includes(document.activeElement?.tagName)) return;
    state.keys.add(e.code);
    if (e.code === "Digit1") state.selectedWeapon = 0;
    if (e.code === "Digit2") state.selectedWeapon = 1;
    if (e.code === "Digit3") state.selectedWeapon = 2;
  });

  window.addEventListener("keyup", (e) => {
    state.keys.delete(e.code);
  });

  window.addEventListener("blur", () => {
    state.keys.clear();
  });

  const canvas = document.getElementById("tactical-canvas");
  canvas?.addEventListener("mousemove", (e) => {
    const rect = canvas.getBoundingClientRect();
    const nx = (e.clientX - rect.left) / rect.width;
    const nz = (e.clientY - rect.top) / rect.height;
    state.mouseAim.worldX = (nx - 0.5) * 64;
    state.mouseAim.worldZ = (nz - 0.5) * 40;
  });

  canvas?.addEventListener("mousedown", (e) => {
    if (e.button === 0) triggerFire();
  });
}

window.addEventListener("DOMContentLoaded", async () => {
  bindEvents();
  await detectBrowserCapabilities();
  await initAssetPipeline();

  // Inicia com operador local em modo Host pronto para receber outras abas na mesma sala P2P
  startSession("host");
  setInterval(stepSimulationTick, 1000 / 30);
  requestAnimationFrame(renderArena);
});
