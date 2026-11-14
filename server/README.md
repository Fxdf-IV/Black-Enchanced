# Servidor de Sinalização e Salas — Black Enhanced Online (`server/`)

Implementa a descoberta de salas por código de 6 caracteres (até 4 jogadores: 1 Host + 3 Clientes), troca de ofertas/respostas SDP + candidatos ICE para **WebRTC DataChannels**, e fallback de relay de pacotes `netcore` ($\le 1200\text{ B}$).

## Deploy na Cota Gratuita do Cloudflare Workers

O `worker.js` utiliza Cloudflare Workers + Durable Objects (`new_sqlite_classes`), que estão disponíveis no **plano gratuito (Workers Free)** do Cloudflare. Nenhum dado do jogo trafega pelo Worker além da sinalização de sala.

```bash
cd server
npx wrangler deploy
```

## Execução Local (Sem Conta Cloudflare)

Para testes locais no navegador (duas ou quatro abas conectando via `BroadcastChannel` + `RTCPeerConnection` local), basta rodar o servidor web com cabeçalhos `COOP`/`COEP`:

```bash
python tools/serve_web.py
```
E abrir `http://localhost:8080` em duas a quatro abas do navegador.
