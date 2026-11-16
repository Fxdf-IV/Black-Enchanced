# Black Enhanced Online — Plano de Implementação (Histórico / Base)

> [!NOTE]
> **Documento atualizado pelo roteiro ativo:** A Fase 6 (motor WASM + WebGPU + AudioWorklet + Loadout CS + UI/UX no navegador) já foi iniciada e está funcional em estado inicial. Para o estado real atual e todas as próximas entregas (Campanha Enhanced/Reloaded + Multiplayer completo no navegador), consulte **[docs/PLANO_PROXIMOS_PASSOS.md](PLANO_PROXIMOS_PASSOS.md)**.

**Atualizado em:** 15 de novembro de 2026 · **Base:** Revisão técnica e Especificação de 20 páginas (`@Vinicius`)

> [!IMPORTANT]
> **Princípio de ordem (PC primeiro, navegador por último):**
> Primeiro provar no Windows nativo (Direct3D 12 via `sdk/`) que conseguimos controlar entidades do executável recompilado de **BLACK** (`default.xbe`) e executar combate local entre duas entidades (**Fases 0 a 3**). Só então integrar a rede (**Fases 4–5**) e abrir a trilha de pesquisa para navegador em WebAssembly/WebGPU (**Fase 6**).
>
> **Regra de ouro contra simulação:** Todo critério de aceite das Fases 0 a 3 deve ser medido **no `default.xbe` real**. Testes em `libs/netcore` ou na bancada JavaScript devem ser explicitamente rotulados como simulações isoladas da camada de rede.

---

## 1. Fronteira de Licenciamento e Inventário de Referências

| Projeto / Artefato | Licença | Papel Permitido no Projeto | Limites e Observações |
| :--- | :--- | :--- | :--- |
| **`TGP482/XboxXBE-Recomp`** (`sdk/`) | **MIT** | **Base técnica do projeto** | Fornece `xbe_recomp`, `xbe_symbols`, `xbe2pe.py`, CMake (`add_xbe_image`) e o runtime Win32/D3D12/XAudio2. Submódulo fixado em `cc9edda3a8`. |
| **`TGP482/BLACK-XBERecomp`** | **Sem licença visível** | **Baseline comportamental apenas** | Sem arquivo de licença no repositório: **não** copiamos nem derivamos código dele. Partimos de `sdk/templates/game` (MIT) + `xbe_symbols` no XBE do usuário. Ação pendente: contatar TGP482 sobre licença. |
| **`MuriloDagustin/black-seamless-coop`** | **MIT** | **Referência aberta de co-op** | Mod co-op em rede (UDP 45410, host autoritativo, atores aliados como corpos remotos). **Limites:** é co-op vs IA (não Deathmatch PvP), projeto jovem (20 commits), README cita 8 jogadores vs descrição 2 jogadores. Endereços tratados como **Baixa confiança** até reprodução local. Ação: contatar autor. |
| **`portingpete/Black-2006-Recomp`** | **MIT** | **Referência gráfica** | Técnicas de ambient occlusion e FOV amplo. |
| **`kROM3r/BLACKReloadedMod`** | **Fechado / All Rights Reserved** | **Prova de existência visual apenas** | **PROIBIDO** descompilar, extrair endereços ou ler arquivos de configuração gerados por este mod (zona cinzenta/restritiva). |
| **`Dhaiwat10/cricket07-browser`** | **MIT** | **Referência de port WASM/WebGPU** | Modelo de gravação de comandos gráficos, OPFS e sinalização Cloudflare Worker + Durable Object (ressalva: `cricket07` usa lockstep; BLACK exige snapshots autoritativos). |
| **`Solid7s/Virtual-On-Web`** | Referência web | **Alerta jurídico de WASM** | Alerta que binários WebAssembly recompilados contêm instruções traduzidas da ROM/XBE — ponto crítico da Decisão #1 (Fase 6). |

---

## 2. Estado Real Atual (Sem Simular Progresso)

