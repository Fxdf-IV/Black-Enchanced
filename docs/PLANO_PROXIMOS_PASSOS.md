# Plano Completo dos Próximos Passos — Black Enhanced Online

Este documento consolida o estado real atual do projeto e o roteiro completo para atingir o objetivo final: **Campanha completa + Multiplayer online jogáveis no navegador usando a nossa versão Enhanced (estilo Reloaded, 100% clean-room)**, mantendo a campanha original intacta quando as melhorias estiverem desligadas.

> [!IMPORTANT]
> **Fronteira Clean-Room (`AGENTS.md`):**
> Todo recurso estilo "Reloaded / Enhanced" é construído de forma limpa a partir do `sdk/` (`XboxXBERecomp`, MIT), da nossa própria engenharia reversa sobre o `assets/default.xbe` NTSC-U do usuário e de referências permissivas explícitas (`black-seamless-coop`, `Black-2006-Recomp` e `cricket07-browser`, MIT). É **estritamente proibido** ler, descompilar ou extrair endereços do `BLACKReloadedMod`.

---

## 1. Linha de Base Atual (Auditada no Repositório)

| Frente | O que já funciona e foi verificado | O que foi implementado mas ainda falta testar ao vivo / concluir |
| :--- | :--- | :--- |
| **Motor Web (`WASM` + `WebGPU` + `AudioWorklet`)** | `default.xbe` rodando na Missão 1 (`Level_00`) a **~58 FPS**, `wb=2`, `0` erros WebGPU, HUD P8 legível, áudio 48 kHz e streaming VFS `Range`/OPFS. | Validar transição e estabilidade WebGPU/áudio da **Campanha inteira** (`Level_01` até o final). |
| **Enhanced ("estilo Reloaded")** | Mira linear 1:1 com mouse + multiplicador ADS (`sub_001BD3F0`), FOV `55°–115°` (`sub_001BB110`), widescreen `16:9`, hitmarker visual (`sub_001E74B0`) e controles de FPS no teclado. | **Persistência de save `IDBFS`** (`/black_assets/_save` em `CMakeLists.txt` + `web/app.js`) e **hitmarker sonoro** recém-adicionados, pendentes de validação ao vivo; faltam refinamentos extras (arsenal/opções avançadas da Campanha, balanço de câmera/FX). |
| **Portal UI/UX (`web/`)** | Layout estilo `nfsu2.xyz` com viewport `16:9` protagonista, separação estrita `Campanha Solo` vs `Multiplayer Online (P2P)`, gaveta técnica recolhida e Fullscreen com `Pointer Lock` só em jogo. | Guiar ou sincronizar entrada na mesma fase pelo fluxo do multiplayer. |
| **Loadout & Autoridade (`libs/netcore`)** | Protocolo `0xB1AC`, economia `$1500`–`$8000`, janela de compra, Loja `B` in-level e compra real da `MP5` verificada no Host (`Level_00`). `netcore_tests` 100% passando (nativo e WASM). | Filtrar/expor dinamicamente na Loja `B` **apenas as armas e equipamentos que existem nativamente carregados naquela fase** (via `WeaponDef[]` em `*kWeaponSystem`), eliminando itens fictícios ou duplicados por `% def_count`. |
| **Multiplayer em Jogo & Rede** | PvP bidirecional validado no executável **Windows D3D12 UDP** (`Level_00`); conexão P2P entre 2 abas locais (`BroadcastChannel` + WebRTC `DataChannel`) e placar `Tab` verificados no Chromium; fallback WebSocket `relay` em `web/netplay_webrtc.js` validado em teste unitário Node. | **Combate PvP real entre 2 abas WebGPU** dentro da `Level_00`; **sincronia visual de arma do ator remoto** (`ConsumeRemoteWeaponApplyRequest` recém-compilado, pendente de teste E2E); **spawn dedicado** (`sub_001AF880`); **explosões** (`sub_001DF830`); deploy de `server/worker.js` na Cloudflare para uso entre PCs distintos; eventual modo **Campanha Co-op**. |

---

## 2. Roteiro Detalhado de Execução

### Fase P1 — Validação Ao Vivo da Base Recém-Compilada (Solo IDBFS + 2 Abas WebGPU)
**Objetivo:** Comprovar no Chromium real (`http://127.0.0.1:8080`) o funcionamento das entregas já compiladas no `out/web/black_enhanced.wasm`.
1. **Campanha Enhanced + `IDBFS`:**
   * Iniciar `Campanha Solo` com modo Enhanced ativo, chegar em `Level_00` (`IN-LEVEL`), gravar progresso/perfil em `/black_assets/_save`, recarregar a página (`Ctrl+Shift+R`) e confirmar restauração automática pelo IndexedDB.
   * Validar FPS (`~58 FPS`, `wb=2`, `0` erros WebGPU), mira 1:1 no mouse, FOV e hitmarker visual + sonoro WebAudio ao atingir inimigos.
