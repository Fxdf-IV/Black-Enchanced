// Campanha Solo Single-Player 3D (First-Person) — BLACK Enhanced Online
// Motor 3D raycaster com Z-buffer, IA de esquadrão, barris explosivos em cadeia,
// visão tática auxiliar (TAB), Pointer Lock + Teclado + Gamepad e sintetizador Web Audio 48 kHz.

const SOLDIER_PALETTE = {
  rifle: { body: "#3b4252", trim: "#9ca3af", visor: "#f59e0b", scale: 1.0, speed: 2.1, rateMs: 950, dmg: 7 },
  shotgun: { body: "#4b2c2c", trim: "#d1d5db", visor: "#ef4444", scale: 1.05, speed: 2.4, rateMs: 1200, dmg: 12 },
  heavy: { body: "#1f2937", trim: "#fbbf24", visor: "#fb923c", scale: 1.18, speed: 1.5, rateMs: 700, dmg: 9 },
  sniper: { body: "#1e293b", trim: "#38bdf8", visor: "#38bdf8", scale: 0.96, speed: 1.4, rateMs: 1800, dmg: 18 },
  commander: { body: "#111827", trim: "#f59e0b", visor: "#ef4444", scale: 1.24, speed: 1.9, rateMs: 600, dmg: 11 },
};

export const SOLO_WEAPONS = [
  { id: 0, name: "M16A2", magSize: 30, reserveInit: 150, fireMs: 115, reloadMs: 1250, damage: 34, pellets: 1, spread: 0.018, auto: true, zoomFov: 0.52 },
  { id: 1, name: "SPAS-12", magSize: 8, reserveInit: 40, fireMs: 560, reloadMs: 1450, damage: 22, pellets: 6, spread: 0.095, auto: false, zoomFov: 0.58 },
  { id: 2, name: "AWP .338", magSize: 5, reserveInit: 25, fireMs: 980, reloadMs: 1650, damage: 135, pellets: 1, spread: 0.002, auto: false, zoomFov: 0.25 },
];

export class TacticalAudio {
  constructor() {
    this.ctx = null;
    this.noiseBuf = null;
    this.rateHz = 48000;
    this.muted = false;
  }

  unlock() {
    if (this.muted) return;
    if (!this.ctx) {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) return;
      try {
        this.ctx = new Ctx({ sampleRate: 48000 });
      } catch {
        this.ctx = new Ctx();
      }
      this.rateHz = this.ctx.sampleRate;
      const len = Math.floor(this.ctx.sampleRate * 1.2);
      this.noiseBuf = this.ctx.createBuffer(1, len, this.ctx.sampleRate);
      const data = this.noiseBuf.getChannelData(0);
      for (let i = 0; i < len; i++) {
        data[i] = Math.random() * 2 - 1;
      }
    }
    if (this.ctx && this.ctx.state === "suspended") {
      this.ctx.resume().catch(() => {});
    }
  }

  playShot(weaponId) {
    if (this.muted || !this.ctx || !this.noiseBuf) return;
    const now = this.ctx.currentTime;

    // Ruído balístico filtrado
    const src = this.ctx.createBufferSource();
    src.buffer = this.noiseBuf;

    const filter = this.ctx.createBiquadFilter();
    filter.type = "bandpass";
    filter.frequency.setValueAtTime(weaponId === 1 ? 650 : weaponId === 2 ? 1200 : 1650, now);
    filter.Q.value = weaponId === 2 ? 0.65 : 0.9;

    const gain = this.ctx.createGain();
    const dur = weaponId === 2 ? 0.36 : weaponId === 1 ? 0.28 : 0.16;
    const peak = weaponId === 2 ? 0.48 : weaponId === 1 ? 0.42 : 0.3;
    gain.gain.setValueAtTime(peak, now);
    gain.gain.exponentialRampToValueAtTime(0.001, now + dur);

    src.connect(filter);
    filter.connect(gain);
    gain.connect(this.ctx.destination);
    src.start(now);
    src.stop(now + dur);

    // Impacto grave do ferrolho / subgrave
    const osc = this.ctx.createOscillator();
    const og = this.ctx.createGain();
    osc.type = "triangle";
    osc.frequency.setValueAtTime(weaponId === 1 ? 140 : weaponId === 2 ? 175 : 195, now);
    osc.frequency.exponentialRampToValueAtTime(38, now + 0.11);
    og.gain.setValueAtTime(0.32, now);
    og.gain.exponentialRampToValueAtTime(0.001, now + 0.12);
    osc.connect(og);
    og.connect(this.ctx.destination);
    osc.start(now);
    osc.stop(now + 0.12);
  }

  playExplosion() {
    if (this.muted || !this.ctx || !this.noiseBuf) return;
    const now = this.ctx.currentTime;

    const src = this.ctx.createBufferSource();
    src.buffer = this.noiseBuf;
    const lp = this.ctx.createBiquadFilter();
    lp.type = "lowpass";
    lp.frequency.setValueAtTime(920, now);
    lp.frequency.exponentialRampToValueAtTime(90, now + 0.78);

    const g = this.ctx.createGain();
    g.gain.setValueAtTime(0.65, now);
    g.gain.exponentialRampToValueAtTime(0.001, now + 0.8);

    src.connect(lp);
    lp.connect(g);
    g.connect(this.ctx.destination);
    src.start(now);
    src.stop(now + 0.8);

    const sub = this.ctx.createOscillator();
    const sg = this.ctx.createGain();
    sub.type = "sine";
    sub.frequency.setValueAtTime(95, now);
    sub.frequency.exponentialRampToValueAtTime(24, now + 0.55);
    sg.gain.setValueAtTime(0.55, now);
    sg.gain.exponentialRampToValueAtTime(0.001, now + 0.58);
    sub.connect(sg);
    sg.connect(this.ctx.destination);
    sub.start(now);
    sub.stop(now + 0.58);
  }

  playReload() {
    if (this.muted || !this.ctx) return;
    const now = this.ctx.currentTime;
    [0, 0.24].forEach((offset, idx) => {
      const osc = this.ctx.createOscillator();
      const g = this.ctx.createGain();
      osc.type = "square";
      osc.frequency.setValueAtTime(idx === 0 ? 520 : 780, now + offset);
      g.gain.setValueAtTime(0.09, now + offset);
      g.gain.exponentialRampToValueAtTime(0.001, now + offset + 0.055);
      osc.connect(g);
      g.connect(this.ctx.destination);
      osc.start(now + offset);
      osc.stop(now + offset + 0.06);
    });
  }

  playHitmarker(isKill) {
    if (this.muted || !this.ctx) return;
    const now = this.ctx.currentTime;
    const osc = this.ctx.createOscillator();
    const g = this.ctx.createGain();
    osc.type = "sine";
    osc.frequency.setValueAtTime(isKill ? 1480 : 1080, now);
    if (isKill) osc.frequency.exponentialRampToValueAtTime(1920, now + 0.07);
    g.gain.setValueAtTime(0.14, now);
    g.gain.exponentialRampToValueAtTime(0.001, now + 0.085);
    osc.connect(g);
    g.connect(this.ctx.destination);
    osc.start(now);
    osc.stop(now + 0.09);
  }

  playDamage() {
    if (this.muted || !this.ctx) return;
    const now = this.ctx.currentTime;
    const osc = this.ctx.createOscillator();
    const g = this.ctx.createGain();
    osc.type = "sawtooth";
    osc.frequency.setValueAtTime(110, now);
    osc.frequency.exponentialRampToValueAtTime(48, now + 0.14);
    g.gain.setValueAtTime(0.18, now);
    g.gain.exponentialRampToValueAtTime(0.001, now + 0.15);
    osc.connect(g);
    g.connect(this.ctx.destination);
    osc.start(now);
    osc.stop(now + 0.15);
  }
}