| Componente | O que realmente existe hoje | Status de Verificação |
| :--- | :--- | :--- |
| **`libs/netcore`** | Protocolo binário (`0xB1AC`, 15 mensagens), validação anti-NaN/flood, `HostSession`/`ClientSession` (incluindo `start_match()` no carregamento da fase, `apply_damage`, `set_spawn_points`, `Hello` duplicado idempotente e fallback de raycast em `apply_validated_fire`) e `LoopbackEndpoint`. | **Compilado e aprovado no `ctest`** (`100% tests passed`, `netcore_tests`) + suíte sintética `tools/test_mp_udp.cpp`, **isolado do motor do jogo**. |
| **`sdk/` + `tools/recomp_black.py` + `config/game.toml`** | Submódulo MIT + recompilação completa de `assets/default.xbe` (`15041` funções, símbolos `D3D8LTCG` corrigidos) + runtime D3D12/XAudio2. | **Compilado e validado em runtime no Windows (`out/build/relwithdebinfo/BlackEnhancedOnline.exe`)** com toolchain portátil LLVM/Clang; `OfflineCampaign` intacta validada em boot (`role=0`). |
| **`src/game/`, `src/mp/` (`mp_bridge`, `mp_udp`)** | Ganchos #1–#11 (`sub_0020F6A0`, `sub_001E5F70`, `sub_001D54C0`, `sub_001BD360`, `sub_001E74B0`, `sub_001DF830`, `sub_001CCBB0`, `sub_0017E280`, `sub_001DFC30`, `sub_001D6000`, `sub_001DBD50`), vínculo de ator remoto, `kFnCharSetPos` (`sub_001CE660`), réplica de disparo (`sub_00168E20`/`sub_001DB860`), guarda anti duplo-dano, respawn in-place e UDP `45410`. | **Loop PvP de 2 instâncias UDP validado em runtime dentro da `Level_00` (`default.xbe` real, `black_enhanced_019.log`/`020.log`)**: disparos bidirecionais, dano `-22`/tiro, mortes, respawns e saída limpa sem crashes. Pendências: explosões (`sub_001DF830`), spawn dedicado (`sub_001AF880`), HUD/placar PvP e LAN multi-máquina. |
| **`web/` + `server/` + `src/common/{wasm_memory,gpu_webgpu,gpu_command_stream,audio_webaudio}.h`** | Bancada JavaScript do protocolo `0xB1AC` via WebRTC DataChannels, Cloudflare Worker, verificador OPFS local (`Title ID 0x45410083`, `> 1 MB`) e blocos preparatórios de arena WASM/fila WebGPU/downmix áudio. | **Não executa o motor de `default.xbe` no navegador.** Coberto apenas pela suíte sintética `tools/test_web_runtime.cpp` e arena 2D JS entre abas. |

---

## 3. Escala Oficial de Confiança (Engenharia Reversa)

Toda entrada em [`docs/re-log.md`](re-log.md) obedece estritamente a esta escala:
* **Alta:** o gancho foi testado por nós no `default.xbe` real e altera o comportamento de forma previsível em **>= 3 cenários**.
* **Média:** observado por nós em **1 cenário** no `default.xbe` real + leitura confirmada no Ghidra.
* **Baixa:** hipótese externa (ex.: `black-seamless-coop` MIT) ainda não reproduzida nem verificada por nós no `default.xbe` real.

---

## 4. Roteiro de Execução por Fases (0 a 6)

### Fase 0 — Baseline Nativa no Windows (Concluída e Verificada em Gameplay 3D)
**Objetivo:** Rodar a campanha original de BLACK recompilada no Windows (`Direct3D 12`) a partir do `sdk/` (MIT) sem alterar jogabilidade, com 1 gancho diagnóstico disparando.
* **Concluído no `assets/default.xbe` real:**
  1. `assets/default.xbe` (NTSC-U, `Title ID 0x45410083`) extraído localmente (não versionado no Git).
  2. Build nativo funcional via toolchain portátil LLVM/Clang (`out/build/relwithdebinfo/BlackEnhancedOnline.exe` / `.dll`), com símbolos `D3D8LTCG` corrigidos em `config/game.toml`.
  3. Boot e gameplay da Missão 1 (`Level_00`) executados via `run_phase0.ps1` sem crash (`Crashed=False`, renderização 3D completa confirmada visualmente em cena, arma, HUD de munição `015 -> 007` e pós-processamento). Pequenos retângulos pretos residuais em alguns glifos/retícula do HUD permanecem não-bloqueantes para as Fases 0–3.
  4. Primeiro gancho (`sub_0020F6A0`) disparando e telemetria `re_probe` confirmada em [`docs/re-log.md`](re-log.md).

