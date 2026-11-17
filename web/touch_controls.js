// Controles de toque (mobile) para o BLACK no navegador.
//
// Traduz gestos em entradas que o motor já entende, sem mexer no WASM:
//  - botões   -> KeyboardEvent sintético na window (reaproveita latch, B/Tab/F10 e o mapa de teclas);
//  - mira     -> engine.pendingDx/Dy (mesmo caminho do mouse, vira mouse_direct ou analógico direito);
//  - tiro/ADS -> engine.mouseButtons (bits 1 e 2);
//  - analógico esquerdo -> engine.touchPad (usado por pollBrowserGamepad em app.js).

const LOOK_SCALE = 2.2; // px de toque -> "px de mouse"; telas de toque precisam de mais ganho
const STICK_RADIUS = 56; // px do raio máximo do analógico virtual
const STICK_DEADZONE = 0.12;

export function detectTouchPreferred() {
  try {
    const q = new URLSearchParams(window.location.search).get("touch");
    if (q === "1") return true;
    if (q === "0") return false;
  } catch {}
  const coarse = window.matchMedia?.("(pointer: coarse)")?.matches;
  const noHover = window.matchMedia?.("(hover: none)")?.matches;
  return Boolean(coarse && noHover) || (navigator.maxTouchPoints > 0 && !window.matchMedia?.("(pointer: fine)")?.matches);
}

function el(tag, className, attrs = {}) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "text") node.textContent = v;
    else node.setAttribute(k, v);
  }
  return node;
}

