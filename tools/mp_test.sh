#!/usr/bin/env bash
# Teste local de multiplayer: sobe o servidor e abre DOIS navegadores com perfis
# separados (Host e Cliente), provando que a sala funciona entre sessões distintas.
# Uso: tools/mp_test.sh [porta] [pasta-do-jogo]
set -euo pipefail
PORT="${1:-8090}"
DATA="${2:-$(cd "$(dirname "$0")/.." && pwd)/assets/BLACK/game}"
ROOM="$(python3 -c 'import random,string;print("".join(random.choices("ABCDEFGHJKLMNPQRSTUVWXYZ23456789",k=6)))')"
BROWSER="${BROWSER:-brave-browser-stable}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FLAGS=(--enable-unsafe-webgpu --ozone-platform=x11 --use-angle=vulkan
       --enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan
       --no-first-run --no-default-browser-check)

if ! curl -fs -m 2 "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
  nohup python3 "$ROOT/tools/serve_web.py" "$PORT" --data "$DATA" >/tmp/black_serve.log 2>&1 &
  for _ in $(seq 20); do curl -fs -m 1 "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.5; done
fi

echo "Sala de teste: $ROOM"
# Janelas separadas, lado a lado, cada uma com perfil próprio (não compartilham BroadcastChannel).
"$BROWSER" "${FLAGS[@]}" --user-data-dir=/tmp/black-mp-host --remote-debugging-port=9301   --new-window --window-position=0,0     --window-size=960,800 "http://127.0.0.1:$PORT/?host=$ROOM" >/dev/null 2>&1 &
sleep 2
"$BROWSER" "${FLAGS[@]}" --user-data-dir=/tmp/black-mp-client --remote-debugging-port=9302 --new-window --window-position=960,0   --window-size=960,800 "http://127.0.0.1:$PORT/?room=$ROOM" >/dev/null 2>&1 &
echo "Host: janela da esquerda (clique em Host/Iniciar). Cliente: janela da direita (?room=$ROOM)."
