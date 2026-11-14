# Black Enhanced Online — Plano de Implementação

**Atualizado em:** 14 de novembro de 2026 · **Base:** Revisão técnica e Especificação de 20 páginas (`@Vinicius`)

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
| **`libs/netcore`** | Protocolo binário (`0xB1AC`, 15 mensagens), validação anti-NaN/flood, `HostSession`/`ClientSession` e `LoopbackEndpoint`. | **Compilado e testado** (9 testes + fuzzing de 10k pacotes) **isolado do motor do jogo**. |
| **`sdk/` + `tools/recomp_black.py`** | Submódulo MIT + `xbe_recomp.exe`, `xbe_symbols.exe` compilados com `zig c++` + extrator `tools/xiso_extract.py`. | **Toolchain validada offline** contra `config/game.toml` e `sdk/xdk/5849/layout.toml`; aguarda `assets/default.xbe` real e VS 2022 para gerar/ligar o executável D3D12. |
| **`src/game/`, `src/mp/`, `config/game.toml`** | Rascunhos de ganchos e tabela de endereços copiados como hipótese de `black-seamless-coop` (MIT). | **Não linkados ao runtime D3D12 nem testados no `default.xbe` real.** Todos os endereços estão em **Confiança Baixa** em [`docs/re-log.md`](re-log.md). |
| **`web/` + `server/`** | Bancada JavaScript do protocolo `0xB1AC` via WebRTC DataChannels, Cloudflare Worker e verificador OPFS local (`Title ID 0x45410083`, `> 1 MB`). | **Não executa o motor de BLACK no navegador.** Arena 2D apenas exercita pacotes `libs/netcore` entre abas. |

---

## 3. Escala Oficial de Confiança (Engenharia Reversa)

Toda entrada em [`docs/re-log.md`](re-log.md) obedece estritamente a esta escala:
* **Alta:** o gancho foi testado por nós no `default.xbe` real e altera o comportamento de forma previsível em **>= 3 cenários**.
* **Média:** observado por nós em **1 cenário** no `default.xbe` real + leitura confirmada no Ghidra.
* **Baixa:** hipótese externa (ex.: `black-seamless-coop` MIT) ainda não reproduzida nem verificada por nós no `default.xbe` real.

---

## 4. Roteiro de Execução por Fases (0 a 6)

### Fase 0 — Baseline Nativa no Windows (Pré-requisito de Tudo)
**Objetivo:** Rodar a campanha original de BLACK recompilada no Windows (`Direct3D 12`) a partir do `sdk/` (MIT) sem alterar jogabilidade, com 1 gancho diagnóstico disparando.
* **Checklist:**
  1. Usuário fornece cópia legal de **BLACK (Xbox NTSC-U)** em `assets/` (`.iso` extraída via `tools/xiso_extract.py` ou arquivos diretos).
  2. Instalar Visual Studio 2022 (*Desktop C++*, *Clang tools*, *CMake tools*).
  3. Executar `xbe_symbols.exe assets/default.xbe sdk/xdk --library BlackEnhancedOnline.dll --out config/game.toml` (ou `python tools/recomp_black.py --run-symbols`) para popular `[symbols]` e `[data]` do XDK 5849.
  4. Executar `.\build.bat` e validar boot da campanha intacta (`mp::Role::OfflineCampaign`).
* **Critério de aceite:** Campanha entra em gameplay sem regressões + parágrafo de log do primeiro gancho de teste disparando em `src/game/hooks.cpp`.

### Fase 1 — Engenharia Reversa Própria no `default.xbe` Real
**Objetivo:** Validar e/ou corrigir os alvos **T1–T7** executando os cenários observáveis **C1–C7** sobre o executável recompilado.
* **Ferramentas nativas do SDK:**
  * Exportar PE plano com símbolos para o Ghidra: `python sdk/tools/symbols/xbe2pe.py config/game.toml build/black_flat.pe.exe` (via `python tools/recomp_black.py --export-pe`).
  * Rastreador de chamadas embutido no `xbe_recomp` (`[xbe] trace = [0x...]` em `config/game.toml`).