function normalizeAngle(a) {
  while (a > Math.PI) a -= Math.PI * 2;
  while (a < -Math.PI) a += Math.PI * 2;
  return a;
}

export class SoloCampaignEngine {
  constructor({ onLog, onHudUpdate }) {
    this.onLog = onLog || (() => {});
    this.onHudUpdate = onHudUpdate || (() => {});
    this.audio = new TacticalAudio();
    this.active = true;
    this.topDownMode = false;
    this.pointerLocked = false;
    this.mission = null;
    this.grid = [];
    this.mapW = 32;
    this.mapH = 21;

    this.keys = new Set();
    this.mouseDownLeft = false;
    this.mouseDownRight = false;

    this.resetState();
  }

  resetState() {
    this.sectorIndex = 0;
    this.missionCompleted = false;
    this.playerDead = false;
    this.kills = 0;
    this.shotsFired = 0;
    this.barrelsBlown = 0;

    this.player = {
      x: 2.5,
      y: 2.5,
      yaw: 0.35,
      pitch: 0,
      hp: 100,
      armor: 50,
      crouch: false,
      grenades: 3,
      bobPhase: 0,
      bobAmount: 0,
      damageFlash: 0,
      shake: 0,
    };

    this.weaponIdx = 0;
    this.weapons = SOLO_WEAPONS.map((w) => ({
      ...w,
      mag: w.magSize,
      reserve: w.reserveInit,
    }));
    this.lastFireMs = 0;
    this.reloadingUntilMs = 0;
    this.muzzleFlashMs = 0;
    this.recoilKick = 0;
    this.hitmarkerMs = 0;
    this.hitmarkerKill = false;
    this.sectorBannerMs = 0;
    this.sectorBannerText = "";

    this.enemies = [];
    this.barrels = [];
    this.crates = [];
    this.pickups = [];
    this.particles = [];
    this.grenades = [];
  }

  loadMission(missionData) {
    this.mission = missionData;
    this.grid = missionData.grid || [];
    this.mapH = this.grid.length;
    this.mapW = this.mapH > 0 ? this.grid[0].length : 32;
    this.resetState();

    if (missionData.spawn) {
      this.player.x = missionData.spawn.x;
      this.player.y = missionData.spawn.y;
      this.player.yaw = missionData.spawn.yaw || 0.35;
    }

    this.spawnSector(0);
    this.onLog("SOLO", `Campanha carregada: ${missionData.title}`);
  }

  spawnSector(index) {
    if (!this.mission || !this.mission.sectors || index >= this.mission.sectors.length) {
      this.missionCompleted = true;
      this.sectorBannerText = "MISSAO CUMPRIDA // TRENESKA LIBERADA";
      this.sectorBannerMs = performance.now() + 6000;
      this.onLog("SOLO", "Todos os 3 setores concluídos! Missão cumprida.");
      return;
    }

    this.sectorIndex = index;
    const sec = this.mission.sectors[index];
    this.sectorBannerText = sec.name;
    this.sectorBannerMs = performance.now() + 3600;
    this.onLog("SETOR", `${sec.name} — ${sec.objective}`);

    this.enemies = (sec.enemies || []).map((e, idx) => ({
      id: idx + 1,
      x: e.x,
      y: e.y,
      hp: e.hp,
      maxHp: e.hp,
      kind: e.kind || "rifle",
      patrol: e.patrol || [[e.x, e.y]],
      patrolIdx: 0,
      alerted: false,
      lastShotMs: 0,
      muzzleUntilMs: 0,
      hitUntilMs: 0,
      alive: true,
    }));

    this.barrels = (sec.barrels || []).map((b) => ({
      x: b.x,
      y: b.y,
      hp: 35,
      alive: true,
    }));

    this.crates = (sec.crates || []).map((c) => ({
      x: c.x,
      y: c.y,
      hp: 40,
      ammo: Boolean(c.ammo),
      alive: true,
    }));
  }

  cellAt(gx, gy) {
    const x = Math.floor(gx);
    const y = Math.floor(gy);
    if (y < 0 || y >= this.mapH || x < 0 || x >= this.mapW) return "#";
    const ch = this.grid[y][x];
    return ch === "." ? "." : ch;
  }

  isSolid(gx, gy) {
    return this.cellAt(gx, gy) !== ".";
  }

  hasLineOfSight(x0, y0, x1, y1) {
    const dx = x1 - x0;
    const dy = y1 - y0;
    const dist = Math.hypot(dx, dy);
    if (dist < 0.05) return true;
    const steps = Math.ceil(dist * 6);
    for (let i = 1; i < steps; i++) {
      const t = i / steps;
      if (this.isSolid(x0 + dx * t, y0 + dy * t)) return false;
    }
    return true;
  }

  startReload() {
    const now = performance.now();
    const w = this.weapons[this.weaponIdx];
    if (this.reloadingUntilMs > now) return;
    if (w.mag >= w.magSize || w.reserve <= 0) return;
    this.reloadingUntilMs = now + w.reloadMs;
    this.audio.playReload();
  }

