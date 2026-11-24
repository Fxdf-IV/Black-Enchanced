# Registro de Engenharia Reversa e Convenções do SDK (`docs/re-log.md`)

Este documento cumpre as diretrizes das **Fases 0 e 1** do plano de implementação:
1. Responde diretamente às 3 perguntas de integração do SDK (`TGP482/XboxXBERecomp`, licença MIT).
2. Documenta o fluxo empírico dos **Cenários C1–C7** e **Alvos T1–T7** sobre o `default.xbe` real usando `xbe2pe.py` + Ghidra + `[xbe] trace`.
3. Registra a lista inicial de **hipóteses** (`sub_XXXXXXXX`, globais e offsets) herdadas de código aberto sob licença MIT, submetidas à **escala estrita de confiança**.

> [!NOTE]
> **Fases 0–4 (loopbase PvP em `Level_00`) verificadas em runtime no `default.xbe` real (Missão 1 / `Level_00`)** (NTSC-U retail, SHA-256 `DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6`):
> - Executável recompilado compilado e linkado com sucesso via toolchain portátil LLVM/Clang (`BlackEnhancedOnline.exe`, `BlackEnhancedOnline.dll`, `BlackEnhancedOnline_code`).
> - Símbolos `D3D8LTCG` corrigidos em `config/game.toml` destravaram renderização 3D completa sobre Direct3D 12 no Windows (`Level_00` inspecionado sem crashes: `Crashed=False`, geometria 3D, modelo de arma em primeira pessoa, efeitos de pós-processamento em baixa vida e partículas/estilhaços).
> - **`OfflineCampaign` intacta (`role=0`):** execução sem flags `--mp-*` validada em boot até `mp_bridge: first guest frame hook fired (sub_0020F6A0)`, `FEMain.bin` e `LO_N_US.xmv` com zero `exception`, `x86_trap` ou `error:`.
> - **Combate PvP real entre 2 instâncias UDP em `Level_00` (`black_enhanced_019.log` Host `role=1` e `black_enhanced_020.log` Cliente `role=2`):**
>   - `level initialized (local_max_hp=1200.0, spawns=2, match_tick=20227)` no Host e `bound player_id=1`/`player_id=0` ao ator `0x816959A0`.
>   - Disparo real bidirecional em `sub_001DBD50` (10 tiros locais do Host + 10 do Cliente, `accepted=1`, `claimed=255`), réplica visual/sonora (`render remote fire shooter=... wpn=0x8204B8D4` via `PrepareRemoteFireRender` -> `sub_00168E20`/`sub_001DB860`) sem erros `uncommitted` e dano limpo `-22`/tiro (`100 -> 78 -> 56 -> 34 -> 12 -> 0`, sem duplo dano de projétil assíncrono em `sub_001E74B0`).
>   - Mortes e respawns automáticos bidirecionais dentro de `Level_00` (`(-5.7, -0.3, 53.6)` e `(-4.2, -0.3, 52.6)` no Host; `(-5.7, -0.3, 53.6)` no Cliente) com `Crashed=False`, `HostExit=True`, `ClientExit=True`.
> - **É proibido** usar ou extrair endereços do `BLACKReloadedMod` (código fechado) ou inspecionar binários portados de terceiros.

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

### Alvos T1–T7 e Cenários Observáveis C1–C7

