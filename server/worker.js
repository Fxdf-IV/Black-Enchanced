// Black Enhanced Online — Signaling & Relay Server (Cloudflare Worker + Durable Object)
// Compatível com o plano gratuito do Cloudflare Workers.
// Responsabilidades (Seção 9 do plano):
// 1. Criar salas privadas por código de 6 caracteres (até 4 jogadores: 1 Host + 3 Clientes).
// 2. Encaminhar ofertas/respostas SDP e candidatos ICE do WebRTC entre clientes e o host.
// 3. Servir de relay de pacotes netcore ( <= 1200 B ) quando NAT estrito impedir WebRTC direto.

const MAX_PLAYERS_PER_ROOM = 4;
const MAX_PACKET_BYTES = 1200;
const ROOM_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

function corsHeaders(origin) {
  return {
    "Access-Control-Allow-Origin": origin || "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
  };
}

function randomRoomCode() {
  const bytes = new Uint8Array(6);
  crypto.getRandomValues(bytes);
  let out = "";
  for (let i = 0; i < 6; i++) {
    out += ROOM_ALPHABET[bytes[i] % ROOM_ALPHABET.length];
  }
  return out;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const origin = request.headers.get("Origin") || "*";

    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: corsHeaders(origin) });
    }

    if (url.pathname === "/health") {
      return Response.json(
        { ok: true, service: "black-enhanced-netplay", max_players: MAX_PLAYERS_PER_ROOM },
        { headers: corsHeaders(origin) }
      );
    }

    if (url.pathname === "/room/create" && request.method === "POST") {
      const code = randomRoomCode();
      return Response.json({ room: code }, { headers: corsHeaders(origin) });
    }

    const match = url.pathname.match(/^\/room\/([A-Z2-9]{6})\/ws$/);
    if (match) {
      const code = match[1];
      const id = env.ROOMS.idFromName(code);
      const stub = env.ROOMS.get(id);
      return stub.fetch(request);
    }

    return new Response("Not Found", { status: 404, headers: corsHeaders(origin) });
  },
};

export class RoomSession {
  constructor(state) {
    this.state = state;
    this.peers = new Map(); // peerId -> { ws, role, name }
    this.hostPeerId = null;
    this.nextPeerId = 1;
  }

  async fetch(request) {
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected WebSocket upgrade", { status: 426 });
    }

    if (this.peers.size >= MAX_PLAYERS_PER_ROOM) {
      return new Response("Room full (max 4 players)", { status: 403 });
    }

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.accept();

    const peerId = this.nextPeerId++;
    const isHost = this.hostPeerId === null;
    if (isHost) {
      this.hostPeerId = peerId;
    }

    this.peers.set(peerId, { ws: server, role: isHost ? "host" : "client" });

    server.send(
      JSON.stringify({
        type: "assigned",
        peer_id: peerId,
        role: isHost ? "host" : "client",
        host_peer_id: this.hostPeerId,
        players_in_room: this.peers.size,
      })
    );

    if (!isHost && this.hostPeerId !== null) {
      const host = this.peers.get(this.hostPeerId);
      if (host) {
        host.ws.send(JSON.stringify({ type: "peer_joined", peer_id: peerId }));
      }
    }

    server.addEventListener("message", (event) => {
      this.onMessage(peerId, event.data);
    });

    const cleanup = () => {
      this.onDisconnect(peerId);
    };
    server.addEventListener("close", cleanup);
    server.addEventListener("error", cleanup);

    return new Response(null, { status: 101, webSocket: client });
  }

  onMessage(senderId, raw) {
    if (typeof raw !== "string" || raw.length > 8192) return;
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }

    // Encaminhamento de sinalização WebRTC (SDP offer/answer, ICE candidate) e fallback relay
    const targetId = Number(msg.to);
    const target = this.peers.get(targetId);
    if (!target) return;

    if (msg.type === "sdp" || msg.type === "ice") {
      target.ws.send(
        JSON.stringify({
          type: msg.type,
          from: senderId,
          payload: msg.payload,
        })
      );
    } else if (msg.type === "relay") {
      if (typeof msg.hex === "string" && msg.hex.length <= MAX_PACKET_BYTES * 2) {
        target.ws.send(
          JSON.stringify({
            type: "relay",
            from: senderId,
            channel: msg.channel === "unreliable" ? "unreliable" : "reliable",
            hex: msg.hex,
          })
        );
      }
    }
  }

  onDisconnect(peerId) {
    if (!this.peers.has(peerId)) return;
    this.peers.delete(peerId);

    // Sem migração de host no MVP: se o host sai, a sala é encerrada
    if (peerId === this.hostPeerId) {
      this.hostPeerId = null;
      for (const [, p] of this.peers) {
        try {
          p.ws.send(JSON.stringify({ type: "host_left" }));
          p.ws.close(1000, "Host left room");
        } catch {}
      }
      this.peers.clear();
      return;
    }

    if (this.hostPeerId !== null) {
      const host = this.peers.get(this.hostPeerId);
      if (host) {
        try {
          host.ws.send(JSON.stringify({ type: "peer_left", peer_id: peerId }));
        } catch {}
      }
    }
  }
}
