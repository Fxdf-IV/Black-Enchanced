# Registro de Engenharia Reversa e Convenções do SDK (`docs/re-log.md`)

Este documento cumpre as diretrizes das **Fases 0 e 1** do plano de implementação:
1. Responde diretamente às 3 perguntas de integração do SDK (`TGP482/XboxXBERecomp`, licença MIT).
2. Documenta o fluxo empírico dos **Cenários C1–C7** e **Alvos T1–T7** sobre o `default.xbe` real usando `xbe2pe.py` + Ghidra + `[xbe] trace`.
3. Registra a lista inicial de **hipóteses** (`sub_XXXXXXXX`, globais e offsets) herdadas de código aberto sob licença MIT, submetidas à **escala estrita de confiança**.

> [!WARNING]
> **Nenhum endereço ou offset abaixo foi verificado ainda no `default.xbe` real neste projeto.**
> Todas as entradas permanecem em **Confiança Baixa (Hipótese)** até que a Fase 0/1 seja executada com a cópia real do jogo na máquina Windows.
> **É proibido** usar ou extrair endereços do `BLACKReloadedMod` (código fechado).

---

## 1. Escala Oficial de Confiança (Página 12 do Plano)

| Nível | Critério Obrigatório |
| :--- | :--- |
| **Alta** | Gancho compilado e executado no `default.xbe` real, alterando o resultado de forma previsível em **>= 3 cenários**. |
| **Média** | Comportamento observado no `default.xbe` real em **1 cenário** + confirmação do fluxo em leitura no Ghidra. |
| **Baixa** | Hipótese externa ainda **não testada por nós no `default.xbe` real**. |

---

## 2. Respostas Verificadas no Código do SDK (`TGP482/XboxXBERecomp`)

1. **Como `src/game/` é compilado no SDK?**
   * Verificado em `sdk/templates/game/CMakeLists.txt` e `sdk/cmake/XbeRecomp.cmake`:
   * O `CMakeLists.txt` coleta fontes de host com `file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)` e **exclui** `/src/game/` via `list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")`.
   * Em seguida, `add_xbe_image(<target> <xbe> <config> <partitions> <game_dir>)` coleta `src/${game_dir}/*.cpp` (`GAME_SOURCES`) e os compila junto ao runtime e ao código gerado das partições.
2. **Qual é a regra de organização de diretórios?**
   * Ambos os `file(GLOB ...)` usam apenas um nível de subdiretório (`src/*/*.cpp` e `src/${game_dir}/*.cpp`). Portanto, **não podem existir subpastas** dentro de `src/common/`, `src/game/` ou `src/mp/`.
3. **O SDK suporta o XDK 5849 usado por BLACK?**
   * Sim. O submódulo contém `sdk/xdk/5849/signatures.toml` e `sdk/xdk/5849/layout.toml` (validados offline via `tools/recomp_black.py --check-toolchain`).

---

## 3. Fluxo de Engenharia Reversa Própria (Fase 1: `default.xbe` Real)

Quando `assets/default.xbe` (`Title ID 0x45410083`, `> 1 MB`, entrada `0x00025A3F`) estiver disponível no PC:

* **SHA-256 de referência do executável retail NTSC-U** (registrado sob licença MIT em `portingpete/Black-2006-Recomp`: `docs/file-formats/validation.md` e `launcher/src/core/GameFiles.h`):
  `DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6`
  *(Checado de forma não bloqueante por `tools/recomp_black.py` e `web/opfs_loader.js`.)*

1. **Gerar `config/game.toml` limpo a partir do XBE real:**
   ```powershell
   python tools/recomp_black.py --run-symbols
   ```
   *(Equivalente a `build/compiler/xbe_symbols.exe assets/default.xbe sdk/xdk --library BlackEnhancedOnline.dll --out config/game.toml`.)*
2. **Exportar PE plano com símbolos para análise no Ghidra:**
   ```powershell
   python tools/recomp_black.py --export-pe
   ```
   *(Executa `python sdk/tools/symbols/xbe2pe.py config/game.toml build/black_flat.pe.exe`, exportando `sub_XXXXXXXX`, `[functions]` e `[symbols]`.)*
3. **Rastrear chamadas nativamente sem recompilar ganchos (`[xbe] trace`):**
   * Conforme `sdk/tools/recomp/main.cpp`, adicionar `trace = [0x0020F6A0, ...]` na seção `[xbe]` de `config/game.toml` faz o recompilador emitir log automático em runtime com chamador, argumentos e retorno `eax`.

### Alvos T1–T7 e Cenários Observáveis C1–C7 (Pendentes de Execução Real)

