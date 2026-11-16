# Instruções e Preferências do Projeto — Black Enhanced Online (`AGENTS.md`)

Este documento define as regras técnicas obrigatórias, padrões arquiteturais e preferências operacionais que qualquer agente de IA ou desenvolvedor deve seguir neste repositório.

---

## 1. Higiene do Repositório e Licenciamento (Tolerância Zero)

1. **Nunca versionar material protegido ou binários gerados:**
   * Jamais adicione ao Git arquivos `.xbe`, `.iso`, `.bin`, conteúdo de `assets/` (exceto `assets/.gitkeep`), código C++ gerado pelo recompilador em `generated/`, pastas de build (`build/`, `out/`), executáveis/bibliotecas (`.exe`, `.dll`, `.lib`, `.pdb`, `.wasm`, `.js` gerado em `out/web/`) ou chaves/credenciais (`coop-online.toml`, `.env`, `.dev.vars`).
   * Antes de qualquer commit (ou ao encerrar entregas de código), rode `python tools/check_repo_hygiene.py`.
2. **Fronteira estrita de licenciamento e engenharia reversa clean-room:**
   * **Proibido extrair endereços, descompilar ou ler arquivos gerados pelo `BLACKReloadedMod` (`kROM3r`):** mod de código fechado/licença restritiva; serve apenas como referência visual externa de ideias possíveis, nunca como fonte técnica.
   * **Caminho limpo obrigatório:** partir do `sdk/` (`TGP482/XboxXBERecomp`, licença MIT) + `sdk/templates/game`, realizar nossa própria engenharia reversa sobre o `default.xbe` fornecido pelo usuário (`generated/game/` + Ghidra + `[xbe] trace`) e consultar apenas referências com licença permissiva explícita (`MuriloDagustin/black-seamless-coop`, `portingpete/Black-2006-Recomp` e `Dhaiwat10/cricket07-browser`, todos MIT). `TGP482/BLACK-XBERecomp` não possui arquivo de licença visível e atua somente como baseline comportamental.
3. **Veracidade técnica estrita (Zero simulação disfarçada):**
   * Nunca relatar teste sintético, unitário (`libs/netcore`, Node mock) ou feature apenas compilada como comportamento verificado no `default.xbe` real ou no navegador WebGPU.
   * Sempre informar exatamente o escopo testado após a última edição (`netcore_tests` nativo/WASM, Windows D3D12 UDP, Navegador WebGPU Solo ou Navegador WebGPU 2+ abas simultâneas).

---

## 2. Arquitetura, Isolamento e Organização de Código

1. **Dois produtos — Campanha Vanilla Intacta por Padrão:**
   * `mp::CurrentRole()` inicia sempre em `mp::Role::OfflineCampaign`.
   * Nenhuma lógica de multiplayer (Loja de Loadout `B`, créditos, janela de compra, placar `Tab`, supressão de morte/IA) pode vazar ou alterar o comportamento da Campanha Solo.
   * Melhorias estilo "Reloaded / Enhanced" (`common::EnhancedConfig` — mira 1:1 no mouse, ADS, FOV, hitmarker visual/sonoro e overhaul opcional de dano, recuo, câmera e munição) devem permanecer estritamente atrás de toggle/configuração do usuário, mantendo a campanha original intacta quando desativadas.
2. **Loadout Multiplayer restrito a equipamentos nativos:**
   * A compra estilo CS no multiplayer é apenas uma inspiração de economia/UI: a Loja (`B`) deve oferecer **exclusivamente armas e equipamentos que existam nativamente em BLACK** (`WeaponDef[]` carregados no jogo), nunca armas fictícias fora do motor.
3. **Foco prioritário no Navegador (`WASM/WebGPU`):**
   * Priorizar execução, testes e paridade de gameplay no alvo Web (`out/build-web` + `web/`) usando testes locais (`BroadcastChannel` / `localhost`) até fechar as fases do roteiro, deixando o deploy de `server/worker.js` na nuvem e a paridade final Windows (`.\build.bat`) para as etapas correspondentes em [docs/PLANO_PROXIMOS_PASSOS.md](docs/PLANO_PROXIMOS_PASSOS.md).