| Alvo | Descrição | Cenários Associados | Estado Atual |
| :--- | :--- | :--- | :--- |
| **T1** | Ator inimigo/aliado, spawners e cérebro de IA | C1, C3 | **Pool e vínculo em `Level_00` verificados em runtime** (`pool=0x81695900`, vínculo `player_id` -> `0x816959A0` no Host e no Cliente em `019.log`/`020.log`); supressão de IA (`sub_001E5F70`) ativa em gameplay; spawn dedicado via `sub_001AF880` permanece estático/pendente para sessões > atores pré-existentes |
| **T2** | Vida, dano, morte de NPC e morte/respawn do jogador | C4, C5, C6 | **Dano, morte e respawn bidirecionais verificados em runtime (`Level_00`)**: dano autoritativo `-22`/tiro sem duplo dano em `sub_001E74B0` (`100->78->56->34->12->0`), mortes e respawns in-place confirmados em `019.log`/`020.log`; dano por explosão (`sub_001DF830`) ainda não testado em PvP |
| **T3** | Disparo, origem/direção do projétil e colisão balística | C2, C3, C4, C7 | **Disparo e réplica remota verificados em runtime (`Level_00`)**: `sub_001DBD50` capturando tiros locais (`accepted=1`, `claimed=255`) e `PrepareRemoteFireRender` (`sub_00168E20` via `*kLevel + 0x4E2C0` + `sub_001DB860`) renderizando disparos remotos em Host e Cliente (`019.log`/`020.log`) |
| **T4** | Fluxo de fase, unidade/subárea e gatilhos de missão | C1, C6 | **Transição `inLevel=1` (`0x815D2F78`) e reset de partida (`level initialized`) verificados em runtime** (`Level_00`, `world=0x816AF180`); transições avançadas entre fases pendentes |
| **T5** | Loop de quadro principal (30 FPS) e prioridade de áudio | C1–C7 | **Verificado em runtime** (`sub_0020F6A0` disparando a cada quadro tanto em `OfflineCampaign` quanto em PvP Host/Cliente) |
| **T6** | Locomoção, animação, postura e câmera | C1, C3, C6 | **Verificado em runtime (`Level_00`)**: D3D12 3D completo, `kFnCharSetPos` (`sub_001CE660`) via `sub_001CCBB0` e locomoção `sub_0017E280` com correção de `IsGuestCommitted` para `kSystemBase (0xB0000000)` (`019.log`/`020.log` sem `uncommitted`) |
| **T7** | Destruição de objetos do cenário / barris explosivos | C7 | Pendente de captura de runtime (C7) |

*Legenda dos cenários no gameplay:* **C1** = boot/idle na fase · **C2** = tiro em parede · **C3** = tiro em inimigo · **C4** = matar inimigo · **C5** = levar dano · **C6** = morte do jogador · **C7** = destruir cenário.

---

## 4. Auditoria de Funções `sub_XXXXXXXX` (`default.xbe` Real)

As funções abaixo tiveram existência, assinatura e/ou execução conferidas diretamente no `assets/default.xbe` real (`generated/game/` e execução de runtime em `Level_00` via `black_enhanced_019.log` / `020.log` / `stderr` da `OfflineCampaign`):