| Alvo | Descrição | Cenários Associados | Estado Atual |
| :--- | :--- | :--- | :--- |
| **T1** | Ator inimigo/aliado, spawners e cérebro de IA | C1, C3 | Pendente do `default.xbe` real |
| **T2** | Vida, dano, morte de NPC e morte/respawn do jogador | C4, C5, C6 | Pendente do `default.xbe` real |
| **T3** | Disparo, origem/direção do projétil e colisão balística | C2, C3, C4, C7 | Pendente do `default.xbe` real |
| **T4** | Fluxo de fase, unidade/subárea e gatilhos de missão | C1, C6 | Pendente do `default.xbe` real |
| **T5** | Loop de quadro principal (30 FPS) e prioridade de áudio | C1–C7 | Pendente do `default.xbe` real |
| **T6** | Locomoção, animação, postura e câmera | C1, C3, C6 | Pendente do `default.xbe` real |
| **T7** | Destruição de objetos do cenário / barris explosivos | C7 | Pendente do `default.xbe` real |

*Legenda dos cenários no gameplay:* **C1** = boot/idle na fase · **C2** = tiro em parede · **C3** = tiro em inimigo · **C4** = matar inimigo · **C5** = levar dano · **C6** = morte do jogador · **C7** = destruir cenário.

---

## 4. Banco Inicial de Hipóteses — Funções `sub_XXXXXXXX` (TODAS EM CONFIANÇA BAIXA)

As funções abaixo **não foram testadas por nós no `default.xbe` real**. Todas constam explicitamente no repositório aberto sob licença MIT `MuriloDagustin/black-seamless-coop` (`README.md`, `src/game/coop*.cpp`) e devem ser confirmadas localmente por `[xbe] trace` e Ghidra:

| Endereço | Nome Provisório | Alvo | Proveniência MIT Auditada | Confiança |
| :--- | :--- | :--- | :--- | :--- |
| `sub_0020F6A0` | `Game_FrameTick` (loop de quadro) | T5 | `black-seamless-coop/README.md` + `src/game/coop.h` | **Baixa (Hipótese)** |
| `sub_001E5F70` | `AI_ThinkTick` (decisão de IA) | T1 | `black-seamless-coop/README.md` + `src/game/coop_npcs.cpp` | **Baixa (Hipótese)** |
| `sub_001AF880` | `Actor_SpawnSquadAlly` (spawn aliado) | T1 | `black-seamless-coop/README.md` + `src/game/coop_npcs.cpp` | **Baixa (Hipótese)** |
| `sub_001D54C0` | `Player_OnDeath` (morte jogador) | T2 | `black-seamless-coop/README.md` + `src/game/coop_life.cpp` | **Baixa (Hipótese)** |
| `sub_0002B830` | `Scene_UpdateCamera` (câmera) | T6 | `black-seamless-coop/README.md` + `src/game/coop.cpp` | **Baixa (Hipótese)** |
| `sub_001DB7B0` | `Player_FireBulletPrimary` | T3, T7 | `black-seamless-coop/README.md` + `src/game/coop_shots.cpp` | **Baixa (Hipótese)** |
| `sub_001DBD50` | `Player_FireBulletSecondary` | T3, T7 | `black-seamless-coop/README.md` + `src/game/coop_shots.cpp` | **Baixa (Hipótese)** |
| `sub_001DF440` | `Actor_SpawnCharacter` | T1 | `black-seamless-coop/README.md` + `src/game/coop_world.cpp` | **Baixa (Hipótese)** |
| `sub_00205C60` | `Spawner_InitCharacter` | T1 | `black-seamless-coop/README.md` + `src/game/coop_npcs.cpp` | **Baixa (Hipótese)** |
| `sub_001CCBB0` | `Actor_UpdateFrame` | T1, T6 | `black-seamless-coop/README.md` + `src/game/coop_npcs.cpp` | **Baixa (Hipótese)** |
| `sub_001DA240` | `Actor_OnDeathA` | T2 | `black-seamless-coop/README.md` + `src/game/coop_world.cpp` | **Baixa (Hipótese)** |
| `sub_001DA1A0` | `Actor_OnDeathB` | T2 | `black-seamless-coop/README.md` + `src/game/coop_world.cpp` | **Baixa (Hipótese)** |
| `sub_001AD440` | `AI_TargetPerception` | T1, T6 | `black-seamless-coop/README.md` + `src/game/coop_npcs.cpp` | **Baixa (Hipótese)** |
| `sub_001BD360` | `Player_DownState` | T2 | `black-seamless-coop/README.md` + `src/game/coop_life.cpp` | **Baixa (Hipótese)** |
| `sub_001C6410` | `Player_DeathFlow` | T2 | `black-seamless-coop/README.md` + `src/game/coop_life.cpp` | **Baixa (Hipótese)** |
| `sub_001B73B0` | `Level_UnitAdvance` | T4 | `black-seamless-coop/README.md` + `src/game/coop_world.cpp` | **Baixa (Hipótese)** |
| `sub_001C7620` | `Level_AreaActivate` | T4 | `black-seamless-coop/README.md` + `src/game/coop_world.cpp` | **Baixa (Hipótese)** |
| `sub_001F45C0` | `Level_FlowRequestA` | T4 | `black-seamless-coop/README.md` + `src/game/coop_control.cpp` | **Baixa (Hipótese)** |
| `sub_001F45B0` | `Level_FlowRequestB` | T4 | `black-seamless-coop/README.md` + `src/game/coop_control.cpp` | **Baixa (Hipótese)** |

