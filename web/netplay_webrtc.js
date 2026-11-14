// Black Enhanced Online — Transporte WebRTC DataChannel + Codec Binário compatível byte-a-byte com libs/netcore
// Magic: 0xB1AC (little-endian: 0xAC, 0xB1), Protocol Version: 1

export const PACKET_MAGIC = 0xb1ac;
export const PROTOCOL_VERSION = 1;
export const DEFAULT_RULES_HASH = 0x424c4b31;

export const MsgType = Object.freeze({
  Hello: 1,
  Welcome: 2,
  Reject: 3,
  MatchStart: 4,
  MatchEnd: 5,
  PlayerState: 6,
  WorldSnapshot: 7,
  FireEvent: 8,
  DamageEvent: 9,
  DeathEvent: 10,
  RespawnEvent: 11,
  Ping: 12,
  Pong: 13,
  Leave: 14,
  Kick: 15,
});

export function encodeHello(name, rulesHash = DEFAULT_RULES_HASH) {
  const clean = (name || "Operator").slice(0, 16);
  const nameBytes = new TextEncoder().encode(clean);
  const buf = new ArrayBuffer(3 + 2 + 4 + 1 + nameBytes.length);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.Hello);
  v.setUint16(3, PROTOCOL_VERSION, true);
  v.setUint32(5, rulesHash >>> 0, true);
  v.setUint8(9, nameBytes.length);
  new Uint8Array(buf, 10).set(nameBytes);
  return new Uint8Array(buf);
}

export function encodeWelcome(playerId, maxPlayers = 4, tickRateHz = 30, snapshotRateHz = 15) {
  const buf = new ArrayBuffer(3 + 6);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.Welcome);
  v.setUint8(3, playerId & 0xff);
  v.setUint8(4, maxPlayers & 0xff);
  v.setUint16(5, tickRateHz, true);
  v.setUint16(7, snapshotRateHz, true);
  return new Uint8Array(buf);
}

export function encodePlayerState(state) {
  const buf = new ArrayBuffer(3 + 43);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.PlayerState);
  v.setUint32(3, state.sequence >>> 0, true);
  v.setUint32(7, state.clientTick >>> 0, true);
  v.setFloat32(11, state.x || 0, true);
  v.setFloat32(15, state.y || 0, true);
  v.setFloat32(19, state.z || 0, true);
  v.setFloat32(23, state.yaw || 0, true);
  v.setFloat32(27, state.pitch || 0, true);
  v.setFloat32(31, state.vx || 0, true);
  v.setFloat32(35, state.vy || 0, true);
  v.setFloat32(39, state.vz || 0, true);
  v.setUint8(43, state.animState || 0);
  v.setUint8(44, state.weapon || 0);
  v.setUint8(45, state.flags ?? 1);
  return new Uint8Array(buf);
}

export function encodeWorldSnapshot(tick, timeLeftS, phase, players) {
  // Cada SnapshotPlayer tem 41 bytes no protocolo C++ (libs/netcore/src/protocol.cpp)
  const count = Math.min(4, players.length);
  const buf = new ArrayBuffer(3 + 8 + count * 41);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.WorldSnapshot);
  v.setUint32(3, tick >>> 0, true);
  v.setUint16(7, timeLeftS & 0xffff, true);
  v.setUint8(9, phase & 0xff);
  v.setUint8(10, count);

  let off = 11;
  for (let i = 0; i < count; i++) {
    const p = players[i];
    v.setUint8(off + 0, p.playerId & 0xff);
    v.setFloat32(off + 1, p.x || 0, true);
    v.setFloat32(off + 5, p.y || 0, true);
    v.setFloat32(off + 9, p.z || 0, true);
    v.setFloat32(off + 13, p.yaw || 0, true);
    v.setFloat32(off + 17, p.pitch || 0, true);
    v.setFloat32(off + 21, p.vx || 0, true);
    v.setFloat32(off + 25, p.vy || 0, true);
    v.setFloat32(off + 29, p.vz || 0, true);
    v.setUint8(off + 33, p.animState || 0);
    v.setUint8(off + 34, p.weapon || 0);
    v.setUint8(off + 35, p.health ?? 100);
    v.setUint8(off + 36, p.flags ?? 1);
    v.setInt16(off + 37, p.kills || 0, true);
    v.setInt16(off + 39, p.deaths || 0, true);
    off += 41;
  }
  return new Uint8Array(buf);
}