| Endereço | Nome Provisório | Alvo | Achado Estático / Runtime no `default.xbe` Real | Estado / Confiança |
| :--- | :--- | :--- | :--- | :--- |
| `sub_0020F6A0` | `Game_FrameTick` (loop de quadro) | T5 | Chamada no loop `0x000E1AB7`; incrementa `[0x002F4368]`. Verificado em `OfflineCampaign` (`0.287s`), boot e gameplay `Level_00` (`019.log`/`020.log`) | **Alta (Runtime `OfflineCampaign` + `Level_00` PvP ok)** |
| `sub_001E74B0` | `Char_TakeBulletDamage` (dano projétil) | T2 | `ret 0x1C`; `ecx = victim`, `[esp+0x0C] = float damage`, `[esp+0x18] = Character* attacker`, sinaliza `[ebx+0x3AB] = 1` (`kCharHit`). Em `019.log`/`020.log`, guarda contra duplo dano suprime hits locais assíncronos chegando ~2 quadros (<= 8 ticks) após `sub_001DBD50` | **Média (Runtime `Level_00` PvP ok + `generated/game/`)** |
| `sub_001DF830` | `Char_TakeBlastDamage` (dano explosão) | T2 | `ret 0x1C`; `ecx = victim`, `[esp+0x0C] = float damage`, `[esp+0x18]` = categoria (`1/2/3`); subtrai de `[edi+0x2F0]` | **Estático no XBE real (gancho linkado; explosões PvP pendentes)** |
| `sub_001E5F70` | `AI_ThinkTick` (decisão de IA) | T1 | `AIController_vt @ 0x28bef0 vf[0]`; executando no loop `Level_00` com `ShouldSkipAiThink` ativo para slot remoto vinculado (`0x816959A0`) | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001AF880` | `Actor_SpawnSquadAlly` (spawn aliado) | T1 | `ret 0x30`; obtém `AIController` de `[0x002D1988]+0x22CDC`, chama `0x001DF440` (`[0x002D1980]`), vida `100.0f` (`0x00295CEC`) | **Estático no XBE real (sem runtime dedicado ainda)** |
| `sub_001D54C0` | `Player_OnDeath` (morte jogador) | T2 | `PlayerController_vt @ 0x28cf94 vf[4]`; `ret 0x10`; interceptado na morte do jogador em `Level_00` permitindo respawn in-place em `019.log`/`020.log` | **Média (Runtime `Level_00` PvP ok)** |
| `sub_0002B830` | `Scene_UpdateCamera` (câmera) | T6 | `RwCameraBeginUpdate`: grava `[ebp+0x0C]` em `0x0056A640`, chama `RwFrameGetLTM` (`0x0002F760`), `RwMatrixInvert` (`0x00032310`), `D3DDevice_SetTransform` (`0x0026AAE0`) | **Estático no XBE real (câmera 3D ativa em gameplay)** |
| `sub_001DB7B0` | `Player_FireBulletPrimary` | T3, T7 | `ret 0x14`; varre 16 slots em `edx+0x6E0` (passo `0x90`, `[esi+0x88]==0`), chama `0x001DB600` (raycast `[0x002D45D4]` via `0x001DAFF0`) | **Estático no XBE real (chamado via `sub_001DBD50`)** |
| `sub_001DBD50` | `Player_FireBulletSecondary` (hook de disparo) | T3, T7 | `ecx = Weapon*` (`weapon+0x14` = handler, `handler+0x28` = `Character* owner`); em `Level_00` captura disparos locais (`accepted=1`, `claimed=255`, 10 no Host + 10 no Cliente) e distingue disparos de NPCs (`owner != local`) | **Alta (Runtime `Level_00` Host/Cliente ok)** |
| `sub_00168E20` | `Tracer_EmitSpark` (`kFnTracerEmit`) | T3 | `edx = *kLevel + 0x4E2C0` (`kTracerMgrOffset`), `bl = 0`, pilha `start*`, `end*`, `ret 0x08`; chamado por `PrepareRemoteFireRender` em `sub_001CCBB0` (`render remote fire` + partículas em `mp2_host_15.png`) | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001DB860` | `Weapon_PlayFireSfx` (`kFnWeaponFireSfx`) | T3 | Lê tabela de assinatura sonora na arma (`sig_va`, `0x14` bytes, `[sig_va+0x10]`, `handler_va+0x108`); chamado por `PrepareRemoteFireRender` com guarda `IsValidWeaponPtrForSfx` sem crashes | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001DF440` | `Actor_SpawnCharacter` | T1 | Chamada por `sub_001AF880`; acessa pool `[0x002D1980]` | **Estático no XBE real (sem runtime)** |
| `sub_001DA1A0` | `Actor_OnDeathB` | T2 | Chamada em `0x001DF362` e `0x001DEC5D`; entrada em `x86_func_mapping.cpp` | **Estático no XBE real (sem runtime)** |
| `sub_001BD360` | `Player_DownState` | T2 | `PlayerController_vt @ 0x28cf94 vf[1]`; linkado no fluxo de queda/respawn em `Level_00` | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001CCBB0` | `Pool_UpdateAll` (pool/transformação) | T1, T6 | `ret 0x04`, `esi = pool` (`[0x002D1980]`), chamada após update dos atores; executa `PrepareRemoteActorTransform` -> `sub_001CE660` e `PrepareRemoteFireRender` em `019.log`/`020.log` | **Alta (Runtime `Level_00` Host/Cliente ok)** |
| `sub_0017E280` | `Anim_SelectLocomotion` | T6 | `plain ret`, `esi = anim_va` (`char+0x328`), `[anim_va] = Character*`; lê `char+0x2D8` (`kCharMeasuredSpeed`) e chama `sub_0017AC80`; ativo em `019.log`/`020.log` | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001CE660` | `Char_SetPosBBox` | T6 | `plain ret`, `ecx = Character*`, `eax = vec4* {x,y,z,w}` alinhado a 16B em `kSystemBase` (`AllocSystem`); atualiza posição, bbox e partição espacial `world+0x4960` sem erros `uncommitted` | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001CCCA0` | `Char_TeleportMatrix` | T6 | `plain ret`, `esi = Character*`, `ebx = &{x,y,z,yaw}` (`movaps` 16B); atualiza matriz `+0xD0..0x100`, prev pos e estado IA `[edi+0x1EA0]` | **Estático no XBE real (reservada como fallback)** |
| `sub_001DFC30` | `Char_BodyHitCrush` | T2, T6 | `ret 0x10`, `ecx = vítima` (`[ebp+0x14]`); gancho #9 ativo suprimindo esmagamento em slots multiplayer (`019.log`/`020.log` sem crashes) | **Média (Runtime `Level_00` PvP ok)** |
| `sub_001D6000` | `Char_CrushKill` | T2, T6 | `ret 0x04`, `edx = vítima` (`0x000E13AE`/`0x000E13E4`); gancho #10 ativo protegendo slots multiplayer | **Média (Runtime `Level_00` PvP ok)** |
| `sub_00205C60` | `Spawner_InitCharacter` | T1 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001DA240` | `Actor_OnDeathA` | T2 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001AD440` | `AI_TargetPerception` | T1, T6 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001C6410` | `Player_DeathFlow` | T2 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001B73B0` | `Level_UnitAdvance` | T4 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001C7620` | `Level_AreaActivate` | T4 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001F45C0` | `Level_FlowRequestA` | T4 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |
| `sub_001F45B0` | `Level_FlowRequestB` | T4 | Entrada exata em `x86_func_mapping.cpp` (corpo/semântica não auditados) | **Baixa (Sem auditoria de corpo / sem runtime)** |