4. **Desacoplamento estanque (`libs/netcore`):**
   * `libs/netcore` é C++20 puro: **não** pode incluir cabeçalhos do `sdk/` (`xdk/`, `runtime/`), do Windows (`<windows.h>`, Winsock) nem do Emscripten.
   * Toda nova mensagem do protocolo `0xB1AC` exige validação de tamanho, tipo, limites numéricos (`std::isfinite`, limites de mapa/velocidade/economia) e teste correspondente + fuzzing em `libs/netcore/tests/test_netcore.cpp`.
5. **Não editar o submódulo `sdk/` sem necessidade comprovada:**
   * Hooks específicos do jogo pertencem a `src/game/`.
   * Código comum de host/input pertence a `src/common/`.
   * Ponte multiplayer e transporte nativo/WASM pertencem a `src/mp/`.
   * Camada de runtime WebAssembly/WebGPU/WebAudio pertence a `src/web/` (respeitando `gpu::Backend` em `sdk/runtime/gpu/backend.h` e `sdk/runtime/audio/audio.h`), frontend em `web/` e sinalização/relay em `server/`.
6. **Sem subpastas de fontes em `src/common/`, `src/game/`, `src/mp/` e `src/web/`:**
   * Tanto `CMakeLists.txt` quanto `sdk/cmake/XbeRecomp.cmake` usam `file(GLOB ...)` de um único nível (`src/*/*.cpp`, `src/web/*.cpp`, `src/${game_dir}/*.cpp`). Mantenha todos os `.cpp`/`.h` em um único nível (a única subpasta existente é `src/web/override/` para overrides de headers em `-BEFORE` no alvo WASM).

---

## 3. Padrões de WebGPU, UI/UX e Persistência no Navegador

1. **Interface "Game-First" (modelo `nfsu2.xyz`):**
   * Priorizar clareza e entrada rápida: viewport `16:9` protagonista, abas distintas para `Campanha Solo` e `Multiplayer Online (P2P)`, e controle imediato em tela cheia.
   * Manter diagnósticos pesados, telemetria WASM/WebGPU, URL de Worker e logs recolhidos na gaveta técnica (`#dev-drawer`); não recriar bancada 2D desconectada do motor real.
   * **Fullscreen e Pointer Lock sem atrito:** aplicar `requestFullscreen()` no container do viewport (`#viewport-wrap`) para que menus/overlays (`F10`, `B` Loadout, `Tab` Placar) funcionem por cima do canvas. Ativar `Pointer Lock` **exclusivamente** em gameplay `inLevel === true`, liberando imediatamente ao abrir qualquer menu.
2. **Desempenho e Qualidade WebGPU (`~60 FPS`):**
   * Nunca introduzir comparações byte-a-byte em structs com padding (`std::memcmp` sobre `TextureDesc`), uploads/recriações redundantes de texturas ou `queue.writeBuffer` em excesso por quadro.
   * Preservar leitura síncrona de paletas `P8`, `VirtualFromPhysical` e shaders WGSL sem erros de validação WebGPU (`0` erros).
3. **Assets e Saves (`OPFS` + `IDBFS`):**
   * Assets protegidos nunca ficam no Git; no navegador usam streaming local (`tools/serve_web.py`), `OPFS` local do jogador (`web/opfs_loader.js`) ou CDN própria (`--cdn-url`).
   * Saves da campanha em `/black_assets/_save` devem persistir via Emscripten `IDBFS` (`-lidbfs.js`) sincronizando de forma leve apenas quando houver alteração.
4. **Multiplayer WebRTC + Relay (`web/netplay_webrtc.js` + `server/worker.js`):**
   * Manter compatibilidade binária byte-a-byte com `libs/netcore` (`0xB1AC`), canais `reliable` + `unreliable`, `BroadcastChannel` para testes locais sem servidor e fallback WebSocket `relay` para redes com NAT restrito via Cloudflare Worker.

---

## 4. Preferências Operacionais para Agentes de IA