export function encodeFireEvent(ev) {
  const buf = new ArrayBuffer(3 + 31);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.FireEvent);
  v.setUint32(3, ev.clientTick >>> 0, true);
  v.setUint8(7, ev.shooter & 0xff);
  v.setUint8(8, ev.weapon & 0xff);
  v.setFloat32(9, ev.ox || 0, true);
  v.setFloat32(13, ev.oy || 0, true);
  v.setFloat32(17, ev.oz || 0, true);
  v.setFloat32(21, ev.dx ?? 1, true);
  v.setFloat32(25, ev.dy || 0, true);
  v.setFloat32(29, ev.dz || 0, true);
  v.setUint8(33, ev.claimedTarget ?? 0xff);
  return new Uint8Array(buf);
}

export function encodePing(nonce, sentMs) {
  const buf = new ArrayBuffer(3 + 8);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.Ping);
  v.setUint32(3, nonce >>> 0, true);
  v.setUint32(7, sentMs >>> 0, true);
  return new Uint8Array(buf);
}

export function encodePong(nonce, echoMs, hostTick) {
  const buf = new ArrayBuffer(3 + 12);
  const v = new DataView(buf);
  v.setUint16(0, PACKET_MAGIC, true);
  v.setUint8(2, MsgType.Pong);
  v.setUint32(3, nonce >>> 0, true);
  v.setUint32(7, echoMs >>> 0, true);
  v.setUint32(11, hostTick >>> 0, true);
  return new Uint8Array(buf);
}

export function decodePacket(uint8) {
  if (!uint8 || uint8.byteLength < 3 || uint8.byteLength > 1200) {
    return null;
  }
  const v = new DataView(uint8.buffer, uint8.byteOffset, uint8.byteLength);
  if (v.getUint16(0, true) !== PACKET_MAGIC) return null;
  const type = v.getUint8(2);

  if (type === MsgType.Hello && uint8.byteLength >= 10) {
    const protocolVersion = v.getUint16(3, true);
    const rulesHash = v.getUint32(5, true);
    const len = v.getUint8(9);
    if (10 + len !== uint8.byteLength) return null;
    const name = new TextDecoder().decode(new Uint8Array(uint8.buffer, uint8.byteOffset + 10, len));
    return { type, protocolVersion, rulesHash, name };
  }
  if (type === MsgType.Welcome && uint8.byteLength === 9) {
    return {
      type,
      playerId: v.getUint8(3),
      maxPlayers: v.getUint8(4),
      tickRateHz: v.getUint16(5, true),
      snapshotRateHz: v.getUint16(7, true),
    };
  }
  if (type === MsgType.PlayerState && uint8.byteLength === 46) {
    return {
      type,
      sequence: v.getUint32(3, true),
      clientTick: v.getUint32(7, true),
      x: v.getFloat32(11, true),
      y: v.getFloat32(15, true),
      z: v.getFloat32(19, true),
      yaw: v.getFloat32(23, true),
      pitch: v.getFloat32(27, true),
      vx: v.getFloat32(31, true),
      vy: v.getFloat32(35, true),
      vz: v.getFloat32(39, true),
      animState: v.getUint8(43),
      weapon: v.getUint8(44),
      flags: v.getUint8(45),
    };
  }
  if (type === MsgType.WorldSnapshot && uint8.byteLength >= 11) {
    const tick = v.getUint32(3, true);
    const timeLeftS = v.getUint16(7, true);
    const phase = v.getUint8(9);
    const count = v.getUint8(10);
    if (11 + count * 41 !== uint8.byteLength) return null;
    const players = [];
    let off = 11;
    for (let i = 0; i < count; i++) {
      players.push({
        playerId: v.getUint8(off + 0),
        x: v.getFloat32(off + 1, true),
        y: v.getFloat32(off + 5, true),
        z: v.getFloat32(off + 9, true),
        yaw: v.getFloat32(off + 13, true),
        pitch: v.getFloat32(off + 17, true),
        vx: v.getFloat32(off + 21, true),
        vy: v.getFloat32(off + 25, true),
        vz: v.getFloat32(off + 29, true),
        animState: v.getUint8(off + 33),
        weapon: v.getUint8(off + 34),
        health: v.getUint8(off + 35),
        flags: v.getUint8(off + 36),
        kills: v.getInt16(off + 37, true),
        deaths: v.getInt16(off + 39, true),
      });
      off += 41;
    }
    return { type, tick, timeLeftS, phase, players };
  }
  if (type === MsgType.FireEvent && uint8.byteLength === 34) {
    return {
      type,
      clientTick: v.getUint32(3, true),
      shooter: v.getUint8(7),
      weapon: v.getUint8(8),
      ox: v.getFloat32(9, true),
      oy: v.getFloat32(13, true),
      oz: v.getFloat32(17, true),
      dx: v.getFloat32(21, true),
      dy: v.getFloat32(25, true),
      dz: v.getFloat32(29, true),
      claimedTarget: v.getUint8(33),
    };
  }
  if (type === MsgType.Ping && uint8.byteLength === 11) {
    return { type, nonce: v.getUint32(3, true), sentMs: v.getUint32(7, true) };
  }
  if (type === MsgType.Pong && uint8.byteLength === 15) {
    return {
      type,
      nonce: v.getUint32(3, true),
      echoMs: v.getUint32(7, true),
      hostTick: v.getUint32(11, true),
    };
  }
  return { type };
}