---

## 5. Ponteiros Globais, Vtables e Offsets (`src/game/black_addrs.h` — Auditado no `default.xbe`)

> [!NOTE]
> **Resultado da validação em runtime (`Level_00`) + auditoria estática em `generated/game/`:** os globais e offsets principais da Fase 0 foram confirmados em tempo de execução no `black_enhanced_001.log` (`game=0x815B2000`, `inLevel=1`, `world=0x816AF180`, `player=0x816AF1B0`, `ctrl=0x816AF6A0`, `pool=0x81695900`, `cnt=5`). Notável em gameplay real: jogador local inicia com `hp ~ 1200.0` (observado `1148.0`) e aliados do pool `actor[0..1]` nascem com `team=0` e `hp=FLT_MAX`, enquanto inimigos `actor[2..4]` possuem `team=1` e `hp=100.0`.

### 5.1 Vtables Confirmadas no `default.xbe` Real
* `PlayerController_vt @ 0x28cf94` (confirmado em runtime `ctrl=0x816AF6A0`): `vf[0]=0x001EC450`, `vf[1]=0x001BD360`, `vf[4]=0x001D54C0` (`ret 0x10`).
* `AIController_vt @ 0x28bef0`: `vf[0]=0x001E5F70`, `vf[1]=0x001B6600`, `vf[2]=0x001CC980`, `vf[3]=0x001D5B00`, `vf[4]=0x001CC900`, `vf[5]=0x001BDC00`.
* `Sub014490_vt @ 0x28cfe0` (`Character` embutido em `world + 0x30`): `vf[4]=0x001E5FA0` (`Character::Update`), `vf[5]=0x001D25D0`.

### 5.2 Globais do Jogo e Mundo
* **Verificados em runtime no `default.xbe` (`Level_00`, `black_enhanced_001.log`):**
  * `0x002D45F0` (`kGame=0x815B2000`), `+0x21070` (`kGameState=0x815D2F78`) e `+0x20F78` (`kInLevelState`, `inLevel=1`, `state_vt=0x00295338`).
  * `0x002F4368` (`kFrameCounter=20227`), sincronizado com `frame=20227` em `sub_0020F6A0`.
  * `0x002D45D4` (`kWorld=0x816AF180`), `+0x30` (`kWorldPlayer=0x816AF1B0`), `+0x520` (`kWorldController=0x816AF6A0`, `PlayerController` embutido, `own=0x816AF1B0`), `+0x1C0` (`kWorldLastPos`), `+0x5AEC` (`kWorldLevel`, 8-bit `X86_MEM8`), `+0x5AF0` (`kWorldUnit`).
  * `0x002D1980` (`kPool=0x81695900`) e `+0x79B0` (`kAliveList`, `flags=0x00000007`, `cap=16`, `cnt=5`, `arr=0x817A8300`).
