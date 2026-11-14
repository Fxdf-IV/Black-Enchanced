# Black Enhanced Online

Projeto de recompilação estática e multiplayer online (**Deathmatch até 4 jogadores**, nativo no Windows e com meta final em **WebAssembly + WebGPU + WebRTC no navegador**) construído para **BLACK** (Criterion Games, Xbox 2006, NTSC-U).

> [!IMPORTANT]
> **Este repositório não contém nenhum arquivo do jogo nem código gerado a partir do executável original.** Para compilar e jogar o jogo completo, você precisa fornecer sua própria cópia legal de **BLACK (Xbox, versão NTSC-U)** na pasta `assets/`.

---

## O que já está pronto neste repositório

1. **Base de Recompilação Limpa (MIT):**
   * Submódulo `sdk/` fixado em [`TGP482/XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) (`cc9edda3a8`), integrado ao `CMakeLists.txt`, `build.bat` e `release.bat`.
   * Ponte `src/mp/` e gancho inicial de quadro (`sub_0020F6A0`) em `src/game/hooks.cpp`, preservando a campanha original (`OfflineCampaign`) por padrão.
2. **Núcleo de Rede Independente (`libs/netcore` — C++20):**
   * Protocolo binário completo (15 tipos de mensagens) com serialização segura (`ByteReader`/`ByteWriter`), verificação de versão e hash FNV-1a das regras da partida.
   * Validação estrita contra entradas hostis ou malformadas (rejeita `NaN`/`Inf`, coordenadas fora dos limites, pacotes truncados ou com bytes excedentes, auto-tiro e IDs duplicados).
   * Máquina de estados de **Host Autoritativo (`HostSession`)** e **Cliente (`ClientSession`)** para Deathmatch de até 4 jogadores, com validação de cadência, alcance, dano, morte, respawn automático, placar, limite de eliminações/tempo e expulsão automática (`Kick`) por flood ou pacotes inválidos.
   * Interface abstrata `ITransport` pronta para receber backends UDP/ENet (nativo) e `RTCDataChannel` (WebRTC no navegador), acompanhada de `LoopbackEndpoint` em memória.
   * Suíte de testes automatizados (`test_netcore.cpp`) incluindo fuzzing determinístico de 10.000 pacotes e simulação completa de partida Deathmatch de 4 jogadores.
3. **Mapeamento Reverso e Arquitetura Web Documentados:**
   * [`docs/PLANO_DE_IMPLEMENTACAO.md`](docs/PLANO_DE_IMPLEMENTACAO.md): Plano de implementação v2 atualizado com as evidências de `black-seamless-coop` (MIT), `BLACKReloadedMod` e `cricket07-browser` (MIT).
   * [`docs/re-log.md`](docs/re-log.md): Registro de engenharia reversa com 19 endereços `sub_XXXXXXXX` verificados do `default.xbe` (quadro, projéteis, spawn de aliados/inimigos, cérebro de IA, morte/respawn e câmera).
   * [`docs/browser-architecture.md`](docs/browser-architecture.md): Especificação técnica do port para navegador usando a interface `gpu::Backend` (`sdk/runtime/gpu/backend.h`) sobre WebGPU, Web Audio, OPFS e WebRTC.

---

## Estrutura do Projeto

```text
├── sdk/                  # XboxXBE-Recomp (MIT), submódulo fixado
├── src/
│   ├── common/           # game_info.cpp (configuração do executável e callbacks)
│   ├── game/             # hooks.cpp (ganchos X86_FUNC sobre o default.xbe)
│   └── mp/               # mp_bridge.h/.cpp (ponte entre o motor recompilado e libs/netcore)
├── libs/
│   └── netcore/          # Protocolo, regras, sessões Host/Client, transporte e testes unitários
├── config/               # game.toml (configuração de símbolos do XBE)
├── data/                 # rules_dm.toml (balanceamento e spawns do Deathmatch)
├── tools/                # check_repo_hygiene.py e hook pre-commit contra envio de assets
├── docs/                 # Plano v2, registro de engenharia reversa (re-log) e arquitetura web
└── assets/               # (Ignorado pelo Git) Coloque aqui sua cópia de BLACK (Xbox NTSC-U)
```

---

## Como testar o núcleo multiplayer (`libs/netcore`) sem os arquivos do jogo

O `libs/netcore` não depende do motor nem do `default.xbe` e roda em qualquer compilador C++20 (Linux, Windows ou macOS):

```bash
cmake -S . -B build -DBLACK_NETCORE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

---

## Como compilar o jogo completo no Windows

1. Clone o repositório com submódulos:
   ```powershell
   git clone --recurse-submodules https://github.com/Fxdf-IV/Black-Enchanced.git
   cd Black-Enchanced
   python tools/install_hooks.py
   ```
2. Copie os arquivos da sua cópia de **BLACK (Xbox, NTSC-U)** para a pasta `assets/` (`default.xbe`, `GlobData.bin` e as pastas `chars`, `data`, `export`, `language`, `levels`, `sound` e `videos`).
3. Com o **Visual Studio 2022** instalado (componentes *Desktop development with C++*, *C++ Clang tools for Windows* e *C++ CMake tools for Windows*), execute:
   ```powershell
   .\build.bat
   ```
   O executável será gerado em `out\build\release\BlackEnhancedOnline.exe`.

---

## Licença

O código próprio deste repositório (`libs/netcore`, `src/`, `tools/`, `docs/`) é distribuído sob a licença **MIT** (veja [`LICENSE`](LICENSE)).
**BLACK** é marca registrada e propriedade da Criterion Games e Electronic Arts. Este projeto é independente e não distribui nenhum arquivo ou código do jogo original.
