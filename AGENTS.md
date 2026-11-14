# Instruções do Projeto — Black Enhanced Online (`AGENTS.md`)

Este arquivo define as regras técnicas, arquiteturais e de higiene obrigatórias para qualquer agente ou desenvolvedor trabalhando neste repositório.

## 1. Escopo e Regra de Ouro de Higiene (Tolerância Zero)

1. **Nunca versionar material protegido ou binários gerados:**
   * Jamais adicione ao Git arquivos `.xbe`, `.iso`, `.bin`, conteúdo de `assets/` (exceto `assets/.gitkeep`), código C++ gerado pelo recompilador em `generated/`, executáveis/bibliotecas (`.exe`, `.dll`, `.lib`, `.pdb`, `.wasm`) ou chaves/credenciais (`coop-online.toml`, `.env`).
   * Antes de qualquer commit, rode `python tools/check_repo_hygiene.py`.
2. **Fronteira estrita de licenciamento e engenharia reversa limpa:**
   * **Proibido extrair endereços, descompilar ou ler arquivos gerados pelo `BLACKReloadedMod` (`kROM3r`):** mod de código fechado/licença restritiva; serve apenas como prova visual externa de que certos efeitos são possíveis, nunca como fonte técnica.
   * **Caminho limpo obrigatório:** partir do `sdk/` (`TGP482/XboxXBERecomp`, licença MIT) + `sdk/templates/game`, realizar nossa própria engenharia reversa sobre o `default.xbe` fornecido pelo usuário (Ghidra + `[xbe] trace`) e consultar apenas referências com licença permissiva explícita (`MuriloDagustin/black-seamless-coop` e `portingpete/Black-2006-Recomp`, ambos MIT). `TGP482/BLACK-XBERecomp` não tem arquivo de licença visível e atua só como baseline comportamental.
   * **Rótulo claro entre simulação e XBE real:** nenhum teste sintético/simulado pode ser descrito como comportamento verificado no jogo; toda validação de Fase 0–3 deve informar se rodou apenas sobre `libs/netcore` ou no `default.xbe` real.
3. **Não editar o submódulo `sdk/` diretamente sem necessidade:**
   * O `sdk/` aponta para `TGP482/XboxXBERecomp` (licença MIT). Ganchos específicos do jogo pertencem a `src/game/`, código de host a `src/common/` e a ponte multiplayer a `src/mp/`.
   * Quando iniciarmos a Fase 6 (trilha de pesquisa WebAssembly/WebGPU do runtime, após Fases 0–3 nativas), as implementações de plataforma (`gpu/webgpu.cpp`, `audio/webaudio.cpp`, memória WASM) devem respeitar as interfaces existentes do SDK (`gpu::Backend` em `sdk/runtime/gpu/backend.h` e `audio` em `sdk/runtime/audio/audio.h`).
4. **Sem subpastas em `src/game/` e `src/mp/`:**
   * O `CMakeLists.txt` e o `sdk/cmake/XbeRecomp.cmake` coletam fontes com `file(GLOB ... src/*/*.cpp)` e `file(GLOB ... src/${game_dir}/*.cpp)`. Mantenha todos os `.cpp`/`.h` em um único nível dentro de `src/common/`, `src/game/` e `src/mp/`.

## 2. Arquitetura de Dois Produtos

* **Campanha Intacta por Padrão:** `mp::CurrentRole()` inicia sempre em `mp::Role::OfflineCampaign`. Nenhuma mudança de multiplayer pode alterar o comportamento da campanha single-player quando o modo online não estiver ativo.
* **Desacoplamento Estanque (`libs/netcore`):**
  * `libs/netcore` não pode incluir cabeçalhos do SDK (`xdk/`, `runtime/`), nem do Windows (`<windows.h>`, Winsock) nem do Emscripten.
  * Toda nova mensagem de rede deve ter validação de tamanho, tipo, limites numéricos (`std::isfinite`, limites de mundo/velocidade) e teste correspondente em `libs/netcore/tests/test_netcore.cpp`.

## 3. Como Compilar e Testar

* **Testar `libs/netcore` (não exige a ISO/XBE de BLACK):**
  ```powershell
  cmake -S . -B build -DBLACK_NETCORE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
  cmake --build build --config Release
  ctest --test-dir build --output-on-failure
  ```
* **Compilar o jogo completo (exige Windows + VS 2022 Clang/CMake + `assets/default.xbe`):**
  ```powershell
  .\build.bat
  ```

## 4. Documentação de Referência no Repositório

* [Plano de Implementação v2](docs/PLANO_DE_IMPLEMENTACAO.md) — visão geral, fases e decisões congeladas.
* [Registro de Engenharia Reversa (`re-log`)](docs/re-log.md) — endereços `sub_XXXXXXXX` verificados do `default.xbe` e convenções do SDK.
* [Arquitetura para Navegador (WASM / WebGPU / WebRTC)](docs/browser-architecture.md) — desenho técnico para o alvo final no navegador.
