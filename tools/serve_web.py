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
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class EnhancedWebHandler(SimpleHTTPRequestHandler):
    data_dir: Path | None = None
    cdn_url: str = ""

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self) -> None:
        if self.path == "/api/manifest":
            self.handle_manifest()
            return
        if self.path.startswith("/game_assets/"):
            self.handle_game_asset()
            return
        super().do_GET()

    def handle_manifest(self) -> None:
        files_info: list[dict[str, int | str]] = []
        total_bytes = 0
        available = False

        if self.data_dir and self.data_dir.is_dir():
            for root, _, files in os.walk(self.data_dir):
                for f in files:
                    if f.startswith("."):
                        continue
                    full_p = Path(root) / f
                    rel_p = str(full_p.relative_to(self.data_dir)).replace("\\", "/")
                    sz = full_p.stat().st_size
                    total_bytes += sz
                    files_info.append({"path": rel_p, "size": sz})
            available = len(files_info) > 0

        payload = {
            "available": available,
            "total_bytes": total_bytes,
            "file_count": len(files_info),
            "files": files_info,
            "cdn_url": self.cdn_url,
        }
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_game_asset(self) -> None:
        if not self.data_dir or not self.data_dir.is_dir():
            self.send_error(404, "Data directory not configured")
            return
        rel = self.path[len("/game_assets/") :].split("?")[0]
        # Previne directory traversal
        target = (self.data_dir / rel).resolve()
        if not str(target).startswith(str(self.data_dir.resolve())) or not target.is_file():
            self.send_error(404, "Asset not found")
            return

        size = target.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
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
    if not data_dir_path.exists():
        data_dir_path = None

    EnhancedWebHandler.data_dir = data_dir_path
    EnhancedWebHandler.cdn_url = args.cdn_url

    handler = partial(EnhancedWebHandler, directory=str(web_dir))
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