// Transporte WebRTC DataChannels (reliable + unreliable) com sinalização via WebSocket (Cloudflare Worker)
// ou BroadcastChannel local (para testar múltiplas abas sem servidor externo).
export class WebRtcTransport {
  constructor({ roomCode, signalingUrl, onEvent, onLog }) {
    this.roomCode = roomCode.toUpperCase();
    this.signalingUrl = signalingUrl || "";
    this.onEvent = onEvent || (() => {});
    this.onLog = onLog || (() => {});
    this.localSignalingId = Math.floor(100000 + Math.random() * 899999);
    this.role = "client";
    this.hostPeerId = null;
    this.peers = new Map(); // remotePeerId -> { pc, reliable, unreliable }
    this.bc = null;
    this.ws = null;
  }

  async start(preferredRole = "auto") {
    if (this.signalingUrl) {
      this.startWebSocketSignaling();
    } else {
      this.startBroadcastChannelSignaling(preferredRole);
    }
  }

  startBroadcastChannelSignaling(preferredRole) {
    this.bc = new BroadcastChannel(`black-enhanced-room-${this.roomCode}`);
    this.role = preferredRole === "host" ? "host" : "client";
    if (this.role === "host") {
      this.hostPeerId = this.localSignalingId;
      this.onLog(`Sala ${this.roomCode} aberta como HOST (ID ${this.localSignalingId}).`);
      this.onEvent({ type: "role", role: "host", peerId: this.localSignalingId });
    } else {
      this.onLog(`Procurando host na sala ${this.roomCode}...`);
      this.bc.postMessage({ type: "join_request", from: this.localSignalingId });
    }

    this.bc.onmessage = async (ev) => {
      const msg = ev.data;
      if (!msg || msg.from === this.localSignalingId) return;

      if (msg.type === "join_request" && this.role === "host") {
        if (this.peers.size >= 3) return;
        await this.initiatePeerConnection(msg.from, true);
      } else if (msg.to === this.localSignalingId) {
        await this.handleSignalingMessage(msg.from, msg);
      }
    };
  }

