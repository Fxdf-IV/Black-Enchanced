# Black Enhanced Online

Projeto de recompilação estática e multiplayer online (**Deathmatch até 4 jogadores**, iniciando nativo no Windows/Direct3D 12 com trilha futura de pesquisa para navegador em WebAssembly/WebGPU/WebRTC) construído para **BLACK** (Criterion Games, Xbox 2006, NTSC-U).

> [!IMPORTANT]
> **Este repositório não contém nenhum arquivo do jogo nem código gerado a partir do executável original.** Para executar as fases de engenharia reversa, recompilação e combate local (Fases 0–3), você precisa fornecer sua própria cópia legal de **BLACK (Xbox, versão NTSC-U)** em `assets/`.
>
> **Nota sobre a grafia:** O projeto chama-se **Black Enhanced Online**; o nome da pasta e do repositório remoto (`Black-Enchanced`) é mantido apenas por compatibilidade de URL do Git.

---

## Estado real do projeto (transparência técnica)

### 1. O que existe e foi compilado/testado hoje (sem exigir `default.xbe`)
* **Base estrutural limpa (`sdk/`):** submódulo fixado em [`TGP482/XboxXBERecomp`](https://github.com/TGP482/XboxXBERecomp) (licença **MIT**, commit `cc9edda3a8`), integrado ao `CMakeLists.txt`, `build.bat` e `release.bat`. Zero assets e zero binários proprietários versionados (`tools/check_repo_hygiene.py`).
* **Núcleo de rede independente (`libs/netcore` — C++20):** protocolo binário completo (15 tipos de mensagens, magic `0xB1AC`), `ByteReader`/`ByteWriter` com validação contra `NaN`/`Inf`, coordenadas inválidas e flood, máquinas de estado `HostSession` / `ClientSession` para Deathmatch de 4 jogadores e suíte de 9 testes unitários + fuzzing determinístico de 10.000 pacotes rodando sem depender do motor do jogo.
* **Ferramentas offline do recompilador e extração XISO:** [`tools/recomp_black.py`](tools/recomp_black.py) compila localmente `build/compiler/xbe_recomp.exe` e `build/compiler/xbe_symbols.exe` a partir de `sdk/tools/` (via `zig c++`), validando `config/game.toml` + `sdk/xdk/5849/layout.toml`, e [`tools/xiso_extract.py`](tools/xiso_extract.py) extrai imagens XDVDFS/Redump de Xbox.

### 2. O que é esqueleto / hipótese aguardando o `default.xbe` real (ainda não ligado ao SDK nem executado no jogo)
* [`src/game/black_addrs.h`](src/game/black_addrs.h), [`src/game/hooks.cpp`](src/game/hooks.cpp), [`src/mp/mp_bridge.cpp`](src/mp/mp_bridge.cpp) e [`config/game.toml`](config/game.toml): rascunhos de integração construídos a partir de endereços e nomes publicados sob licença MIT por [`MuriloDagustin/black-seamless-coop`](https://github.com/MuriloDagustin/black-seamless-coop).
* Todos esses endereços e deslocamentos estão classificados como **Confiança Baixa (hipótese)** em [`docs/re-log.md`](docs/re-log.md): **ainda não foram linkados ao runtime D3D12 do SDK nem testados contra o `default.xbe` real** (dependem das Fases 0 a 3 no Windows com Visual Studio 2022 + cópia de `BLACK` em `assets/`).
* A campanha original inicia sempre em `mp::Role::OfflineCampaign` por padrão para não interferir no jogo solo quando compilado.

### 3. Protótipo de transporte web (`web/` + `server/` — **não executa o jogo no navegador**)
* [`web/`](web/index.html) e [`server/`](server/README.md) formam uma **bancada de teste P2P** em JavaScript: testam o envio do protocolo binário `0xB1AC` (`libs/netcore`) via `RTCDataChannel` (`reliable` + `unreliable`) entre abas (`BroadcastChannel`) ou via Cloudflare Worker + Durable Object, além de validar localmente a presença e o hash SHA-256 de um `default.xbe` real (`Title ID 0x45410083`, `> 1 MB`) em **OPFS** sem envio para servidor.
* **Não há execução de BLACK (3D nem WASM) no navegador hoje.** O port para navegador é a **Fase 6** (trilha de pesquisa), que só será iniciada após as **Fases 0–3** comprovarem controle de entidades e combate local no PC nativo.

### 4. Documentação de engenharia e pesquisa
* [`docs/PLANO_DE_IMPLEMENTACAO.md`](docs/PLANO_DE_IMPLEMENTACAO.md): Plano de Implementação alinhado à ordem estrita PC primeiro -> Rede -> Navegador e às fronteiras de licenciamento (`XboxXBE-Recomp` MIT vs `BLACK-XBERecomp` sem licença visível vs `BLACKReloadedMod` fechado).
* [`docs/re-log.md`](docs/re-log.md): Registro de engenharia reversa com a escala oficial de confiança (Alta / Média / Baixa), origem de cada hipótese e respostas verificadas no `sdk/`.
* [`docs/browser-architecture.md`](docs/browser-architecture.md): Trilha de pesquisa para WASM (`wasm32` arena 256 MB vs `wasm64`) + `gpu::Backend` sobre WebGPU (incluindo os desafios assíncronos de `Sync`, `SyncLate`, fences e relatórios de visibilidade).

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