  finishReloadIfReady(now) {
    if (this.reloadingUntilMs === 0 || now < this.reloadingUntilMs) return;
    this.reloadingUntilMs = 0;
    const w = this.weapons[this.weaponIdx];
    const need = w.magSize - w.mag;
    const take = Math.min(need, w.reserve);
    w.mag += take;
    w.reserve -= take;
  }

  throwGrenade() {
    if (this.playerDead || this.missionCompleted || this.player.grenades <= 0) return;
    this.player.grenades -= 1;
    const speed = 10.5;
    this.grenades.push({
      x: this.player.x + Math.cos(this.player.yaw) * 0.4,
      y: this.player.y + Math.sin(this.player.yaw) * 0.4,
      vx: Math.cos(this.player.yaw) * speed,
      vy: Math.sin(this.player.yaw) * speed,
      detonateAtMs: performance.now() + 1050,
    });
    this.onLog("GRANADA", `Granada lançada (${this.player.grenades} restantes).`);
  }

  triggerExplosion(ex, ey, radius, maxDamage) {
    this.audio.playExplosion();
    this.player.shake = Math.max(this.player.shake, 14);
    for (let i = 0; i < 24; i++) {
      const ang = Math.random() * Math.PI * 2;
      const spd = 1 + Math.random() * 4.5;
      this.particles.push({
        x: ex,
        y: ey,
        z: 0.2 + Math.random() * 0.4,
        vx: Math.cos(ang) * spd,
        vy: Math.sin(ang) * spd,
        vz: 1.5 + Math.random() * 3.5,
        color: i % 2 === 0 ? "#f59e0b" : "#ef4444",
        size: 0.25 + Math.random() * 0.35,
        life: 0.55 + Math.random() * 0.35,
        maxLife: 0.9,
      });
    }

    // Dano em inimigos
    for (const enemy of this.enemies) {
      if (!enemy.alive) continue;
      const d = Math.hypot(enemy.x - ex, enemy.y - ey);
      if (d < radius) {
        const dmg = Math.round(maxDamage * (1 - d / radius));
        this.damageEnemy(enemy, dmg, true);
      }
    }

    // Detonação em cadeia de outros barris próximos!
    for (const b of this.barrels) {
      if (!b.alive) continue;
      const d = Math.hypot(b.x - ex, b.y - ey);
      if (d > 0.05 && d < radius * 0.9) {
        b.alive = false;
        this.barrelsBlown += 1;
        setTimeout(() => this.triggerExplosion(b.x, b.y, 4.0, 130), 90);
      }
    }

    // Dano no próprio jogador se estiver muito perto
    const dp = Math.hypot(this.player.x - ex, this.player.y - ey);
    if (dp < radius * 0.7) {
      this.damagePlayer(Math.round(35 * (1 - dp / (radius * 0.7))));
    }
  }

  damageEnemy(enemy, dmg, fromExplosion = false) {
    if (!enemy.alive) return;
    enemy.hp -= dmg;
    enemy.alerted = true;
    enemy.hitUntilMs = performance.now() + 140;

    if (enemy.hp <= 0) {
      enemy.hp = 0;
      enemy.alive = false;
      this.kills += 1;
      this.hitmarkerMs = performance.now() + 180;
      this.hitmarkerKill = true;
      this.audio.playHitmarker(true);
      for (let i = 0; i < 8; i++) {
        this.particles.push({
          x: enemy.x,
          y: enemy.y,
          z: 0.5,
          vx: (Math.random() - 0.5) * 2.5,
          vy: (Math.random() - 0.5) * 2.5,
          vz: Math.random() * 2.5,
          color: fromExplosion ? "#f59e0b" : "#ef4444",
          size: 0.14,
          life: 0.45,
          maxLife: 0.45,
        });
      }
      this.checkSectorCleared();
    } else {
      this.hitmarkerMs = performance.now() + 110;
      this.hitmarkerKill = false;
      this.audio.playHitmarker(false);
    }
  }

  damagePlayer(dmg) {
    if (this.playerDead || dmg <= 0) return;
    let rem = dmg;
    if (this.player.armor > 0) {
      const absorb = Math.min(this.player.armor, Math.ceil(dmg * 0.55));
      this.player.armor -= absorb;
      rem -= absorb;
    }
    this.player.hp = Math.max(0, this.player.hp - rem);
    this.player.damageFlash = 1.0;
    this.player.shake = Math.max(this.player.shake, 6);
    this.audio.playDamage();

    if (this.player.hp === 0) {
      this.playerDead = true;
      this.onLog("SOLO", "Operador abatido! Clique em Reiniciar Missão ou pressione R.");
    }
  }

  checkSectorCleared() {
    const aliveCount = this.enemies.filter((e) => e.alive).length;
    if (aliveCount === 0 && !this.missionCompleted) {
      setTimeout(() => {
        if (this.sectorIndex + 1 < (this.mission?.sectors?.length || 0)) {
          this.player.hp = Math.min(100, this.player.hp + 30);
          this.spawnSector(this.sectorIndex + 1);
        } else {
          this.spawnSector(this.sectorIndex + 1);
        }
      }, 950);
    }
  }