  startWebSocketSignaling() {
    const wsUrl = `${this.signalingUrl.replace(/\/$/, "")}/room/${this.roomCode}/ws`;
    this.ws = new WebSocket(wsUrl);
    this.ws.onmessage = async (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.type === "assigned") {
        this.localSignalingId = msg.peer_id;
        this.role = msg.role;
        this.hostPeerId = msg.host_peer_id;
        this.onLog(`Conectado à sala ${this.roomCode} como ${this.role.toUpperCase()} (#${this.localSignalingId}).`);
        this.onEvent({ type: "role", role: this.role, peerId: this.localSignalingId });
      } else if (msg.type === "peer_joined" && this.role === "host") {
        await this.initiatePeerConnection(msg.peer_id, true);
      } else if (msg.type === "sdp" || msg.type === "ice") {
        await this.handleSignalingMessage(msg.from, msg);
      }
    };
  }

  sendSignaling(to, type, payload) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ to, type, payload }));
    } else if (this.bc) {
      this.bc.postMessage({ from: this.localSignalingId, to, type, payload });
    }
  }

  async initiatePeerConnection(remoteId, isOfferer) {
    const pc = new RTCPeerConnection({
      iceServers: [{ urls: "stun:stun.l.google.com:19302" }],
    });

    const entry = { pc, reliable: null, unreliable: null };
    this.peers.set(remoteId, entry);

    pc.onicecandidate = (e) => {
      if (e.candidate) {
        this.sendSignaling(remoteId, "ice", e.candidate.toJSON());
      }
    };

    const bindChannel = (dc, kind) => {
      dc.binaryType = "arraybuffer";
      dc.onopen = () => {
        if (entry.reliable?.readyState === "open" && entry.unreliable?.readyState === "open") {
          this.onLog(`WebRTC DataChannels (reliable + unreliable) abertos com par #${remoteId}.`);
          this.onEvent({ type: "connected", peer: remoteId });
        }
      };
      dc.onclose = () => {
        this.onEvent({ type: "disconnected", peer: remoteId });
      };
      dc.onmessage = (ev) => {
        this.onEvent({
          type: "packet",
          peer: remoteId,
          channel: kind,
          payload: new Uint8Array(ev.data),
        });
      };
    };

    if (isOfferer) {
      entry.reliable = pc.createDataChannel("reliable", { ordered: true });
      entry.unreliable = pc.createDataChannel("unreliable", { ordered: false, maxRetransmits: 0 });
      bindChannel(entry.reliable, "reliable");
      bindChannel(entry.unreliable, "unreliable");

      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      this.sendSignaling(remoteId, "sdp", pc.localDescription);
    } else {
      pc.ondatachannel = (ev) => {
        const dc = ev.channel;
        if (dc.label === "unreliable") {
          entry.unreliable = dc;
          bindChannel(dc, "unreliable");
        } else {
          entry.reliable = dc;
          bindChannel(dc, "reliable");
        }
      };
    }

    return entry;
  }

  async handleSignalingMessage(fromId, msg) {
    let entry = this.peers.get(fromId);
    if (!entry) {
      entry = await this.initiatePeerConnection(fromId, false);
    }
    if (msg.type === "sdp") {
      await entry.pc.setRemoteDescription(msg.payload);
      if (msg.payload.type === "offer") {
        const answer = await entry.pc.createAnswer();
        await entry.pc.setLocalDescription(answer);
        this.sendSignaling(fromId, "sdp", entry.pc.localDescription);
      }
    } else if (msg.type === "ice" && msg.payload) {
      try {
        await entry.pc.addIceCandidate(msg.payload);
      } catch {}
    }
  }

  send(peerId, channel, uint8) {
    const entry = this.peers.get(peerId);
    if (!entry) return false;
    const dc = channel === "unreliable" ? entry.unreliable : entry.reliable;
    if (!dc || dc.readyState !== "open") return false;
    dc.send(uint8);
    return true;
  }

  broadcast(channel, uint8) {
    for (const [peerId] of this.peers) {
      this.send(peerId, channel, uint8);
    }
  }

  close() {
    for (const [, entry] of this.peers) {
      try {
        entry.pc.close();
      } catch {}
    }
    this.peers.clear();
    if (this.bc) this.bc.close();
    if (this.ws) this.ws.close();
  }
}