* **Confirmados em `Level_00` (`default.xbe`) e memória do SDK (`sdk/runtime/kernel/memory.cpp`):**
  * `0x002D1990` (`kLevel`) + `0x0004E2C0` (`kTracerMgrOffset`): subobjeto emissor de traçantes/faíscas validado em runtime via `PrepareRemoteFireRender` -> `sub_00168E20`.
  * `AllocSystem` aloca ponteiros convidados na faixa `mem::kSystemBase` (`0xB0000000..0xBFFFFFFF`), acima de `kUserHigh` (`0x7FFE0000`); `IsGuestCommitted` (`mp_bridge.cpp`) passou a checar páginas via `mem::IsCommitted(p)` em toda a faixa virtual `0..0xFFFFFFFF`, destravando `kFnCharSetPos` e `PrepareRemoteFireRender` sem rejeições silenciosas.
* **Pendentes de auditoria estática detalhada e runtime:**
  * `0x002F3A38` (`kGameFrameTime` = `1/30` s NTSC), `0x002D45D0` (`kLogic`), `0x002D67F4` (`kDisplay`).

### 5.3 Offsets de `Character` (Verificados no `default.xbe` Real — Base em Runtime + Combate Estático)
* `+0x008` (`name` uint64), `+0x0A0` (`pos float[3]`, lido em runtime `(-10.42,-0.34,56.87)`), `+0x0D0` (`view float[4][4]`), `+0x29C` (`weapon`, lido em runtime `0x82048690`), `+0x2D8` (`measured_speed float`, lido em `sub_0017E280`/`sub_0017AC80`), `+0x2F0` (`health`, `1148.0f` no jogador / `100.0f` em inimigos / `FLT_MAX` em aliados), `+0x324` (`control`, `0x816AF6A0`), `+0x328` (`anim`), `+0x384` (`state`, `0=vivo`, `2=morto`), `+0x39C` (`team`, `0` aliado / `1` inimigo), `+0x3A1` (`crouch`), `+0x3A7` (`kinematic`, `uint8`), `+0x3AB` (`hit`, `mov byte ptr [ebx+0x3AB], 1` em `sub_001E74B0`), `+0x630` (`deathControl` embutido).

### 5.4 Offsets de `AiController` / `PlayerController` (`[char + 0x324]`, Auditados no `default.xbe`)
* `+0x010`..`+0x01C` (`angles` em **graus**, lidos em runtime `yaw=36.5 pitch=0.0`; `max_pitch=70.0`, `min_pitch=-70.0`, `min_yaw=-180.0`, `wrap_yaw=360.0`). Coerente com `kDegToRad`/`kRadToDeg` em `mp_bridge.cpp`.
* `+0x020` (`speed` calculado em `0x001CC490`, lido em `0x001CE1A0`), `+0x040` (`crouch`), `+0x041` (`fire`), `+0x042` (`zoom/ADS` em `0x001EC450` / `0x001F0860`), `+0x044` (`reload`), `+0x045` (`fireMode` alternado em `0x001EC450` / `0x001F0860`), `+0x04A` (`locomotion` `S_MOV` `uint8`, setado em `0x001D210D`/`0x001D53B1`/`0x001DF3F3` e lido no sistema de animação), `+0x04B` (`melee` em `0x001ECB8E`, `0x001F08A7`), `+0x04C` (`throw`), `+0x060`/`+0x070` (`move`/`moveDir` `vec4`), `+0x084` (`rate`, lido em `sub_0017AC80`), `+0x08C` (`owner char`, `0x816AF1B0` em runtime), `+0x0B8`/`+0x0BC` (multiplicadores/sinais de sensibilidade de yaw e pitch em `sub_001BD3F0`), `+0x101` (`invert_y`, carregado de `[kGame+0x20167]` em `sub_001D5570`).