export function createTouchControls({ viewport, engine, pushMouseButtons }) {
  const enabled = detectTouchPreferred();
  const api = {
    enabled,
    update() {},
    enterPlayMode() {},
  };
  if (!enabled || !viewport) return api;

  engine.touchPad = { active: false, lx: 0, ly: 0 };

  const root = el("div", "tc-root tc-hidden", { id: "touch-controls" });
  const menuLayer = el("div", "tc-layer tc-menu");
  const combatLayer = el("div", "tc-layer tc-combat");
  const mpLayer = el("div", "tc-layer tc-mp");
  root.append(menuLayer, combatLayer, mpLayer);
  viewport.appendChild(root);

  // ---------- helpers de tecla ----------
  const sendKey = (type, code) => {
    window.dispatchEvent(new KeyboardEvent(type, { code, key: code, bubbles: true, cancelable: true }));
  };
  const tapKey = (code, holdMs = 90) => {
    sendKey("keydown", code);
    setTimeout(() => sendKey("keyup", code), holdMs);
  };

  // Registra um botão de toque. mode: "hold" (segura enquanto toca) | "tap" | custom {down, up}.
  const makeButton = (parent, label, className, handlers, title = "") => {
    const b = el("button", `tc-btn ${className}`, { type: "button", "aria-label": title || label });
    b.textContent = label;
    const active = new Set();
    const down = (e) => {
      e.preventDefault();
      e.stopPropagation();
      try { b.setPointerCapture(e.pointerId); } catch {}
      active.add(e.pointerId);
      b.classList.add("tc-down");
      handlers.down?.(e);
    };
    const up = (e) => {
      if (!active.has(e.pointerId)) return;
      e.preventDefault();
      active.delete(e.pointerId);
      if (active.size === 0) b.classList.remove("tc-down");
      handlers.up?.(e);
    };
    const move = (e) => {
      if (active.has(e.pointerId)) handlers.move?.(e);
    };
    b.addEventListener("pointerdown", down);
    b.addEventListener("pointerup", up);
    b.addEventListener("pointercancel", up);
    b.addEventListener("pointermove", move);
    b.addEventListener("contextmenu", (e) => e.preventDefault());
    parent.appendChild(b);
    return b;
  };

  const holdKey = (code) => ({ down: () => sendKey("keydown", code), up: () => sendKey("keyup", code) });
  const pressKey = (code) => ({ down: () => tapKey(code) });

  // ---------- mira (arrastar) ----------
  const lookPointers = new Map(); // pointerId -> {x, y}
  const addLook = (dx, dy) => {
    engine.pendingDx += dx * LOOK_SCALE;
    engine.pendingDy += dy * LOOK_SCALE;
  };

  const lookZone = el("div", "tc-zone tc-look-zone");
  lookZone.addEventListener("pointerdown", (e) => {
    e.preventDefault();
    try { lookZone.setPointerCapture(e.pointerId); } catch {}
    lookPointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  });
  lookZone.addEventListener("pointermove", (e) => {
    const p = lookPointers.get(e.pointerId);
    if (!p) return;
    addLook(e.clientX - p.x, e.clientY - p.y);
    p.x = e.clientX;
    p.y = e.clientY;
  });
  const endLook = (e) => lookPointers.delete(e.pointerId);
  lookZone.addEventListener("pointerup", endLook);
  lookZone.addEventListener("pointercancel", endLook);
  combatLayer.appendChild(lookZone);

  // ---------- analógico esquerdo (flutuante) ----------
  const stickZone = el("div", "tc-zone tc-stick-zone");
  const stickBase = el("div", "tc-stick-base tc-hidden");
  const stickKnob = el("div", "tc-stick-knob");
  stickBase.appendChild(stickKnob);
  stickZone.appendChild(stickBase);
  combatLayer.appendChild(stickZone);

  let stickId = null;
  let stickOrigin = { x: 0, y: 0 };
  const setStick = (nx, ny) => {
    engine.touchPad.active = nx !== 0 || ny !== 0;
    engine.touchPad.lx = Math.round(nx * 32767);
    engine.touchPad.ly = Math.round(-ny * 32767); // topo da tela = para frente
  };
  const stickEnd = (e) => {
    if (e.pointerId !== stickId) return;
    stickId = null;
    stickBase.classList.add("tc-hidden");
    setStick(0, 0);
  };
  stickZone.addEventListener("pointerdown", (e) => {
    if (stickId !== null) return;
    e.preventDefault();
    try { stickZone.setPointerCapture(e.pointerId); } catch {}
    stickId = e.pointerId;
    const rect = stickZone.getBoundingClientRect();
    stickOrigin = { x: e.clientX, y: e.clientY };
    stickBase.style.left = `${e.clientX - rect.left}px`;
    stickBase.style.top = `${e.clientY - rect.top}px`;
    stickKnob.style.transform = "translate(-50%, -50%)";
    stickBase.classList.remove("tc-hidden");
  });
  stickZone.addEventListener("pointermove", (e) => {
    if (e.pointerId !== stickId) return;
    let dx = e.clientX - stickOrigin.x;
    let dy = e.clientY - stickOrigin.y;
    const len = Math.hypot(dx, dy);
    if (len > STICK_RADIUS) {
      dx = (dx / len) * STICK_RADIUS;
      dy = (dy / len) * STICK_RADIUS;
    }
    stickKnob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
    let nx = dx / STICK_RADIUS;
    let ny = dy / STICK_RADIUS;
    if (Math.hypot(nx, ny) < STICK_DEADZONE) {
      nx = 0;
      ny = 0;
    }
    setStick(nx, ny);
  });
  stickZone.addEventListener("pointerup", stickEnd);
  stickZone.addEventListener("pointercancel", stickEnd);

  // ---------- botões de combate ----------
  const right = el("div", "tc-cluster tc-right");
  let fireLast = null;
  const fire = makeButton(
    right,
    "ATIRAR",
    "tc-fire",
    {
      down: (e) => {
        fireLast = { x: e.clientX, y: e.clientY };
        engine.mouseButtons |= 1;
        pushMouseButtons();
      },
      up: () => {
        engine.mouseButtons &= ~1;
        pushMouseButtons();
      },
      move: (e) => {
        // arrastar o dedo de tiro também mira (estilo shooters mobile)
        if (fireLast) addLook(e.clientX - fireLast.x, e.clientY - fireLast.y);
        fireLast = { x: e.clientX, y: e.clientY };
      },
    },
    "Atirar"
  );
  fire.dataset.role = "fire";
  const ads = makeButton(
    right,
    "MIRAR",
    "tc-ads",
    {
      down: () => {
        engine.mouseButtons ^= 2; // alterna (toque simples liga/desliga o zoom)
        ads.classList.toggle("tc-on", (engine.mouseButtons & 2) !== 0);
        pushMouseButtons();
      },
    },
    "Mirar (liga/desliga)"
  );
  makeButton(right, "PULAR", "tc-jump", holdKey("Space"), "Pular / Ação");
  makeButton(right, "AGACHAR", "tc-crouch", pressKey("KeyC"), "Agachar");
  makeButton(right, "RECARR.", "tc-reload", pressKey("KeyR"), "Recarregar / Interagir");

  const left = el("div", "tc-cluster tc-left");
  makeButton(left, "CORRER", "tc-sprint", holdKey("ShiftLeft"), "Correr");
  makeButton(left, "GRANADA", "tc-grenade", pressKey("KeyG"), "Granada");
  makeButton(left, "FACA", "tc-melee", pressKey("KeyV"), "Coronhada");
  makeButton(left, "ARMA", "tc-swap", pressKey("KeyY"), "Trocar de arma");
  combatLayer.append(right, left);

  const top = el("div", "tc-cluster tc-top");
  makeButton(top, "II", "tc-small tc-pause", pressKey("Enter"), "Pausa / Start");
  makeButton(
    top,
    "☰",
    "tc-small tc-options",
    { down: () => tapKey("F10") },
    "Opções / Online"
  );
  root.appendChild(top);

  // ---------- botões só do multiplayer ----------
  const mpBar = el("div", "tc-cluster tc-mpbar");
  makeButton(mpBar, "LOADOUT", "tc-small", pressKey("KeyB"), "Loja de loadout");
  makeButton(mpBar, "PLACAR", "tc-small", { down: () => sendKey("keydown", "Tab"), up: () => sendKey("keyup", "Tab") }, "Placar");
  mpLayer.appendChild(mpBar);

  // ---------- menus do jogo (fora da fase): D-pad + A/B + Start ----------
  const dpad = el("div", "tc-cluster tc-dpad");
  const dirBtn = (label, code, cls) => makeButton(dpad, label, `tc-dir ${cls}`, holdKey(code), label);
  dirBtn("▲", "ArrowUp", "tc-up");
  dirBtn("◀", "ArrowLeft", "tc-lf");
  dirBtn("▶", "ArrowRight", "tc-rt");
  dirBtn("▼", "ArrowDown", "tc-dn");
  const face = el("div", "tc-cluster tc-face");
  makeButton(face, "A", "tc-a", holdKey("Space"), "Confirmar (A)");
  makeButton(face, "B", "tc-b", holdKey("Backspace"), "Voltar (B)");
  makeButton(face, "START", "tc-start", pressKey("Enter"), "Start");
  makeButton(
    face,
    "PULAR TUDO",
    "tc-skipall",
    {
      down: () => {
        document
          .getElementById("btn-assist-start")
          ?.dispatchEvent(new MouseEvent("click", { shiftKey: true, bubbles: true }));
      },
    },
    "Pular todas as cutscenes"
  );
  menuLayer.append(dpad, face);

  // ---------- visibilidade / modo de jogo ----------
  api.update = ({ bootStarted, presenting, inLevel, overlay, multiplayer }) => {
    const show = Boolean(bootStarted && presenting && overlay === "none");
    root.classList.toggle("tc-hidden", !show);
    menuLayer.classList.toggle("tc-hidden", !(show && !inLevel));
    combatLayer.classList.toggle("tc-hidden", !(show && inLevel));
    mpLayer.classList.toggle("tc-hidden", !(show && inLevel && multiplayer));
    top.classList.toggle("tc-hidden", !show);
    document.body.classList.toggle("touch-playing", Boolean(bootStarted && overlay === "none"));
    if (!(show && inLevel) && stickId !== null) {
      stickId = null;
      stickBase.classList.add("tc-hidden");
      setStick(0, 0);
    }
  };

  api.enterPlayMode = () => {
    // Precisa rodar dentro do gesto do usuário (clique em Jogar).
    try {
      const p = document.documentElement.requestFullscreen?.({ navigationUI: "hide" });
      p?.catch?.(() => {});
    } catch {}
    try {
      screen.orientation?.lock?.("landscape")?.catch?.(() => {});
    } catch {}
  };

  // Evita zoom por pinça / duplo toque e rolagem durante o jogo.
  const stop = (e) => {
    if (document.body.classList.contains("touch-playing")) e.preventDefault();
  };
  document.addEventListener("gesturestart", stop, { passive: false });
  document.addEventListener("touchmove", stop, { passive: false });

  document.body.classList.add("touch-device");
  return api;
}
