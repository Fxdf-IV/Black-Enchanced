# Black Enhanced Online

Projeto de recompilação estática e multiplayer online (**Deathmatch até 4 jogadores**, nativo no Windows e com meta final em **WebAssembly + WebGPU + WebRTC no navegador**) construído para **BLACK** (Criterion Games, Xbox 2006, NTSC-U).

> [!IMPORTANT]
> **Este repositório não contém nenhum arquivo do jogo nem código gerado a partir do executável original.** Para compilar e jogar o jogo completo, você precisa fornecer sua própria cópia legal de **BLACK (Xbox, versão NTSC-U)** na pasta `assets/`.

---

## O que já está pronto neste repositório

1. **Base de Recompilação Limpa (MIT) + Controle de Entidades no Mundo (Fases 1–3):**
   * Submódulo `sdk/` fixado em [`TGP482/XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) (`cc9edda3a8`), integrado ao `CMakeLists.txt`, `build.bat` e `release.bat`.
   * Mapa completo de endereços e offsets do `default.xbe` NTSC-U (`0x45410083`, XDK 5849) em [`src/game/black_addrs.h`](src/game/black_addrs.h) e [`config/game.toml`](config/game.toml).
   * Ponte [`src/mp/mp_bridge.cpp`](src/mp/mp_bridge.cpp) e ganchos em [`src/game/hooks.cpp`](src/game/hooks.cpp): leitura do jogador local, aplicação de snapshots em atores remotos (flag cinemático `+0x3A7`, ângulos e vetores de locomoção no `AiController`), desligamento seletivo do cérebro de IA (`sub_001E5F70`), interceptação de morte (`sub_001D54C0` / `sub_001BD360`), respawn in-place sem recarregar a fase e prevenção de crash de colisão no mesmo ponto (`kPool + 0x79B0`), mantendo `OfflineCampaign` intacta por padrão.
2. **Núcleo de Rede Independente (`libs/netcore` — C++20):**
   * Protocolo binário completo (15 tipos de mensagens, magic `0xB1AC`) com serialização segura (`ByteReader`/`ByteWriter`), verificação de versão e hash FNV-1a das regras da partida.
   * Validação estrita contra entradas hostis ou malformadas (rejeita `NaN`/`Inf`, coordenadas fora dos limites, pacotes truncados ou com bytes excedentes, auto-tiro e IDs duplicados).
   * Máquina de estados de **Host Autoritativo (`HostSession`)** e **Cliente (`ClientSession`)** para Deathmatch de até 4 jogadores, com validação de cadência, alcance, dano, morte, respawn automático, placar, limite de eliminações/tempo e expulsão automática (`Kick`) por flood ou pacotes inválidos.
   * Suíte de testes automatizados (`test_netcore.cpp`) incluindo fuzzing determinístico de 10.000 pacotes e simulação completa de partida Deathmatch de 4 jogadores.
3. **Pilha de Navegador (WebRTC DataChannels + OPFS + Sinalização Serverless — Fases 5–6):**
   * [`src/common/gpu_command_stream.h`](src/common/gpu_command_stream.h): Gravador thread-safe de comandos `gpu::Backend` (`Init`, `Clear`, `Draw`, `Copy`, `Present`, `SetGamma`) entre a thread do jogo e o `requestAnimationFrame` da página.
   * [`server/`](server/README.md): Cloudflare Worker + Durable Object (`RoomSession`) para códigos de sala de 6 caracteres, sinalização WebRTC (SDP/ICE) e fallback de relay.
   * [`web/`](web/index.html): Cliente web tático com verificador local de `default.xbe` via **OPFS** (`XBEH` + Title ID `0x45410083` + SHA-256 sem upload), transporte **WebRTC DataChannels** (`reliable` + `unreliable`) falando o protocolo binário `0xB1AC` byte a byte com `libs/netcore`, e arena tática interativa de 4 jogadores.
4. **Mapeamento Reverso e Arquitetura Web Documentados:**
   * [`docs/PLANO_DE_IMPLEMENTACAO.md`](docs/PLANO_DE_IMPLEMENTACAO.md): Plano de implementação v2 atualizado com as evidências de `black-seamless-coop` (MIT), `BLACKReloadedMod` e `cricket07-browser` (MIT).
   * [`docs/re-log.md`](docs/re-log.md): Registro de engenharia reversa com 19 endereços `sub_XXXXXXXX` verificados e tabela completa de offsets de `Character` e `AiController`.
   * [`docs/browser-architecture.md`](docs/browser-architecture.md): Especificação técnica do port para navegador usando a interface `gpu::Backend` (`sdk/runtime/gpu/backend.h`) sobre WebGPU, Web Audio, OPFS e WebRTC.

---

## Estrutura do Projeto

```text
├── sdk/                  # XboxXBE-Recomp (MIT), submódulo fixado
├── src/
│   ├── common/           # game_info.cpp e gpu_command_stream.h (fila thread-safe p/ WebGPU)
│   ├── game/             # black_addrs.h e hooks.cpp (ganchos X86_FUNC sobre o default.xbe)
│   └── mp/               # mp_bridge.h/.cpp (ponte entre o motor recompilado e libs/netcore)
├── libs/
│   └── netcore/          # Protocolo, regras, sessões Host/Client, transporte e testes unitários
├── web/                  # Cliente de navegador (OPFS XBE loader, WebRTC DataChannels, HUD tático)
├── server/               # Cloudflare Worker + Durable Object de sinalização WebRTC (salas 6 chars)
├── config/               # game.toml (configuração completa XDK 5849 e ABI do default.xbe)
├── data/                 # rules_dm.toml (balanceamento e spawns do Deathmatch)
├── tools/                # check_repo_hygiene.py, serve_web.py (COOP/COEP) e hook pre-commit
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

## Como rodar o Console Web (OPFS + WebRTC DataChannels)

Para testar o cliente de navegador com isolamento `COOP`/`COEP` (`SharedArrayBuffer` habilitado), validação local do `default.xbe` via OPFS e multiplayer P2P entre abas ou via Cloudflare Worker:

```powershell
python tools/serve_web.py 8080
```

Abra `http://localhost:8080` em duas ou mais abas (até 4 operadores), mantenha o mesmo código de sala (`VEX401`) e clique em **Entrar na Sala** nas abas clientes para estabelecer os dois `RTCDataChannel`s (`reliable` e `unreliable`) trocando pacotes binários `0xB1AC` (`libs/netcore`) a 30 Hz. Para publicar o servidor de sinalização na internet, consulte [`server/README.md`](server/README.md).

---

## Licença

O código próprio deste repositório (`libs/netcore`, `src/`, `tools/`, `docs/`) é distribuído sob a licença **MIT** (veja [`LICENSE`](LICENSE)).
**BLACK** é marca registrada e propriedade da Criterion Games e Electronic Arts. Este projeto é independente e não distribui nenhum arquivo ou código do jogo original.