1. **Idioma e Estilo de Resposta:**
   * Responder em **português brasileiro** por padrão (acompanhando o idioma pedido pelo usuário).
   * Ser direto, claro e objetivo; evitar elogios automáticos ou explicações longas que não ajudem a decisão. Usar links `file:///` clicáveis ao referenciar arquivos.
2. **Disciplina de Engenharia Reversa e Edição:**
   * Antes de editar `src/game/` ou `src/mp/`, inspecionar o código real recompilado em `generated/game/` e o registro em [docs/re-log.md](docs/re-log.md); nunca inferir ou chutar endereços `sub_XXXXXXXX` ou offsets.
   * Manter escopo cirúrgico, funções pequenas e sem lógica duplicada; documentar o *porquê* não-óbvio de hooks e sincronizações.
3. **Servidores Locais, Cache e Subagentes:**
   * `tools/serve_web.py` envia `Cache-Control: no-store`. Ao validar alterações no navegador após recompilar WASM/JS, garantir que o servidor em execução está servindo os arquivos novos e abrir/recarregar sem depender de aba antiga parada em memória. Evitar deixar processos órfãos ocupando a porta `8080`.
   * Usar **`consult_hermes`** (MCP local configurado na rota gratuita NVIDIA NIM) para segunda opinião técnica em planos amplos, análise de riscos em engenharia reversa e revisões arquiteturais, sempre passando `cwd` e contexto preciso.
   * Usar o subagente **`browser`** (`Model: "flash"`) para validações reais na UI, WebGPU (`FPS`, erros de validação, HUD) e testes multi-aba `WebRTC`; não simular testes de navegador via script quando o teste real de browser for viável.
4. **Decisões, Segurança e Custos:**
   * Nunca expor tokens, chaves ou URLs privadas em arquivos versionados; nunca habilitar faturamento ou serviços pagos sem autorização explícita (preferir sempre cota gratuita, ex. Cloudflare Workers Free).
   * Prosseguir autonomamente em tarefas técnicas e correções seguras; perguntar ao usuário apenas quando houver decisão material de produto/escopo ou ação irreversível.

---

## 5. Como Compilar, Servir e Testar

* **1. Higiene do repositório (obrigatório antes de commits):**
  ```powershell
  python tools/check_repo_hygiene.py
  ```
* **2. Núcleo multiplayer (`libs/netcore` — nativo, não exige `assets/default.xbe`):**
  ```powershell
  cmake -S . -B build -DBLACK_NETCORE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build build --config Release
  ctest --test-dir build --output-on-failure
  ```
* **3. Cliente WebAssembly + testes `netcore` em WASM (exige Emscripten `emsdk` + `generated/game/`):**
  ```powershell
  cmake --build out/build-web
  node out/build-web/libs/netcore/netcore_tests.js
  node --check web/app.js
  node --check web/netplay_webrtc.js
  ```
* **4. Servidor local Web (`COOP`/`COEP` ativos para `SharedArrayBuffer`):**
  ```powershell
  python tools/serve_web.py --host 127.0.0.1 --port 8080
  ```
* **5. Executável nativo Windows (exige VS 2022 Clang/CMake + `assets/default.xbe`):**
  ```powershell
  .\build.bat
  ```

---

## 6. Documentação de Referência no Repositório

* [Plano dos Próximos Passos](docs/PLANO_PROXIMOS_PASSOS.md) — estado atual verificado e roteiro ativo (`Fases P1–P5`).
* [Plano de Implementação v2](docs/PLANO_DE_IMPLEMENTACAO.md) — base histórica (Fases 0–6 originais), licenciamento e matriz inicial de testes.
* [Registro de Engenharia Reversa (`re-log`)](docs/re-log.md) — endereços `sub_XXXXXXXX`, vtables e offsets auditados do `default.xbe`.
* [Arquitetura para Navegador (WASM / WebGPU / WebRTC)](docs/browser-architecture.md) — decisões base do runtime web.
* [Sinalização Cloudflare Worker](server/README.md) — deploy gratuito do servidor de salas/relay.
