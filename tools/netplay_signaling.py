"""Sinalização + relay local de salas (mesmo protocolo de server/worker.js), sem dependências externas.

Permite que duas sessões de navegadores/perfis diferentes (ou PCs na mesma rede) se encontrem usando só o
`tools/serve_web.py`, sem Cloudflare. Endpoints: GET /health e WebSocket /room/<CÓDIGO>/ws.
"""

from __future__ import annotations

import base64
import hashlib
import json
import re
import struct
import threading

MAX_PLAYERS = 4
MAX_PACKET_BYTES = 1200
_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ROOM_PATH = re.compile(r"^/room/([A-Za-z0-9]{6})/ws$")


class _Peer:
    def __init__(self, sock_file, peer_id: int, role: str) -> None:
        self.f = sock_file
        self.id = peer_id
        self.role = role
        self.lock = threading.Lock()
        self.alive = True

    def send_text(self, obj: dict) -> None:
        data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        n = len(data)
        head = bytearray([0x81])
        if n < 126:
            head.append(n)
        elif n < 65536:
            head += bytes([126]) + struct.pack(">H", n)
        else:
            head += bytes([127]) + struct.pack(">Q", n)
        with self.lock:
            if not self.alive:
                return
            try:
                self.f.write(bytes(head) + data)
                self.f.flush()
            except OSError:
                self.alive = False


class _Room:
    def __init__(self) -> None:
        self.peers: dict[int, _Peer] = {}
        self.host_id: int | None = None
        self.next_id = 1


_rooms: dict[str, _Room] = {}
_rooms_lock = threading.Lock()


def is_signaling_path(path: str) -> bool:
    return path == "/health" or bool(ROOM_PATH.match(path))


def _read_exact(f, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def _read_message(f, peer: _Peer) -> str | None:
    """Lê uma mensagem de texto; trata ping/close. Devolve None ao fechar."""
    text = b""
    while True:
        b1, b2 = _read_exact(f, 2)
        fin, opcode = b1 & 0x80, b1 & 0x0F
        masked, ln = b2 & 0x80, b2 & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", _read_exact(f, 2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", _read_exact(f, 8))[0]
        if ln > 65536:
            return None
        mask = _read_exact(f, 4) if masked else b""
        payload = _read_exact(f, ln) if ln else b""
        if masked:
            payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        if opcode == 0x8:
            return None
        if opcode == 0x9:  # ping -> pong
            with peer.lock:
                peer.f.write(bytes([0x8A, len(payload)]) + payload[:125])
                peer.f.flush()
            continue
        if opcode == 0xA:
            continue
        if opcode in (0x1, 0x0):
            text += payload
            if fin:
                return text.decode("utf-8", "replace")


def handle_http(handler) -> bool:
    """Atende /health e o upgrade WebSocket. Devolve True se a requisição foi tratada."""
    path = handler.path.split("?", 1)[0]
    if path == "/health":
        body = json.dumps({"ok": True, "service": "black-enhanced-netplay", "max_players": MAX_PLAYERS}).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "application/json; charset=utf-8")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
        return True
    m = ROOM_PATH.match(path)
    if not m:
        return False
    key = handler.headers.get("Sec-WebSocket-Key")
    if handler.headers.get("Upgrade", "").lower() != "websocket" or not key:
        handler.send_error(426, "Expected WebSocket upgrade")
        return True
    code = m.group(1).upper()
    with _rooms_lock:
        room = _rooms.setdefault(code, _Room())
        if len(room.peers) >= MAX_PLAYERS:
            handler.send_error(403, "Room full (max 4 players)")
            return True
        accept = base64.b64encode(hashlib.sha1(key.encode() + _GUID).digest()).decode()
        handler.wfile.write(
            (
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode()
        )
        handler.wfile.flush()
        peer_id = room.next_id
        room.next_id += 1
        is_host = room.host_id is None
        if is_host:
            room.host_id = peer_id
        peer = _Peer(handler.wfile, peer_id, "host" if is_host else "client")
        room.peers[peer_id] = peer
        peer.send_text(
            {
                "type": "assigned",
                "peer_id": peer_id,
                "role": peer.role,
                "host_peer_id": room.host_id,
                "players_in_room": len(room.peers),
            }
        )
        if not is_host and room.host_id in room.peers:
            room.peers[room.host_id].send_text({"type": "peer_joined", "peer_id": peer_id})

    handler.close_connection = True
    try:
        while True:
            raw = _read_message(handler.rfile, peer)
            if raw is None:
                break
            _on_message(room, peer, raw)
    except (EOFError, OSError):
        pass
    finally:
        _on_disconnect(code, room, peer)
    return True


def _on_message(room: _Room, sender: _Peer, raw: str) -> None:
    if len(raw) > 8192:
        return
    try:
        msg = json.loads(raw)
        target = room.peers.get(int(msg.get("to")))
    except (ValueError, TypeError):
        return
    if target is None:
        return
    kind = msg.get("type")
    if kind in ("sdp", "ice"):
        target.send_text({"type": kind, "from": sender.id, "payload": msg.get("payload")})
    elif kind == "relay":
        hx = msg.get("hex")
        if isinstance(hx, str) and len(hx) <= MAX_PACKET_BYTES * 2:
            ch = "unreliable" if msg.get("channel") == "unreliable" else "reliable"
            target.send_text({"type": "relay", "from": sender.id, "channel": ch, "hex": hx})


def _on_disconnect(code: str, room: _Room, peer: _Peer) -> None:
    peer.alive = False
    with _rooms_lock:
        if peer.id not in room.peers:
            return
        del room.peers[peer.id]
        if peer.id == room.host_id:
            room.host_id = None
            for p in list(room.peers.values()):
                p.send_text({"type": "host_left"})
            room.peers.clear()
        elif room.host_id in room.peers:
            room.peers[room.host_id].send_text({"type": "peer_left", "peer_id": peer.id})
        if not room.peers:
            _rooms.pop(code, None)