### Fase 1 — Engenharia Reversa Própria no `default.xbe` Real (Telemetria Base + Combate `Level_00` Validados)
**Objetivo:** Validar e/ou corrigir os alvos **T1–T7** executando os cenários observáveis **C1–C7** sobre o executável recompilado.
* **Progresso real:** `T1–T6` com globais, `player`, `ctrl`, `pool`, captura de tiro (`sub_001DBD50`), emissor de traçantes (`*kLevel + 0x4E2C0` / `sub_00168E20`), SFX (`sub_001DB860`), dano balístico (`sub_001E74B0`), posicionamento (`sub_001CE660`) e respawn in-place validados em runtime na `Level_00` (`019.log`/`020.log`); cenário C7 (destruição de barris/cenário isolado) e spawner dedicado `sub_001AF880` seguem como próximos refinamentos.

### Fases 2 e 3 — Controle de Entidade e Combate Local (Matriz de 11 Testes)
**Status atual no `default.xbe` real (`Level_00`, `black_enhanced_019.log` / `020.log`):**
* **Validado em runtime PvP real de 2 instâncias na `Level_00`:** vínculo do ator remoto no pool (`0x816959A0`), supressão de IA (`sub_001E5F70`), sincronização espacial/locomoção após correção de `IsGuestCommitted` (`sub_001CCBB0` -> `sub_001CE660` + `sub_0017E280`), réplica de disparo remoto (`PrepareRemoteFireRender` -> `sub_00168E20` / `sub_001DB860`, partículas confirmadas em `mp2_host_15.png`), combate bidirecional (`-22`/tiro sem duplo dano em `sub_001E74B0`, baixa vida em `mp2_client_15.png`) e múltiplas mortes/respawns in-place sem crash (`Crashed=False HostExit=True ClientExit=True`).
* **Ressalvas explícitas (sem mascarar pendências):** nos logs `019.log`/`020.log`, os disparos usaram o fluxo autoritativo de rede (`claimed=255` via `HostSession` com fallback de raycast) sobre ator reutilizado do pool (`0x816959A0`); spawn dedicado via `sub_001AF880`, dano PvP por explosões (`sub_001DF830`), destruição C7 e HUD de placar Deathmatch seguem pendentes antes de declarar 100% dos refinamentos encerrados:

| Fase | Teste | Descrição | Status no `default.xbe` Real |
| :--- | :--- | :--- | :--- |
| **Fase 2** | **#1** | Instanciar/identificar um segundo ator humanoide no mundo durante fase ativa sem crashar o jogo. | **Validado via reuso de pool** (`0x816959A0` em `Level_00`); spawn dedicado `sub_001AF880` pendente. |
| **Fase 2** | **#2** | Desligar o cérebro de IA do ator controlado sem congelar suas animações/esqueleto. | **Validado em runtime** (`sub_001E5F70` ativo em `Level_00`). |
| **Fase 2** | **#3** | Teleportar/mover continuamente o ator #2 para coordenadas arbitrárias válidas com suavidade. | **Validado em runtime** (`sub_001CCBB0` -> `sub_001CE660` após fix de `IsGuestCommitted`). |
| **Fase 2** | **#4** | Replicar orientação (yaw/pitch), postura (em pé/agachado) e animação de locomoção no ator #2. | **Validado em runtime (`Level_00`)** (`sub_0017E280` + `ApplySnapshotToActor`). |
| **Fase 2** | **#5** | Aplicar dano fatal no ator #2 e ler estado de abatido/vida chegando a zero. | **Validado no loop PvP** (`100 -> 78 -> 56 -> 34 -> 12 -> 0` em Host/Cliente via `HostSession`). |
| **Fase 2** | **#6** | Permitir que inimigos ou projéteis interajam/colidam normalmente com o ator #2. | **Validado no loop PvP em `Level_00`** (guarda anti duplo-dano em `sub_001E74B0` + `kCharHit`); explosões (`sub_001DF830`) pendentes. |
| **Fase 2** | **#7** | Interceptar morte do jogador local evitando que dispare tela irrecuperável de *Missão Fracassada*. | **Validado em runtime** (2 mortes no Host e 1 no Cliente em `Level_00` sem travar). |
| **Fase 2** | **#8** | Ressuscitar (respawn in-place) o jogador e o ator #2 sem recarregar a fase inteira do disco. | **Validado em runtime** (`(-5.7,-0.3,53.6)` e `(-4.2,-0.3,52.6)` em `Level_00`). |
| **Fase 3** | **#9** | Disparar projétil real originado do ator #2 (flash, som, traçante e colisão na parede/cenário). | **Validado nos logs + captura** (`render remote fire ... wpn=0x8204B8D4`, partículas em `mp2_host_15.png`). |
| **Fase 3** | **#10** | Combate bidirecional: ator #2 fere/mata jogador local e jogador local fere/mata ator #2 com feedback de dano. | **Validado em `Level_00` real** (`019.log` / `020.log`: 10 tiros Host + 10 tiros Cliente, baixa vida em `mp2_client_15.png`). |
| **Fase 3** | **#11** | Loop de partida Deathmatch local com dois atores: contagem de kills/deaths e respawn automático sem crash de colisão dupla. | **Loop base validado sem crashes** (`sub_001DFC30`/`sub_001D6000` ativos; HUD de placar PvP pendente). |

