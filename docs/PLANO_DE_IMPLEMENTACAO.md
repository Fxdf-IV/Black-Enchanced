# Black Enhanced Online — Plano de Implementação (v2 Atualizado)

**Atualizado em:** 14 de novembro de 2026 · **Autores:** @Vinicius & Equipe Black Enhanced

> [!IMPORTANT]
> **Objetivo final:** Levar **Black Enhanced Online** a partidas multiplayer de até 4 jogadores **no navegador (WebAssembly + WebGPU + WebRTC)**, mantendo a campanha original intacta e construindo sobre uma base 100% livre de assets ou código proprietário no controle de versão.

---

## 1. Evidências: O que foi verificado e atualizado na v2

Todas as pendências de verificação da v1 foram resolvidas por inspeção direta dos repositórios e pacotes:

| Item | Status na v1 | Status na v2 | O que as fontes mostram agora |
| :--- | :--- | :--- | :--- |
| **`TGP482/XboxXBERecomp` (SDK)** | Verificado (CMake parcial) | **Verificado 100%** | Licença MIT (`cc9edda3a8`). `templates/game` lido integralmente. Além disso, `runtime/gpu/backend.h` define uma interface abstrata `gpu::Backend` de 72 linhas entre `d3d8.cpp` e o renderizador D3D12, facilitando um backend WebGPU direto. |
| **`MuriloDagustin/black-seamless-coop`** | Não listado na v1 | **Verificado (Novo — MIT)** | Mod co-op em rede para até 8 jogadores sobre o port de BLACK, sob **licença MIT**. Documenta e implementa em C++ os ganchos exatos do `default.xbe` (`0x20F6A0` frame, `0x1DB7B0`/`0x1DBD50` balas, `0x1AF880` corpo aliado, `0x1E5F70` AI think, `0x1CCBB0` update, `0x1D54C0` morte do jogador, `0x02B830` câmera). |
| **`kROM3r/BLACKReloadedMod`** | "Não encontrado" | **Verificado** | Modpack (v1.9, nov/2026) em `winmm.dll` para XBE Recomp V3+ e PC port 0.3.0. Prova na prática: corpo em 3ª pessoa com 10 modelos e mira alinhada ao cano (< 1,5°), Squad Mode, Enemy Multiplier (1×–3×), correção de crash de colisão quando dois atores ocupam o mesmo ponto e reamostragem de áudio a 48 kHz com prioridade de thread. |
| **`TGP482/BLACK-XBERecomp`** | Verificado | **Verificado** | Port nativo para PC da versão Xbox. Sem arquivo de licença explícito; usado como baseline comportamental. |
| **`portingpete/Black-2006-Recomp`** | Verificado em parte | **Verificado (MIT)** | Licença MIT; traz ambient occlusion em quatro técnicas e FOV amplo. |
| **`Dhaiwat10/cricket07-browser`** | Verificado | **Verificado (MIT)** | Jogo x86 de 2006 em WebAssembly, D3D8 sobre WebGPU (gravação de comandos na thread do jogo e replay na main thread), DirectSound sobre Web Audio, arquivos do usuário copiados para OPFS via File System Access API e sinalização em Cloudflare Worker + Durable Object. |

---

## 2. Produto e Escopo

O projeto mantém a separação estrita em dois produtos sobre a mesma base técnica:
* **Black Enhanced (Campanha):** campanha original preservada (`mp::Role::OfflineCampaign` é o padrão no boot), melhorias gráficas/desempenho e correções de estabilidade.
* **Black Enhanced Online (Multiplayer):** Deathmatch para até 4 jogadores, com regras, configurações e balanceamento próprios (`data/rules_dm.toml` e `libs/netcore`).

### Posicionamento por Etapa

| Etapa | Plataforma | Posicionamento |
| :--- | :--- | :--- |
| **Protótipo (Fases 0–3)** | Windows (Nativo) | Controle de entidade, combate bidirecional e Deathmatch local/LAN |
| **MVP 0.1 (Fase 4)** | Windows (Internet) | Multiplayer casual entre amigos (até 4 jogadores, host autoritativo) |
| **Web Alpha (Fase 5)** | Navegador (Cliente WebRTC) + Host Nativo | Cliente WebAssembly/WebGPU conectando em partidas hospedadas no PC |
| **Objetivo Final (Fase 6)** | Navegador Completo (WASM + WebGPU + WebRTC) | Partidas completas no navegador (usuário carrega sua própria ISO/pasta via OPFS) |

---

## 3. Decisões de Arquitetura Atualizadas

| Decisão | Escolha | Status | Motivo |
| :--- | :--- | :--- | :--- |
| **Jogo-base** | BLACK (Xbox NTSC-U, XDK 5849) | **Congelada** | Recompilação estática madura e endereços `sub_XXXXXXXX` já mapeados. |
| **Base de código** | `TGP482/XboxXBERecomp` (MIT) em `sdk/` + `libs/netcore` (MIT) | **Congelada** | 100% sob licença MIT no nosso repositório, sem depender de código sem licença. |
| **Modelo de sincronização** | Snapshots com Host Autoritativo (Listen-Server) | **Congelada (Nativo e Web)** | Validado tecnicamente (inclusive para o navegador): BLACK é multithread e usa ponto flutuante x87/SSE, tornando lockstep determinístico inviável para 4 jogadores. |
| **Núcleo de rede (`libs/netcore`)** | C++20 puro, zero dependência do motor ou SO | **Implementado e Testado** | Compila igual no Windows e no Emscripten/WASM; testado em CI sem arquivos do jogo. |
| **Transporte (`ITransport`)** | Interface comum: Loopback (testes), UDP/ENet (nativo) e WebRTC DataChannels (web) | **Congelada** | Dois canais lógicos (`Reliable` e `Unreliable`) mapeiam 1:1 tanto para ENet quanto para WebRTC. |
| **Renderizador Web** | Implementar `gpu::Backend` sobre WebGPU (`runtime/gpu/webgpu.cpp`) | **Definida** | Evita traduzir D3D12 para WebGPU; intercepta diretamente a camada D3D8 do SDK (72 linhas em `gpu/backend.h`). |
| **Distribuição no Navegador** | Assets locais via OPFS + compilação/link local do XBE do usuário | **Definida** | Nenhum arquivo da EA/Criterion nem binário derivado do XBE é hospedado publicamente. |