2. **Multiplayer em 2 Abas Simultâneas (`Level_00`):**
   * Aba 1 (`Host`) e Aba 2 (`Cliente` via `?room=VEX401`) entrando ambas em `IN-LEVEL` na `Level_00`.
   * Comprar armas distintas na Loja CS (`B`) e verificar nos logs + tela:
     * `mp_bridge: bound player_id=...` e `hooks: remote actor ... equipped eng_wpn=...` (troca visível do modelo 3D da arma na mão do outro jogador).
     * Troca real de tiros, hitmarkers, dano autoritativo, morte, atualização do placar (`Tab`) e respawn in-place sem crash em ambas as abas.

**Critério de aceite P1:** zero erros WebGPU/console, save preservado após reload na Campanha e loop completo de tiro/morte/respawn/troca de arma funcionando entre as 2 abas no navegador.

---

### Fase P2 — Overhaul "estilo Reloaded / Enhanced" na Campanha Inteira (Via Toggle)
**Objetivo:** Garantir que toda a campanha do jogo funcione no navegador com overhaul opcional de gameplay e acabamento moderno de PC, mantendo a campanha vanilla 100% intacta quando o modo Enhanced estiver desligado.
1. **Compatibilidade da Campanha Inteira (`Level_01` → missão final):**
   * Executar e auditar transições de missão, vídeos XMV e carregamento de novos pacotes de textura/áudio em `src/web/gpu_webgpu.cpp` e `web/webgpu_renderer.js`, prevenindo vazamentos de VRAM GPU ou travamentos entre fases.
2. **Overhaul Opcional Clean-Room (Controlado por `common::EnhancedConfig`, via Toggle):**
   * **Conforto Visual & Widescreen (`16:9`):** auditar câmera/projeção (`sub_0002B830`) e proporção de HUD/retícula em resoluções altas; expor controle de escala de resolução interna (`render_percent`) no menu `F10`.
   * **Reequilíbrio Opcional de Gameplay ("Reloaded" Clean-Room):** realizar engenharia reversa própria em `default.xbe` (`generated/game/`) para expor ajustes opcionais no toggle Enhanced:
     * **Dano / Letalidade:** ajuste de multiplicador de dano/HP em campanha.
     * **Recuo, Espalhamento e Câmera:** redução opcional de recuo/spread, camera shake excessivo e atordoamento visual.
     * **Munição & Arsenal:** opções de reserva/munição e acesso/troca de armas nativas da fase na Campanha Enhanced.
   * **Campanha Vanilla Intacta:** com o toggle desligado, `Campanha Solo` preserva comportamento fiel original (mantendo apenas o `IDBFS` para saves).

**Critério de aceite P2:** transição limpa entre missões no navegador, FPS estável (`>= 50–60 FPS`), saves persistidos entre sessões e alternância real entre modo Vanilla e Enhanced.

---

### Fase P3 — Robustez do Multiplayer In-Game: PvP Primeiro + Campanha Co-op na Sequência (`src/game/`, `src/mp/`, `libs/netcore`)
**Objetivo:** Fechar primeiro o PvP Arena com compra de equipamentos nativos do jogo (até 4 jogadores) e, em seguida, habilitar o modo Campanha Co-op contra a IA das missões.
1. **Spawn Dedicado de Jogadores Remotos (`sub_001AF880` / `sub_001DF440`):**
   * Substituir o sequestro de NPCs pré-existentes de `kAliveList` em `EnsureRemoteActorForSlot` (`src/mp/mp_bridge.cpp`) pela criação dedicada a partir do pool/AIController livre do motor (`sub_001AF880`), garantindo visibilidade de 1 a 3 jogadores remotos em qualquer ponto do mapa (essencial também para o Co-op, onde os inimigos da missão devem continuar ativos).
2. **Loja Loadout Somente com Equipamentos Nativos de BLACK (`*kWeaponSystem` / `0x00014090`):**
   * Usar a tabela `WeaponDef[]` carregada em runtime (`*kWeaponSystem`) e `kFnWeaponDefByHash` (`0x00014090`) para disponibilizar na Loja `B` **somente armas e acessórios que existam nativamente no jogo/missão atual**, mantendo o catálogo da loja 100% fiel ao arsenal nativo de BLACK (e mantendo a Campanha normal sem loja).