---

## 5. Hipóteses de Ponteiros Globais e Offsets (`src/game/black_addrs.h` — CONFIANÇA BAIXA)

> [!WARNING]
> **Proveniência MIT esclarecida, mas ainda em Confiança Baixa:** Todos os globais e offsets mantidos em `src/game/black_addrs.h` foram conferidos nos repositórios MIT `MuriloDagustin/black-seamless-coop` (`src/game/coop*.cpp`) e `portingpete/Black-2006-Recomp` (`docs/plans/split-screen-coop.md`, `src/recomp_manual.c`). Um endereço morto sem fonte MIT (`kGameClockStep = 0x0002013C`) foi removido. Nenhum deles foi testado por nós contra um `default.xbe` real no PC até aqui.

### 5.1 Globais do Jogo e Mundo (Hipóteses a Conferir)
* `0x002D45F0` (`kGame`), `+0x21070` (`kGameState`), `+0x20F78` (`kInLevelState`) — `black-seamless-coop/src/game/coop.cpp`, `coop_life.cpp`, `coop_world.cpp`.
* `0x002F3A38` (`kGameFrameTime` = `1/30` s NTSC) — `Black-2006-Recomp/src/recomp_manual.c`.
* `0x002F4368` (`kFrameCounter`), `0x002D45D4` (`kWorld`), `+0x30` (`kWorldPlayer`, embutido), `+0x520` (`kWorldController`, **`PlayerController` embutido em `world + 0x520`, não um ponteiro para desreferenciar**), `+0x1C0` (`kWorldLastPos`), `+0x5AEC` (`kWorldLevel`), `+0x5AF0` (`kWorldUnit`) — `black-seamless-coop/src/game/coop.cpp`, `coop_control.cpp`, `coop_life.cpp`.
* `0x002D1980` (`kPool`), `+0x79B0` (`kAliveList`: flags `+0`, cap `+4`, count `+8`, array `+0xC`) — `black-seamless-coop/src/game/coop_npcs.cpp`.
* `0x002D45D0` (`kLogic`), `0x002D1990` (`kLevel`), `0x002D67F4` (`kDisplay`) — `black-seamless-coop/src/game/coop_control.cpp`, `coop_life.cpp`, `Black-2006-Recomp/src/recomp_manual.c`.

### 5.2 Offsets Hipotéticos de `Character` (`black-seamless-coop/src/game/coop_npcs.cpp`, `coop_life.cpp`, `coop.cpp`)
* `+0x008` (`name` uint64), `+0x0A0` (`pos float[3]`), `+0x0D0` (`view float[4][4]`), `+0x29C` (`weapon`), `+0x2F0` (`health`), `+0x324` (`control`), `+0x328` (`anim`), `+0x384` (`state`: 0 vivo / 1 ragdoll / 2 morto / 3 roteirizada / 4 livre), `+0x39C` (`team`), `+0x3A1` (`crouch`), `+0x3A7` (`kinematic`), `+0x3AB` (`hit`), `+0x630` (`deathControl` embutido).

### 5.3 Offsets Hipotéticos de `AiController` (`[char + 0x324]`, `black-seamless-coop/src/game/coop_npcs.cpp`)
* `+0x010`..`+0x01C` (`angles`), `+0x020` (`speed`), `+0x040` (`crouch`), `+0x041` (`fire`), `+0x044` (`reload`), `+0x04A` (`locomotion`), `+0x04B` (`melee`), `+0x04C` (`throw`), `+0x060`/`+0x070` (`move`/`moveDir`), `+0x084` (`rate`), `+0x08C` (`owner char`).