* **Alvos T1–T7:**
  * **T1:** Ator inimigo/aliado, pool de personagens e cérebro de IA.
  * **T2:** Vida, dano, morte do NPC e interceptação de morte/respawn do jogador.
  * **T3:** Rotina de disparo, origem/direção de projétil e balística.
  * **T4:** Estado de fase, unidade/subárea e fluxo de missão.
  * **T5:** Loop principal de quadro (30 FPS) e prioridade/fluxo de áudio.
  * **T6:** Atualização de locomoção, animações, postura e câmera.
  * **T7:** Propagação de disparos sobre objetos destrutíveis do cenário.
* **Cenários observáveis C1–C7:** C1 = boot/idle em fase; C2 = disparo em parede; C3 = disparo em inimigo; C4 = matar inimigo; C5 = sofrer dano de inimigo; C6 = morte do jogador local; C7 = destruição de cenário/explosivos.
* **Critério de aceite:** Funções essenciais promovidas a confiança **Média/Alta** em `docs/re-log.md` a partir de logs no XBE real + análise no Ghidra.

### Fases 2 e 3 — Controle de Entidade e Combate Local (Matriz de 11 Testes)
**Portão obrigatório:** **Não avançar para a Fase 4 (rede)** enquanto os 11 testes abaixo não passarem **no `default.xbe` real** em máquina única:

| Fase | Teste | Descrição |
| :--- | :--- | :--- |
| **Fase 2** | **#1** | Instanciar/identificar um segundo ator humanoide no mundo durante fase ativa sem crashar o jogo. |
| **Fase 2** | **#2** | Desligar o cérebro de IA do ator controlado sem congelar suas animações/esqueleto. |
| **Fase 2** | **#3** | Teleportar/mover continuamente o ator #2 para coordenadas arbitrárias válidas com suavidade. |
| **Fase 2** | **#4** | Replicar orientação (yaw/pitch), postura (em pé/agachado) e animação de locomoção no ator #2. |
| **Fase 2** | **#5** | Aplicar dano fatal no ator #2 e ler estado de abatido/vida chegando a zero. |
| **Fase 2** | **#6** | Permitir que inimigos ou projéteis interajam/colidam normalmente com o ator #2. |
| **Fase 2** | **#7** | Interceptar morte do jogador local evitando que dispare tela irrecuperável de *Missão Fracassada*. |
| **Fase 2** | **#8** | Ressuscitar (respawn in-place) o jogador e o ator #2 sem recarregar a fase inteira do disco. |
| **Fase 3** | **#9** | Disparar projétil real originado do ator #2 (flash, som, traçante e colisão na parede/cenário). |
| **Fase 3** | **#10** | Combate bidirecional: ator #2 fere/mata jogador local e jogador local fere/mata ator #2 com feedback de dano. |
| **Fase 3** | **#11** | Loop de partida Deathmatch local com dois atores: contagem de kills/deaths e respawn automático sem crash de colisão dupla. |

### Fase 4 — Rede Nativa no Windows (`libs/netcore` + UDP/ENet)
* Só inicia após 11/11 testes aprovados no XBE real.
* Implementar `ENetTransport` (ou UDP equivalente) satisfazendo `netcore::ITransport`.
* Ligar `src/mp/mp_bridge.cpp` aos sockets reais mantendo `OfflineCampaign` intacta por padrão.
* Validar partida LAN/Internet entre duas instâncias Windows de BLACK recompilado.

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
2. **Disponibilização local dos arquivos de BLACK (Xbox NTSC-U):**
   * Sem o `default.xbe` e os assets reais na máquina do usuário, **nenhum avanço das Fases 0 a 3 pode ser verificado de verdade**.