3. **Sincronia de Explosões e Cenário Destrutível (`sub_001DF830`):**
   * Replicar dano em área e abates por `M203`, `RPG-7`, granadas (`G`) e barris explosivos validados pelo Host autoritativo, removendo a supressão cega atual.
4. **Entrada Coordenada no PvP e Novo Modo Campanha Co-op:**
   * Indicar/sincronizar status da fase selecionada no Host para evitar dessincronia de mapa no lobby.
   * **Etapa 3B — Campanha Co-op (2–4 jogadores vs IA):** após o PvP estar redondo, adicionar seletor de modo na sala (`PvP Arena` vs `Campanha Co-op`), preservando a IA dos inimigos da fase (`sub_001E5F70`), desabilitando fogo amigo por padrão e sincronizando objetivos/subáreas/triggers (`sub_001B73B0`, `sub_001C7620`) com base clean-room inspirada no `black-seamless-coop` (MIT).

**Critério de aceite P3:** até 4 operadores jogando PvP com spawn dedicado, loja contendo apenas equipamentos nativos disponíveis na fase, armas visíveis sincronizadas e testes unitários/fuzzing de toda nova mensagem em `libs/netcore/tests/test_netcore.cpp`; seguido da missão 1 jogável em Co-op contra a IA.

---

### Fase P4 — Multiplayer Online Multi-PC (Reservada para Após Testes Locais)
**Objetivo:** Manter foco total nos testes em abas locais/`localhost` (`BroadcastChannel` + WebRTC direto) durante P1–P3, deixando a publicação pública na internet pronta para ativação quando solicitado.
1. **Pronto para deploy futuro (`server/worker.js`):**
   * O código do Worker (Cloudflare Workers + Durable Objects gratuito) e o fallback WebSocket `relay` em `web/netplay_webrtc.js` permanecem mantidos no repositório, **sem deploy ativo por enquanto** por preferência do usuário.
2. **Ativação futura (quando desejar jogar entre PCs/redes diferentes):**
   * Publicar `server/worker.js` via Wrangler (`cd server && npx wrangler deploy`), configurar a URL `wss://...workers.dev` e validar cenários P2P vs `relay` em redes distintas + fluxo OPFS / `--cdn-url`.

**Critério de aceite P4:** testes multi-aba em `localhost` estáveis em P1–P3 e pipeline de deploy do Worker documentado e verificado em código (`node` unitário de `relay`).

---

### Fase P5 — Paridade Nativa Windows & Regressão Final
**Objetivo:** Concentrar 100% do desenvolvimento e validação até a versão jogável no **Navegador (WASM/WebGPU)**, atualizando o executável nativo Windows (`D3D12` + `UDP`) apenas na etapa final.
1. Recompilar via `.\build.bat` garantindo paridade de `src/game/` e `src/mp/` com Windows (`D3D12` + `XAudio2` + `UdpTransport`).
2. Rodar bateria final completa:
   * `python tools/check_repo_hygiene.py`
   * `ctest --test-dir build --output-on-failure`
   * `node out/build-web/libs/netcore/netcore_tests.js`
   * Smoke E2E no navegador (Campanha Overhaul Multi-Fase com save `IDBFS` + PvP + Co-op em abas locais).

---

## 3. Decisões Congeladas (Confirmadas pelo Usuário em 16/11/2026)

1. **Escopo do Multiplayer:** **PvP Arena com compra inspirada em CS primeiro**, seguido de **Campanha Co-op** contra a IA na sequência (Fase P3).
2. **Restrição do Loadout Multiplayer:** a loja inspirada em CS **só pode vender armas/equipamentos que existam nativamente no jogo** (carregados em `*kWeaponSystem`), mantendo a Campanha normal sem loja.
3. **Nível de "Reloaded / Enhanced":** **Overhaul completo opcional de gameplay (via toggle)**, incluindo além dos confortos de PC (mira 1:1, FOV, widescreen `16:9`, hitmarker visual/sonoro, save `IDBFS`) o reequilíbrio opcional de **dano, recuo, câmera e munição/arsenal**, com a campanha vanilla intacta por padrão.
4. **Rede Online vs Testes Locais:** manter testes **apenas em abas locais / `localhost`** por enquanto, sem publicar o `server/worker.js` na Cloudflare agora.
5. **Plataforma Alvo:** **foco total no Navegador (`WASM/WebGPU`) primeiro**, deixando a atualização e validação do build nativo Windows (`build.bat`) para a **Fase P5 final**.