  fireWeapon() {
    const now = performance.now();
    if (this.playerDead || this.missionCompleted) return;
    const w = this.weapons[this.weaponIdx];
    if (this.reloadingUntilMs > now) return;
    if (now - this.lastFireMs < w.fireMs) return;

    if (w.mag <= 0) {
      this.lastFireMs = now + 220;
      this.startReload();
      return;
    }

    this.lastFireMs = now;
    w.mag -= 1;
    this.shotsFired += 1;
    this.muzzleFlashMs = now + 55;
    this.recoilKick = Math.min(1.0, this.recoilKick + (this.weaponIdx === 1 ? 0.9 : 0.45));
    this.player.shake = Math.max(this.player.shake, this.weaponIdx === 2 ? 7 : 3);
    this.audio.playShot(this.weaponIdx);

    // Som do disparo alerta inimigos próximos
    for (const enemy of this.enemies) {
      if (enemy.alive && Math.hypot(enemy.x - this.player.x, enemy.y - this.player.y) < 13) {
        enemy.alerted = true;
      }
    }

    const crouchFactor = this.player.crouch ? 0.55 : 1.0;
    const adsFactor = this.isAiming() ? 0.35 : 1.0;

    for (let p = 0; p < w.pellets; p++) {
      const ang = this.player.yaw + (Math.random() - 0.5) * w.spread * crouchFactor * adsFactor * 2;
      const dx = Math.cos(ang);
      const dy = Math.sin(ang);

      // Encontra distância até parede mais próxima
      let wallDist = 40;
      for (let step = 0.2; step < 36; step += 0.12) {
        if (this.isSolid(this.player.x + dx * step, this.player.y + dy * step)) {
          wallDist = step;
          // Faísca na parede
          this.particles.push({
            x: this.player.x + dx * (step - 0.12),
            y: this.player.y + dy * (step - 0.12),
            z: 0.5 + (Math.random() - 0.5) * 0.2,
            vx: -dx * 1.4 + (Math.random() - 0.5),
            vy: -dy * 1.4 + (Math.random() - 0.5),
            vz: 1.0 + Math.random(),
            color: "#fbbf24",
            size: 0.08,
            life: 0.22,
            maxLife: 0.22,
          });
          break;
        }
      }

      // Testa colisão contra inimigos, barris e caixas antes da parede
      let bestHit = null;
      let bestDist = wallDist;

      for (const enemy of this.enemies) {
        if (!enemy.alive) continue;
        const rx = enemy.x - this.player.x;
        const ry = enemy.y - this.player.y;
        const proj = rx * dx + ry * dy;
        if (proj <= 0 || proj > bestDist) continue;
        const perp = Math.abs(rx * dy - ry * dx);
        if (perp < 0.36) {
          bestDist = proj;
          bestHit = { kind: "enemy", obj: enemy };
        }
      }

      for (const b of this.barrels) {
        if (!b.alive) continue;
        const rx = b.x - this.player.x;
        const ry = b.y - this.player.y;
        const proj = rx * dx + ry * dy;
        if (proj <= 0 || proj > bestDist) continue;
        const perp = Math.abs(rx * dy - ry * dx);
        if (perp < 0.34) {
          bestDist = proj;
          bestHit = { kind: "barrel", obj: b };
        }
      }

      for (const c of this.crates) {
        if (!c.alive) continue;
        const rx = c.x - this.player.x;
        const ry = c.y - this.player.y;
        const proj = rx * dx + ry * dy;
        if (proj <= 0 || proj > bestDist) continue;
        const perp = Math.abs(rx * dy - ry * dx);
        if (perp < 0.35) {
          bestDist = proj;
          bestHit = { kind: "crate", obj: c };
        }
      }

      if (bestHit) {
        if (bestHit.kind === "enemy") {
          this.damageEnemy(bestHit.obj, w.damage);
        } else if (bestHit.kind === "barrel") {
          const b = bestHit.obj;
          b.hp -= w.damage;
          this.hitmarkerMs = now + 90;
          if (b.hp <= 0) {
            b.alive = false;
            this.barrelsBlown += 1;
            this.triggerExplosion(b.x, b.y, 4.2, 145);
          }
        } else if (bestHit.kind === "crate") {
          const c = bestHit.obj;
          c.hp -= w.damage;
          if (c.hp <= 0) {
            c.alive = false;
            this.pickups.push({
              x: c.x,
              y: c.y,
              type: c.ammo ? "ammo" : "armor",
              alive: true,
            });
            for (let k = 0; k < 6; k++) {
              this.particles.push({
                x: c.x,
                y: c.y,
                z: 0.35,
                vx: (Math.random() - 0.5) * 3,
                vy: (Math.random() - 0.5) * 3,
                vz: 1 + Math.random() * 2,
                color: "#d97706",
                size: 0.12,
                life: 0.4,
                maxLife: 0.4,
              });
            }
          }
        }
      }
    }

    if (w.mag === 0) {
      this.startReload();
    }
  }

  isAiming() {
    return this.mouseDownRight || this.keys.has("KeyE");
  }

  pollGamepad(dt) {
    if (!navigator.getGamepads) return { moveF: 0, moveS: 0, turn: 0, fire: false };
    const pads = navigator.getGamepads();
    const gp = pads && pads[0];
    if (!gp) return { moveF: 0, moveS: 0, turn: 0, fire: false };
    const dead = (v) => (Math.abs(v) < 0.18 ? 0 : v);
    const moveS = dead(gp.axes[0] || 0);
    const moveF = -dead(gp.axes[1] || 0);
    const turn = dead(gp.axes[2] || 0) * 2.6 * dt;
    const fire = Boolean(gp.buttons[7]?.pressed);
    if (gp.buttons[2]?.pressed) this.startReload();
    return { moveF, moveS, turn, fire };
  }