---

## 4. Estrutura do Repositório (Ativa)

```text
black-enhanced-online/
├── .github/workflows/ci.yml  # CI: verifica higiene do repo + compila e roda netcore_tests
├── sdk/                      # Submódulo TGP482/XboxXBERecomp fixado em cc9edda3a821
├── src/
│   ├── common/               # game_info.cpp (configuração da aplicação e callbacks)
│   ├── game/                 # hooks.cpp (ganchos X86_FUNC sobre o default.xbe)
│   └── mp/                   # mp_bridge.h / mp_bridge.cpp (ponte entre o jogo e libs/netcore)
├── libs/
│   └── netcore/              # Protocolo binário, validação, regras, sessões Host/Client e Loopback
│       ├── include/netcore/  # limits.hpp, buffer.hpp, protocol.hpp, rules.hpp, transport.hpp, session.hpp
│       ├── src/              # protocol.cpp, rules.cpp, session.cpp
│       └── tests/            # test_netcore.cpp (roundtrip, fuzzing 10k pacotes, simulação 4P DM, anti-flood)
├── config/                   # game.toml (template / saída do xbe_symbols)
├── data/                     # rules_dm.toml (regras e armas do Deathmatch)
├── tools/                    # check_repo_hygiene.py, install_hooks.py, hooks/pre-commit
├── docs/                     # PLANO_DE_IMPLEMENTACAO.md, re-log.md, browser-architecture.md, plano-v1/
├── assets/                   # Ignorado no Git (exceto .gitkeep): onde o usuário coloca sua cópia do jogo
└── AGENTS.md                 # Instruções técnicas e regras para agentes e desenvolvedores
```

---

## 5. Roteiro de Execução por Fases

### Fase 0 e Fase 1 — Fundação, Higiene e Mapeamento Reverso (Concluídas no Repositório)
* [x] Estrutura de repositório criada com `sdk/` (MIT) fixado em commit conhecido (`cc9edda3a8`).
* [x] Dúvidas da Seção 5 resolvidas (`add_xbe_image`, `src/game`, `xdk/5849`) e documentadas em `docs/re-log.md`.
* [x] Registro de engenharia reversa (`docs/re-log.md`) preenchido com os 19 endereços verificados de quadro, disparo, spawn de ator/aliado, IA, dano, morte e câmera.
* [x] Gancho inicial de quadro (`sub_0020F6A0`) implementado em `src/game/hooks.cpp` integrado à ponte `src/mp/mp_bridge.cpp`.
* [x] Higiene do repositório automatizada (`tools/check_repo_hygiene.py`, hook `pre-commit` e workflow `.github/workflows/ci.yml`).

### Fase 2 e Fase 3 — Controle de Entidade e Combate Local (Próximo Marco com `default.xbe`)
* Executar `build.bat` com a ISO/arquivos NTSC-U em `assets/` para gerar `config/game.toml` e `generated/`.
* Validar em jogo a matriz de 11 testes da Seção 8 usando os endereços já conhecidos (`sub_001AF880` para instanciar o corpo do jogador remoto, `sub_001E5F70` para desligar a IA dele, `sub_001CCBB0` para aplicar posição/mira do `netcore::SnapshotPlayer` e `sub_001DB7B0`/`sub_001DBD50` para disparar).
* Aplicar a proteção contra colisão em coordenadas idênticas documentada pelo `BLACKReloadedMod`.

### Fase 4 — Deathmatch Nativo para 4 Jogadores (`libs/netcore` + Transporte UDP/ENet)
* O núcleo `libs/netcore` (protocolo, validação contra pacotes malformados, simulação de Deathmatch 4 jogadores, limite de kills/tempo, respawn e kick por flood/violação) **já está implementado e passando nos testes unitários**.
* Plugar `ENetTransport` (implementando `netcore::ITransport`) na ponte `src/mp/mp_bridge.cpp`.

### Fase 5 — Cliente e Transporte Web (`WebRTCDataChannel` + Sinalização)
* Implementar `WebRtcTransport` sobre `ITransport` (canais `Reliable` e `Unreliable`) e servidor leve de sinalização por código de sala (Cloudflare Worker + Durable Object, compatível com a cota gratuita).
* Permitir que clientes conectem por código de sala sem port-forwarding.

### Fase 6 — Port do Runtime para WebAssembly + WebGPU (`Black Enhanced Online no Navegador`)
* Implementar `sdk/runtime/gpu/webgpu.cpp` sobre a interface `gpu::Backend` (traduzindo estados D3D8 para WebGPU com gravação de comandos na thread do jogo e replay no `requestAnimationFrame`).
* Implementar `sdk/runtime/audio/webaudio.cpp` sobre `AudioWorklet` (48 kHz).
* Adaptar `sdk/runtime/kernel/memory.cpp` para `wasm64` (Memory64) ou janela compacta em `wasm32` e carregar os arquivos do disco do próprio jogador via **OPFS (Origin Private File System)**.