### Fase 4 — Rede Nativa no Windows (`libs/netcore` + UDP)
* **Validado em runtime no `default.xbe` real (`Level_00`):** `UdpTransport` (`src/mp/mp_udp.h`/`.cpp`, porta UDP `45410`), `--mp-host` / `--mp-join` (`src/mp/mp_bridge.cpp`), `HostSession::start_match()` no início da fase, retry de `Hello`, escalamento de vida e combate bidirecional completo entre Host (`role=1`) e Cliente (`role=2`) em `127.0.0.1:45410` (`black_enhanced_019.log` / `020.log`).
* **Pendente na Fase 4:** teste em LAN/internet entre PCs distintos e polimento de HUD/placar de fim de partida.

### Fase 5 — Transporte WebRTC & Sinalização por Código de Sala
* Implementar `WebRtcTransport` compatível com os dois canais lógicos (`Reliable` / `Unreliable`) e sinalização leve (já prototipada em `server/` + `web/`).
* Testar interoperabilidade nativa Windows <-> sinalização em nuvem (Cloudflare Worker cota gratuita).

### Fase 6 — Trilha de Pesquisa: Navegador (`WASM` + `WebGPU`)
* Mantida estritamente como **trilha de pesquisa** até que as Fases 0–3 estejam comprovadas no PC.
* Foco técnico em [`docs/browser-architecture.md`](browser-architecture.md):
  1. Memória linear em `wasm32` com arena compacta de 256 MB (evitando exigir `wasm64` experimental no navegador).
  2. `gpu::Backend` (`sdk/runtime/gpu/backend.h`) sobre WebGPU, resolvendo o gargalo assíncrono de `Sync`, `SyncLate`, `ClearReport`/`GetReport` e fences.
  3. Áudio sobre `AudioWorklet` (48 kHz) cumprindo `sdk/runtime/audio/audio.h`.

---

## 5. Decisões em Aberto (Dependem de Escolha do Usuário)

1. **"Clicar e jogar" (`nfsu2.xyz`) vs. Usuário traz seus arquivos (OPFS):**
   * Hospedar assets e módulo WASM recompilado entra em conflito direto com a política de não distribuir conteúdo protegido da EA/Criterion — e o `Virtual-On-Web` alerta que o `.wasm` gerado contém instruções traduzidas do executável original.
   * Alternativa limpa: usuário fornece o disco uma única vez no navegador (salvo no OPFS), restando resolver o fluxo técnico de geração/link local do `.wasm` a partir do `default.xbe` do usuário.
2. **Arquivos locais do BLACK (Xbox NTSC-U):**
   * **Resolvido na máquina local:** `assets/default.xbe` e os assets reais do disco já estão presentes localmente (excluídos do Git) e permitiram validar `OfflineCampaign` + loop PvP UDP de 2 instâncias na `Level_00`. Pendentes para alcance do objetivo final completo: refinamentos das Fases 2–4 (spawn dedicado `sub_001AF880`, explosões, HUD/placar PvP, LAN entre máquinas), Fase 5 (WebRTC + sinalização nuvem no cliente Windows) e Fase 6 (recompilação e execução real do motor no navegador via WASM/WebGPU).