  update(dt) {
    if (!this.active || !this.mission) return;
    const now = performance.now();

    this.finishReloadIfReady(now);

    if (this.player.damageFlash > 0) this.player.damageFlash = Math.max(0, this.player.damageFlash - dt * 3.2);
    if (this.player.shake > 0) this.player.shake = Math.max(0, this.player.shake - dt * 28);
    if (this.recoilKick > 0) this.recoilKick = Math.max(0, this.recoilKick - dt * 5.2);

    const gp = this.pollGamepad(dt);

    if (!this.playerDead) {
      this.player.crouch = this.keys.has("ShiftLeft") || this.keys.has("ShiftRight") || this.keys.has("KeyC");
      const baseSpeed = this.player.crouch ? 2.4 : 4.2;

      let fwd = gp.moveF;
      let strafe = gp.moveS;
      if (this.keys.has("KeyW") || this.keys.has("ArrowUp")) fwd += 1;
      if (this.keys.has("KeyS") || this.keys.has("ArrowDown")) fwd -= 1;
      if (this.keys.has("KeyD")) strafe += 1;
      if (this.keys.has("KeyA")) strafe -= 1;

      if (this.keys.has("ArrowLeft")) this.player.yaw -= 2.3 * dt;
      if (this.keys.has("ArrowRight")) this.player.yaw += 2.3 * dt;
      this.player.yaw += gp.turn;

      const cosY = Math.cos(this.player.yaw);
      const sinY = Math.sin(this.player.yaw);
      let vx = cosY * fwd - sinY * strafe;
      let vy = sinY * fwd + cosY * strafe;
      const vLen = Math.hypot(vx, vy);
      if (vLen > 0.01) {
        vx = (vx / vLen) * baseSpeed * dt;
        vy = (vy / vLen) * baseSpeed * dt;
        this.player.bobPhase += dt * (this.player.crouch ? 7 : 10.5);
        this.player.bobAmount = Math.min(1, this.player.bobAmount + dt * 8);
      } else {
        this.player.bobAmount = Math.max(0, this.player.bobAmount - dt * 6);
      }

      const margin = 0.28;
      const nx = this.player.x + vx;
      if (!this.isSolid(nx + Math.sign(vx) * margin, this.player.y) && !this.isSolid(nx, this.player.y + margin) && !this.isSolid(nx, this.player.y - margin)) {
        this.player.x = nx;
      }
      const ny = this.player.y + vy;
      if (!this.isSolid(this.player.x, ny + Math.sign(vy) * margin) && !this.isSolid(this.player.x + margin, ny) && !this.isSolid(this.player.x - margin, ny)) {
        this.player.y = ny;
      }

      if ((this.mouseDownLeft || this.keys.has("Space") || gp.fire) && (this.weapons[this.weaponIdx].auto || now - this.lastFireMs > 260)) {
        this.fireWeapon();
      }

      // Coleta pickups ao passar por cima
      for (const pk of this.pickups) {
        if (!pk.alive) continue;
        if (Math.hypot(pk.x - this.player.x, pk.y - this.player.y) < 0.75) {
          pk.alive = false;
          if (pk.type === "ammo") {
            this.weapons.forEach((w) => (w.reserve += w.magSize * 2));
            this.player.grenades = Math.min(5, this.player.grenades + 1);
            this.onLog("LOOT", "+Munição e +1 Granada recolhida.");
          } else {
            this.player.armor = Math.min(100, this.player.armor + 35);
            this.player.hp = Math.min(100, this.player.hp + 20);
            this.onLog("LOOT", "+35 Colete balístico e +20 HP.");
          }
          this.audio.playReload();
        }
      }
    }

    // Atualiza granadas
    for (let i = this.grenades.length - 1; i >= 0; i--) {
      const g = this.grenades[i];
      const nx = g.x + g.vx * dt;
      const ny = g.y + g.vy * dt;
      if (this.isSolid(nx, g.y)) g.vx *= -0.5;
      else g.x = nx;
      if (this.isSolid(g.x, ny)) g.vy *= -0.5;
      else g.y = ny;
      g.vx *= 0.96;
      g.vy *= 0.96;
      if (now >= g.detonateAtMs) {
        this.triggerExplosion(g.x, g.y, 4.5, 160);
        this.grenades.splice(i, 1);
      }
    }

    // Atualiza IA inimiga (equivalente ao sub_001E5F70 AiController)
    for (const enemy of this.enemies) {
      if (!enemy.alive) continue;
      const spec = SOLDIER_PALETTE[enemy.kind] || SOLDIER_PALETTE.rifle;
      const distP = Math.hypot(this.player.x - enemy.x, this.player.y - enemy.y);
      const canSee = !this.playerDead && distP < 15.5 && this.hasLineOfSight(enemy.x, enemy.y, this.player.x, this.player.y);

      if (canSee) {
        enemy.alerted = true;
      }

      if (enemy.alerted && !this.playerDead) {
        if (canSee && distP < 13.5) {
          // Aproxima um pouco se longe, mantém distância tática se perto
          if (distP > 5.5) {
            const tx = ((this.player.x - enemy.x) / distP) * spec.speed * dt;
            const ty = ((this.player.y - enemy.y) / distP) * spec.speed * dt;
            if (!this.isSolid(enemy.x + tx, enemy.y)) enemy.x += tx;
            if (!this.isSolid(enemy.x, enemy.y + ty)) enemy.y += ty;
          }
          if (now - enemy.lastShotMs >= spec.rateMs) {
            enemy.lastShotMs = now + Math.random() * 250;
            enemy.muzzleUntilMs = now + 70;
            const hitChance = Math.max(0.22, 0.72 - distP * 0.035) * (this.player.crouch ? 0.72 : 1.0);
            if (Math.random() < hitChance) {
              this.damagePlayer(spec.dmg);
            }
          }
        }
      } else if (enemy.patrol && enemy.patrol.length > 0) {
        const pt = enemy.patrol[enemy.patrolIdx % enemy.patrol.length];
        const dx = pt[0] - enemy.x;
        const dy = pt[1] - enemy.y;
        const d = Math.hypot(dx, dy);
        if (d < 0.25) {
          enemy.patrolIdx = (enemy.patrolIdx + 1) % enemy.patrol.length;
        } else {
          const sx = (dx / d) * (spec.speed * 0.65) * dt;
          const sy = (dy / d) * (spec.speed * 0.65) * dt;
          if (!this.isSolid(enemy.x + sx, enemy.y)) enemy.x += sx;
          if (!this.isSolid(enemy.x, enemy.y + sy)) enemy.y += sy;
        }
      }
    }

    // Atualiza partículas 3D
    for (let i = this.particles.length - 1; i >= 0; i--) {
      const pt = this.particles[i];
      pt.x += pt.vx * dt;
      pt.y += pt.vy * dt;
      pt.z = Math.max(0, pt.z + pt.vz * dt);
      pt.vz -= 7.5 * dt;
      pt.life -= dt;
      if (pt.life <= 0) this.particles.splice(i, 1);
    }

    this.onHudUpdate(this.getHudSnapshot());
  }

  getHudSnapshot() {
    const w = this.weapons[this.weaponIdx];
    const sec = this.mission?.sectors?.[this.sectorIndex];
    return {
      sectorName: sec ? sec.name : "MISSAO CONCLUIDA",
      objective: sec ? sec.objective : "Treneska Border Crossing dominada.",
      enemiesAlive: this.enemies.filter((e) => e.alive).length,
      enemiesTotal: this.enemies.length,
      hp: this.player.hp,
      armor: this.player.armor,
      grenades: this.player.grenades,
      weaponName: w.name,
      mag: w.mag,
      magSize: w.magSize,
      reserve: w.reserve,
      reloading: this.reloadingUntilMs > performance.now(),
      kills: this.kills,
      barrelsBlown: this.barrelsBlown,
      sampleRate: this.audio.rateHz,
      dead: this.playerDead,
      completed: this.missionCompleted,
    };
  }

