#!/usr/bin/env python3
"""Servidor HTTP local para o cliente Web de Black Enhanced Online (web/).

Envia obrigatoriamente os cabeçalhos:
  - Cross-Origin-Opener-Policy: same-origin
  - Cross-Origin-Embedder-Policy: require-corp
Sem eles, o navegador desabilita SharedArrayBuffer (necessário para pthreads no WebAssembly).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

sys.path.insert(0, str(Path(__file__).resolve().parent))
import netplay_signaling
import xiso_extract

REAL_XBE_MIN_BYTES = 1_000_000


class EnhancedWebHandler(SimpleHTTPRequestHandler):
    data_dir: Path | None = None
    wasm_dir: Path | None = None
    cdn_url: str = ""

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self) -> None:
        clean_path = self.path.split("?", 1)[0]
        if netplay_signaling.is_signaling_path(clean_path) and netplay_signaling.handle_http(self):
            return
        if clean_path == "/api/manifest":
            self.handle_manifest()
            return
        if clean_path.startswith("/game_assets/"):
            self.handle_game_asset()
            return
        if clean_path.startswith("/wasm/"):
            self.handle_wasm_file()
            return
        super().do_GET()

    def handle_manifest(self) -> None:
        files_info: list[dict[str, int | str]] = []
        total_bytes = 0
        has_real_xbe = False
        has_iso = False
        wasm_ready = bool(
            self.wasm_dir
            and (self.wasm_dir / "black_enhanced.js").is_file()
            and (self.wasm_dir / "black_enhanced.wasm").is_file()
        )

        if self.data_dir and self.data_dir.is_dir():
            xbe_path = self.data_dir / "default.xbe"
            if xbe_path.is_file() and xbe_path.stat().st_size > REAL_XBE_MIN_BYTES:
                has_real_xbe = True

            for root, _, files in os.walk(self.data_dir):
                for f in files:
                    if f.startswith("."):
                        continue
                    if f.lower().endswith(".iso"):
                        has_iso = True
                        continue
                    full_p = Path(root) / f
                    rel_p = str(full_p.relative_to(self.data_dir)).replace("\\", "/")
                    sz = full_p.stat().st_size
                    total_bytes += sz
                    files_info.append({"path": rel_p, "size": sz})

        available = has_real_xbe and len(files_info) > 0
        payload = {
            "available": available,
            "has_real_xbe": has_real_xbe,
            "has_iso": has_iso,
            "wasm_ready": wasm_ready,
            "total_bytes": total_bytes,
            "file_count": len(files_info),
            "files": files_info if available else [],
            "cdn_url": self.cdn_url,
            "recompiler": {
                "sdk": "TGP482/XboxXBERecomp",
                "title_id": "0x45410083",
                "entry": "0x25A3F",
                "xdk": 5849,
                "partitions": 64,
            },
        }
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_wasm_file(self) -> None:
        if not self.wasm_dir or not self.wasm_dir.is_dir():
            self.send_error(404, "WASM build output not found (out/web/)")
            return
        rel = unquote(self.path[len("/wasm/") :].split("?")[0]).lstrip("/")
        target = (self.wasm_dir / rel).resolve()
        if not str(target).startswith(str(self.wasm_dir.resolve())) or not target.is_file():
            self.send_error(404, "WASM file not found")
            return
        suffix = target.suffix.lower()
        if suffix == ".wasm":
            ctype = "application/wasm"
        elif suffix in (".js", ".mjs"):
            ctype = "text/javascript; charset=utf-8"
        elif suffix == ".json" or suffix == ".map":
            ctype = "application/json; charset=utf-8"
        else:
            ctype = "application/octet-stream"
        size = target.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(target, "rb") as fp:
            while chunk := fp.read(64 * 1024):
                self.wfile.write(chunk)

    def handle_game_asset(self) -> None:
        if not self.data_dir or not self.data_dir.is_dir():
            self.send_error(404, "Data directory not configured")
            return
        rel = unquote(self.path[len("/game_assets/") :].split("?")[0]).lstrip("/")
        # Previne directory traversal
        target = (self.data_dir / rel).resolve()
        if not str(target).startswith(str(self.data_dir.resolve())) or not target.is_file():
            self.send_error(404, "Asset not found")
            return

        size = target.stat().st_size
        range_hdr = self.headers.get("Range", "")
        if range_hdr.startswith("bytes="):
            spec = range_hdr[len("bytes=") :].split(",", 1)[0].strip()
            parts = spec.split("-", 1)
            try:
                start = int(parts[0]) if parts[0] else 0
                end = int(parts[1]) if len(parts) > 1 and parts[1] else size - 1
            except ValueError:
                start, end = 0, size - 1
            start = max(0, min(start, max(0, size - 1)))
            end = max(start, min(end, max(0, size - 1)))
            length = (end - start + 1) if size > 0 else 0
            self.send_response(206 if size > 0 else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(length))
            self.end_headers()
            if length > 0:
                with open(target, "rb") as fp:
                    fp.seek(start)
                    rem = length
                    while rem > 0:
                        chunk = fp.read(min(64 * 1024, rem))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        rem -= len(chunk)
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(target, "rb") as fp:
            while chunk := fp.read(64 * 1024):
                self.wfile.write(chunk)


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve web/ with COOP/COEP headers for WASM + WebGPU + WebRTC.")
    parser.add_argument("port_pos", type=int, nargs="?", default=None, help="HTTP port (optional positional argument)")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Bind address (default: 127.0.0.1)")
    parser.add_argument("--data", type=str, default="", help="Path to BLACK game folder (default: assets/ if populated)")
    parser.add_argument("--cdn-url", type=str, default="", help="Remote CDN URL hosting assets bundle (Cloudflare R2 / S3)")
    args = parser.parse_args()

    port = args.port_pos if args.port_pos is not None else args.port
    repo_root = Path(__file__).resolve().parent.parent
    web_dir = repo_root / "web"

    data_dir_path = Path(args.data) if args.data else (repo_root / "assets")
    if data_dir_path.is_dir():
        xbe_file = data_dir_path / "default.xbe"
        if not xbe_file.is_file() or xbe_file.stat().st_size <= REAL_XBE_MIN_BYTES:
            isos = sorted(p for p in data_dir_path.iterdir() if p.is_file() and p.suffix.lower() == ".iso")
            if isos:
                print(f"[serve_web] ISO encontrada em {isos[0].name}. Extraindo arquivos de BLACK (Xbox) para {data_dir_path}...")
                try:
                    xiso_extract.extract_iso(isos[0], data_dir_path)
                except Exception as exc:
                    print(f"[serve_web] Aviso: falha ao extrair {isos[0].name}: {exc}")
            else:
                print(f"[serve_web] Aviso: nenhum default.xbe real (>1 MB) ou .iso encontrado em {data_dir_path}.")
    else:
        print(f"[serve_web] Aviso: diretório de dados não existe ({data_dir_path}).")

    EnhancedWebHandler.data_dir = data_dir_path
    EnhancedWebHandler.wasm_dir = repo_root / "out" / "web"
    EnhancedWebHandler.cdn_url = args.cdn_url

    handler = partial(EnhancedWebHandler, directory=str(web_dir))
    if os.name == "nt":
        ThreadingHTTPServer.allow_reuse_address = False
    server = ThreadingHTTPServer((args.host, port), handler)
    print(f"[serve_web] Servindo {web_dir} em http://{args.host}:{port} (COOP/COEP ativos)")
    if data_dir_path and data_dir_path.is_dir():
        print(f"[serve_web] Servindo streaming de assets do jogo a partir de: {data_dir_path}")
    if args.cdn_url:
        print(f"[serve_web] CDN remota configurada: {args.cdn_url}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[serve_web] Encerrado.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