### 5.5 Câmera, FOV, Mira 1:1 e `WeaponSlotMgr` (Auditados Estaticamente no `default.xbe`)
* **`sub_001BD3F0` (`kFnPlayerLookUpdate`)** — chamada de `sub_001EC450` (`0x001ECF71`, `esi = PlayerController`, `ret 0x08`). Aplica deadzone (`0x00288C00`), curva/aceleração (`+0xD0`/`+0xD4`) e escala pelo fator de zoom (`[char+0x2A4]`), atualizando yaw (`[ctrl+0x18]` + espelho `+0x10`, wrap `360.0f @ 0x00295DF0`) e pitch (`[ctrl+0x1C]`, clamp `-70..+70` via `0x00295E38`/`0x002888C8`).
* **`sub_001BB110` (`kFnCameraFovUpdate`)** — atualiza `[char+0x2A4]` (`zoom_factor`, `1.0f` em hipfire) e, quando `*0x002D12B8` (`kCamera`) possui `[cam+0x16A4] != 0`, grava `[cam+0x750] = 70.0f / zoom_factor`.
* **Inventário de armas em `Character` (`WeaponSlotMgr @ char + 0x278`)**:
  * `+0x28C` (`kCharReserveAmmoPtr`, `uint8_t*` carregadores reserva indexados por `engine_weapon_id`), `+0x294` (`owner Character*`), `+0x298` (`kCharSlotsPtr`, `Weapon*[2]`), `+0x29C` (`kCharWeapon`), `+0x2A4` (`kCharZoomFactor`), `+0x2BA` (`kCharSlotCount`), `+0x2BB` (`kCharActiveSlot`), `+0x2BC` (`kCharActiveWeaponId`).
  * Global `0x002D12D0` (`kWeaponSystem`): `*ws` aponta ao cabeçalho de `WeaponDef[]` (`+0x01` contagem `uint8_t`, `+0x04` tabela de descritores 32B iniciando em `{hash_lo, hash_hi}`); `[ws+0x04]` e `[ws+0x08]` gerenciam o pool de `Weapon*` (passo `0x110`). Em `Weapon*`, `[wpn+0x04]` é o handler (`*handler` = `int8_t engine_weapon_id`) e `[wpn+0x108]` é o estado do silenciador (`sub_001DA570` / `sub_001C2690`).
  * Rotinas estáticas mapeadas em `hooks.cpp`: `sub_00014090` (`kFnWeaponDefByHash`), `sub_001C9F00` (`kFnWeaponFreeAll`, `ret 0x04`), `sub_001CEE10` (`kFnWeaponSpawn`, `push flag, id, char`, `ret 0x0C` -> `eax=Weapon*`), `sub_001C2690` (`kFnWeaponEquip`, `ecx=char+0x278, edx=Weapon*`, `plain ret`).

### 5.6 Lançamento Direto de Mapa via FrontEnd (`default.xbe` Real — Verificado em WebGPU)
* **Tabela de mapeamento Missão → ID interno de fase (`0x00288648`, 8 × `uint32_t`):** `{0, 1, 5, 3, 4, 6, 7, 8}` (`kMissionInternalLevelIds` em `src/game/black_addrs.h`). Nota: a Missão 3 (`Naszran Foundry`) usa o ID interno `5` (o ID `2` foi cortado pela Criterion).
* **Gatilho oficial de transição FrontEnd → InLevel (`sub_001132E0` / `sub_00124330` → `sub_0020DCA0`):**
  * `Game* = *(uint32_t*)0x002D45F0` (`kGame`); estado FrontEnd ativo quando `[Game + 0x21070] == Game + 0x20220` (`kFrontEndState`); estado InLevel em `Game + 0x20F78` (`kInLevelState`, vtable `0x00295338`, `OnEnter = sub_0020CE00`).
  * `FrontEnd* = *(uint32_t*)0x002D199C` (`kFrontEnd`).
  * Gravar `[Game + 0x2020C] = internal_level_id`, `[Game + 0x2020D] = 1` (dificuldade), `[Game + 0x2020E] = 1` (unidade inicial), `[FE + 0x3880] = Game + 0x20F78`, `[FE + 0x3884] = 1`, `[FE + 0x3888] = 1` faz o próprio tick do FrontEnd (`0x001244AC`) encerrar o FrontEnd de forma limpa e invocar `sub_0020DCA0(Game, Game + 0x20F78)`.
  * **Confiança:** **Média** — confirmado em `generated/game/x86_recomp.{29,30,31,52,53,55}.cpp`, no binário `assets/default.xbe` (`0x00288648`) e em execução real no navegador WebGPU (Host entrou diretamente em `02 · Treneska Border Crossing`, `internal_level_id = 1`, sem nenhum clique no menu do Xbox).