  render(canvas) {
    if (!canvas || !this.mission) return;
    const ctx = canvas.getContext("2d");
    const w = canvas.width;
    const h = canvas.height;
    const now = performance.now();

    if (this.topDownMode) {
      this.renderTopDown(ctx, w, h);
      return;
    }

    const shakeX = this.player.shake > 0 ? (Math.random() - 0.5) * this.player.shake : 0;
    const shakeY = this.player.shake > 0 ? (Math.random() - 0.5) * this.player.shake : 0;
    const horizon = h * 0.5 + this.player.pitch * 180 + (this.player.crouch ? 24 : 0) + Math.sin(this.player.bobPhase) * 3.5 * this.player.bobAmount + shakeY;

    // Céu noturno/dusk estilo Treneska (BLACK 2006)
    const skyGrad = ctx.createLinearGradient(0, 0, 0, Math.max(10, horizon));
    skyGrad.addColorStop(0, "#0b0e14");
    skyGrad.addColorStop(1, "#262b36");
    ctx.fillStyle = skyGrad;
    ctx.fillRect(0, 0, w, Math.max(0, horizon));

    // Chão concreto com fog atmosférico
    const floorGrad = ctx.createLinearGradient(0, Math.max(0, horizon), 0, h);
    floorGrad.addColorStop(0, "#14171c");
    floorGrad.addColorStop(1, "#232730");
    ctx.fillStyle = floorGrad;
    ctx.fillRect(0, Math.max(0, horizon), w, h - Math.max(0, horizon));

    const activeWeapon = this.weapons[this.weaponIdx];
    const fov = this.isAiming() ? activeWeapon.zoomFov : 0.72;
    const numRays = 190; // 4px por coluna em 760px -> suave e ultra-rápido a 60 FPS
    const colW = w / numRays;
    const zBuffer = new Float32Array(numRays);

    for (let i = 0; i < numRays; i++) {
      const camX = (i / numRays - 0.5) * 2;
      const rayAngle = this.player.yaw + Math.atan(camX * Math.tan(fov));
      const rdx = Math.cos(rayAngle);
      const rdy = Math.sin(rayAngle);

      let mapX = Math.floor(this.player.x);
      let mapY = Math.floor(this.player.y);
      const deltaDistX = Math.abs(1 / (rdx || 1e-6));
      const deltaDistY = Math.abs(1 / (rdy || 1e-6));

      let stepX, sideDistX;
      if (rdx < 0) {
        stepX = -1;
        sideDistX = (this.player.x - mapX) * deltaDistX;
      } else {
        stepX = 1;
        sideDistX = (mapX + 1.0 - this.player.x) * deltaDistX;
      }

      let stepY, sideDistY;
      if (rdy < 0) {
        stepY = -1;
        sideDistY = (this.player.y - mapY) * deltaDistY;
      } else {
        stepY = 1;
        sideDistY = (mapY + 1.0 - this.player.y) * deltaDistY;
      }

      let hit = false;
      let side = 0;
      let tile = "#";
      let guard = 0;
      while (!hit && guard++ < 48) {
        if (sideDistX < sideDistY) {
          sideDistX += deltaDistX;
          mapX += stepX;
          side = 0;
        } else {
          sideDistY += deltaDistY;
          mapY += stepY;
          side = 1;
        }
        tile = this.cellAt(mapX, mapY);
        if (tile !== ".") hit = true;
      }

      const rawDist = side === 0 ? sideDistX - deltaDistX : sideDistY - deltaDistY;
      const perpDist = Math.max(0.15, rawDist * Math.cos(rayAngle - this.player.yaw));
      zBuffer[i] = perpDist;

      const lineH = h / perpDist;
      const drawTop = horizon - lineH * 0.52;
      const fog = Math.max(0.15, Math.min(1, 1 - perpDist / 22));
      const shade = side === 1 ? 0.78 : 1.0;

      // Cores por material arquitetônico (concreto, bunker, contêiner metálico)
      let baseR = 68, baseG = 74, baseB = 84;
      if (tile === "B") {
        baseR = 92; baseG = 78; baseB = 64;
      } else if (tile === "C") {
        baseR = 52; baseG = 78; baseB = 90;
      }
      // Textura procedural sutil por coordenada de impacto
      const wallU = side === 0 ? this.player.y + rawDist * rdy : this.player.x + rawDist * rdx;
      const stripe = ((Math.floor((wallU - Math.floor(wallU)) * 8) % 2) === 0 ? 1.0 : 0.92);

      const r = Math.floor(baseR * fog * shade * stripe);
      const g = Math.floor(baseG * fog * shade * stripe);
      const b = Math.floor(baseB * fog * shade * stripe);
      ctx.fillStyle = `rgb(${r},${g},${b})`;
      ctx.fillRect(Math.floor(i * colW + shakeX), Math.floor(drawTop), Math.ceil(colW) + 1, Math.ceil(lineH));
    }

    // Ordena todos os objetos 3D por distância (painter's algorithm + zBuffer)
    const renderables = [];
    for (const enemy of this.enemies) {
      if (enemy.alive) renderables.push({ kind: "enemy", obj: enemy, dist: Math.hypot(enemy.x - this.player.x, enemy.y - this.player.y) });
    }
    for (const b of this.barrels) {
      if (b.alive) renderables.push({ kind: "barrel", obj: b, dist: Math.hypot(b.x - this.player.x, b.y - this.player.y) });
    }
    for (const c of this.crates) {
      if (c.alive) renderables.push({ kind: "crate", obj: c, dist: Math.hypot(c.x - this.player.x, c.y - this.player.y) });
    }
    for (const pk of this.pickups) {
      if (pk.alive) renderables.push({ kind: "pickup", obj: pk, dist: Math.hypot(pk.x - this.player.x, pk.y - this.player.y) });
    }
    for (const g of this.grenades) {
      renderables.push({ kind: "grenade", obj: g, dist: Math.hypot(g.x - this.player.x, g.y - this.player.y) });
    }
    for (const pt of this.particles) {
      renderables.push({ kind: "particle", obj: pt, dist: Math.hypot(pt.x - this.player.x, pt.y - this.player.y) });
    }

    renderables.sort((a, b) => b.dist - a.dist);

    const tanHalfFov = Math.tan(fov);
    for (const item of renderables) {
      const dist = item.dist;
      if (dist < 0.2 || dist > 26) continue;
      const dx = item.obj.x - this.player.x;
      const dy = item.obj.y - this.player.y;
      const ang = normalizeAngle(Math.atan2(dy, dx) - this.player.yaw);
      if (Math.abs(ang) > fov + 0.35) continue;

      const screenX = w * 0.5 + (Math.tan(ang) / tanHalfFov) * (w * 0.5) + shakeX;
      const rayIdx = Math.floor(screenX / colW);
      if (rayIdx >= 0 && rayIdx < numRays && dist > zBuffer[rayIdx] + 0.15) {
        continue; // atrás da parede
      }

      const spriteH = h / dist;
      const baseBottom = horizon + spriteH * 0.48;

      if (item.kind === "barrel") {
        const bw = spriteH * 0.36;
        const bh = spriteH * 0.56;
        ctx.fillStyle = "#b91c1c";
        ctx.fillRect(screenX - bw * 0.5, baseBottom - bh, bw, bh);
        ctx.fillStyle = "#ef4444";
        ctx.fillRect(screenX - bw * 0.5, baseBottom - bh + bh * 0.18, bw, bh * 0.14);
        ctx.fillStyle = "#fbbf24";
        ctx.fillRect(screenX - bw * 0.5, baseBottom - bh + bh * 0.55, bw, bh * 0.1);
      } else if (item.kind === "crate") {
        const cw = spriteH * 0.46;
        const ch = spriteH * 0.44;
        ctx.fillStyle = item.obj.ammo ? "#4d5e3a" : "#78512b";
        ctx.fillRect(screenX - cw * 0.5, baseBottom - ch, cw, ch);
        ctx.strokeStyle = "#111827";
        ctx.lineWidth = Math.max(1, cw * 0.08);
        ctx.strokeRect(screenX - cw * 0.5, baseBottom - ch, cw, ch);
      } else if (item.kind === "pickup") {
        const pw = spriteH * 0.28;
        const ph = spriteH * 0.22;
        const bob = Math.sin(now * 0.006) * ph * 0.25;
        ctx.fillStyle = item.obj.type === "ammo" ? "#f59e0b" : "#10b981";
        ctx.fillRect(screenX - pw * 0.5, baseBottom - ph * 1.4 + bob, pw, ph);
      } else if (item.kind === "grenade") {
        const r = Math.max(3, spriteH * 0.08);
        ctx.fillStyle = "#22c55e";
        ctx.beginPath();
        ctx.arc(screenX, baseBottom - spriteH * 0.25, r, 0, Math.PI * 2);
        ctx.fill();
      } else if (item.kind === "particle") {
        const pt = item.obj;
        const pr = Math.max(2, spriteH * pt.size);
        ctx.globalAlpha = Math.max(0, pt.life / pt.maxLife);
        ctx.fillStyle = pt.color;
        ctx.beginPath();
        ctx.arc(screenX, baseBottom - spriteH * pt.z, pr, 0, Math.PI * 2);
        ctx.fill();
        ctx.globalAlpha = 1;
      } else if (item.kind === "enemy") {
        const e = item.obj;
        const pal = SOLDIER_PALETTE[e.kind] || SOLDIER_PALETTE.rifle;
        const eh = spriteH * 0.78 * pal.scale;
        const ew = spriteH * 0.34 * pal.scale;
        const flashHit = now < e.hitUntilMs;

        // Pernas & Tronco
        ctx.fillStyle = flashHit ? "#f8fafc" : pal.body;
        ctx.fillRect(screenX - ew * 0.42, baseBottom - eh * 0.72, ew * 0.84, eh * 0.46);
        ctx.fillRect(screenX - ew * 0.38, baseBottom - eh * 0.26, ew * 0.3, eh * 0.26);
        ctx.fillRect(screenX + ew * 0.08, baseBottom - eh * 0.26, ew * 0.3, eh * 0.26);

        // Capacete / Cabeça + Visor tático
        ctx.fillStyle = flashHit ? "#ffffff" : pal.trim;
        ctx.fillRect(screenX - ew * 0.22, baseBottom - eh * 0.94, ew * 0.44, eh * 0.2);
        ctx.fillStyle = pal.visor;
        ctx.fillRect(screenX - ew * 0.14, baseBottom - eh * 0.86, ew * 0.28, eh * 0.06);

        // Clarão de disparo do inimigo
        if (now < e.muzzleUntilMs) {
          ctx.fillStyle = "#f59e0b";
          ctx.beginPath();
          ctx.arc(screenX + ew * 0.36, baseBottom - eh * 0.56, Math.max(4, ew * 0.35), 0, Math.PI * 2);
          ctx.fill();
        }

        // Barra de vida sobre inimigo ferido
        if (e.hp < e.maxHp) {
          const bw = Math.max(24, ew * 1.1);
          ctx.fillStyle = "rgba(0,0,0,0.75)";
          ctx.fillRect(screenX - bw * 0.5, baseBottom - eh * 1.08, bw, 4);
          ctx.fillStyle = "#ef4444";
          ctx.fillRect(screenX - bw * 0.5, baseBottom - eh * 1.08, bw * (e.hp / e.maxHp), 4);
        }
      }
    }

    // Viewmodel da arma em 1ª pessoa (ou escopo AWP)
    if (this.isAiming() && this.weaponIdx === 2) {
      this.renderSniperScope(ctx, w, h);
    } else {
      this.renderWeaponViewmodel(ctx, w, h, now);
    }

    // Mira central & Hitmarker
    this.renderCrosshair(ctx, w, h, now);

    // Miniradar no canto superior direito do viewport
    this.renderMinimap(ctx, w - 126, 12, 114, 78);

    // Vinheta de dano
    if (this.player.damageFlash > 0) {
      ctx.fillStyle = `rgba(220, 38, 38, ${(this.player.damageFlash * 0.36).toFixed(2)})`;
      ctx.fillRect(0, 0, w, h);
    }

    // Banner de Setor / Missão Cumprida / Abatido
    if (this.playerDead) {
      ctx.fillStyle = "rgba(11, 12, 14, 0.78)";
      ctx.fillRect(0, 0, w, h);
      ctx.fillStyle = "#ef4444";
      ctx.font = "700 28px 'Chakra Petch', sans-serif";
      ctx.textAlign = "center";
      ctx.fillText("OPERADOR ABATIDO", w * 0.5, h * 0.46);
      ctx.fillStyle = "#f1f3f5";
      ctx.font = "600 13px 'JetBrains Mono', monospace";
      ctx.fillText("Pressione R ou clique em Reiniciar Missão", w * 0.5, h * 0.55);
      ctx.textAlign = "left";
    } else if (now < this.sectorBannerMs) {
      ctx.fillStyle = "rgba(11, 12, 14, 0.72)";
      ctx.fillRect(w * 0.18, 22, w * 0.64, 34);
      ctx.strokeStyle = "#f59e0b";
      ctx.strokeRect(w * 0.18, 22, w * 0.64, 34);
      ctx.fillStyle = "#f59e0b";
      ctx.font = "700 14px 'Chakra Petch', sans-serif";
      ctx.textAlign = "center";
      ctx.fillText(this.sectorBannerText, w * 0.5, 44);
      ctx.textAlign = "left";
    } else if (!this.pointerLocked) {
      ctx.fillStyle = "rgba(11, 12, 14, 0.65)";
      ctx.fillRect(12, h - 36, 370, 24);
      ctx.fillStyle = "#f59e0b";
      ctx.font = "600 11px 'JetBrains Mono', monospace";
      ctx.fillText("CLIQUE NA TELA PARA TRAVAR MIRA 360° (ESC SOLTA)", 20, h - 20);
    }
  }

