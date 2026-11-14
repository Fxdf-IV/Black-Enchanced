#!/usr/bin/env python3
"""Servidor HTTP local para o cliente Web de Black Enhanced Online (web/).

Envia obrigatoriamente os cabeçalhos:
  - Cross-Origin-Opener-Policy: same-origin
  - Cross-Origin-Embedder-Policy: require-corp
Sem eles, o navegador desabilita SharedArrayBuffer (necessário para pthreads no WebAssembly).
"""

from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class CoopCoepHandler(SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve web/ with COOP/COEP headers for WASM + WebGPU + WebRTC.")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--host", type=str, default="127.0.0.1", help="Bind address (default: 127.0.0.1)")
    args = parser.parse_args()

    web_dir = Path(__file__).resolve().parent.parent / "web"
    handler = partial(CoopCoepHandler, directory=str(web_dir))
    server = ThreadingHTTPServer((args.host, args.port), handler)
    print(f"[serve_web] Servindo {web_dir} em http://{args.host}:{args.port} (COOP/COEP ativos)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[serve_web] Encerrado.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