  renderWeaponViewmodel(ctx, w, h, now) {
    const aimLerp = this.isAiming() ? 1 : 0;
    const bobX = Math.cos(this.player.bobPhase * 0.5) * 8 * this.player.bobAmount * (1 - aimLerp * 0.7);
    const bobY = Math.abs(Math.sin(this.player.bobPhase)) * 8 * this.player.bobAmount * (1 - aimLerp * 0.7);
    const reloadOffset = now < this.reloadingUntilMs ? 65 : 0;
    const kick = this.recoilKick * 24;

    const cx = w * (aimLerp ? 0.5 : 0.72) + bobX;
    const cy = h + bobY + kick + reloadOffset;

    ctx.save();
    ctx.translate(cx, cy);

    // Clarão de disparo
    if (now < this.muzzleFlashMs) {
      ctx.fillStyle = "rgba(245, 158, 11, 0.85)";
      ctx.beginPath();
      ctx.arc(0, -142, 28, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = "#fef3c7";
      ctx.beginPath();
      ctx.arc(0, -142, 12, 0, Math.PI * 2);
      ctx.fill();
    }

    // Corpo da arma (silhueta industrial tática)
    const barrelW = this.weaponIdx === 1 ? 26 : 18;
    ctx.fillStyle = "#1f2937";
    ctx.fillRect(-barrelW * 0.5, -140, barrelW, 95);
    ctx.fillStyle = "#111827";
    ctx.fillRect(-26, -75, 52, 85);
    ctx.fillStyle = "#374151";
    ctx.fillRect(-6, -125, 12, 48);
    // Mira óptica/ferro
    ctx.fillStyle = "#f59e0b";
    ctx.fillRect(-2, -138, 4, 8);
    ctx.restore();
  }

  renderSniperScope(ctx, w, h) {
    ctx.fillStyle = "rgba(0,0,0,0.88)";
    ctx.beginPath();
    ctx.rect(0, 0, w, h);
    ctx.arc(w * 0.5, h * 0.5, Math.min(w, h) * 0.36, 0, Math.PI * 2, true);
    ctx.fill();

    ctx.strokeStyle = "#f59e0b";
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    ctx.moveTo(w * 0.5 - 140, h * 0.5);
    ctx.lineTo(w * 0.5 + 140, h * 0.5);
    ctx.moveTo(w * 0.5, h * 0.5 - 140);
    ctx.lineTo(w * 0.5, h * 0.5 + 140);
    ctx.stroke();
  }

  renderCrosshair(ctx, w, h, now) {
    const cx = w * 0.5;
    const cy = h * 0.5;
    const gap = this.isAiming() ? 4 : 8 + this.recoilKick * 12;

    ctx.strokeStyle = "#f59e0b";
    ctx.lineWidth = 1.6;
    ctx.beginPath();
    ctx.moveTo(cx - gap - 6, cy);
    ctx.lineTo(cx - gap, cy);
    ctx.moveTo(cx + gap, cy);
    ctx.lineTo(cx + gap + 6, cy);
    ctx.moveTo(cx, cy - gap - 6);
    ctx.lineTo(cx, cy - gap);
    ctx.moveTo(cx, cy + gap);
    ctx.lineTo(cx, cy + gap + 6);
    ctx.stroke();

    if (now < this.hitmarkerMs) {
      ctx.strokeStyle = this.hitmarkerKill ? "#ef4444" : "#ffffff";
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(cx - 7, cy - 7);
      ctx.lineTo(cx - 2, cy - 2);
      ctx.moveTo(cx + 7, cy - 7);
      ctx.lineTo(cx + 2, cy - 2);
      ctx.moveTo(cx - 7, cy + 7);
      ctx.lineTo(cx - 2, cy + 2);
      ctx.moveTo(cx + 7, cy + 7);
      ctx.lineTo(cx + 2, cy + 2);
      ctx.stroke();
    }
  }

  renderMinimap(ctx, ox, oy, mw, mh) {
    ctx.fillStyle = "rgba(11, 12, 14, 0.85)";
    ctx.fillRect(ox, oy, mw, mh);
    ctx.strokeStyle = "#232936";
    ctx.strokeRect(ox, oy, mw, mh);

    const cw = mw / this.mapW;
    const ch = mh / this.mapH;
    for (let y = 0; y < this.mapH; y++) {
      for (let x = 0; x < this.mapW; x++) {
        if (this.cellAt(x, y) !== ".") {
          ctx.fillStyle = "#374151";
          ctx.fillRect(ox + x * cw, oy + y * ch, Math.ceil(cw), Math.ceil(ch));
        }
      }
    }

    // Barris explosivos
    ctx.fillStyle = "#ef4444";
    for (const b of this.barrels) {
      if (b.alive) ctx.fillRect(ox + b.x * cw - 1, oy + b.y * ch - 1, 2.5, 2.5);
    }

    // Inimigos
    ctx.fillStyle = "#fb7185";
    for (const e of this.enemies) {
      if (e.alive) {
        ctx.beginPath();
        ctx.arc(ox + e.x * cw, oy + e.y * ch, 2.3, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // Jogador + direção
    const px = ox + this.player.x * cw;
    const py = oy + this.player.y * ch;
    ctx.fillStyle = "#f59e0b";
    ctx.beginPath();
    ctx.arc(px, py, 2.8, 0, Math.PI * 2);
    ctx.fill();
    ctx.strokeStyle = "#f59e0b";
    ctx.beginPath();
    ctx.moveTo(px, py);
    ctx.lineTo(px + Math.cos(this.player.yaw) * 8, py + Math.sin(this.player.yaw) * 8);
    ctx.stroke();
  }

  renderTopDown(ctx, w, h) {
    ctx.fillStyle = "#0b0c0e";
    ctx.fillRect(0, 0, w, h);
    this.renderMinimap(ctx, 20, 20, w - 40, h - 40);
  }
}
