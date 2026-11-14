# Registro de Engenharia Reversa e Convenções do SDK (`docs/re-log.md`)

Este documento cumpre os critérios de documentação das **Fases 0 e 1** do plano de implementação:
1. Documenta a relação exata entre `CMakeLists.txt`, `add_xbe_image` e `src/game` no `XboxXBE-Recomp`.
2. Registra todas as funções recompiladas (`sub_XXXXXXXX`) do `default.xbe` (NTSC-U, XDK 5849) já localizadas e verificadas em código aberto (`MuriloDagustin/black-seamless-coop`, licença MIT) e na auditoria do `BLACKReloadedMod` (`kROM3r`).

---

## 1. Convenção do SDK: como `add_xbe_image` e `src/game` se relacionam

No `XboxXBE-Recomp` (`sdk/cmake/XbeRecomp.cmake`), o `CMakeLists.txt` da raiz coleta primeiro os arquivos-fonte do host com `file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)` e **exclui** explicitamente `/src/game/` dessa lista (`list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")`). Isso ocorre porque um mesmo repositório pode registrar múltiplas imagens XBE, cada uma com sua própria pasta de ganchos. Quando `add_xbe_image(<target> <xbe> <config> <partitions> <game_dir>)` é chamado (por exemplo, `add_xbe_image(BlackEnhancedOnline default.xbe game.toml ${GAME_PARTITIONS} game)`), a função CMake coleta diretamente `src/${game_dir}/*.cpp` em `GAME_SOURCES` e compila o executável juntando `XBE_RUNTIME_SOURCES`, `XBE_HOST_SOURCES` (onde entram `src/common/*.cpp` e `src/mp/*.cpp`), `GAME_SOURCES` (`src/game/*.cpp`) e o código gerado das partições. Como ambos os `file(GLOB ...)` usam curinga de um único nível (`src/<módulo>/*.cpp`), **não devem ser criadas subpastas** dentro de `src/game/` nem de `src/mp/`.

---

## 2. Alvos da Fase 1 — Estado Atual das Evidências

Com a localização do `MuriloDagustin/black-seamless-coop` (MIT) e do `kROM3r/BLACKReloadedMod`, os principais alvos da Fase 1 já possuem funções candidatas verificadas em execução real:

| Alvo do Plano | Status | Endereços Principais | Evidência Empírica |
| :--- | :--- | :--- | :--- |
| **T1. Ator inimigo e cérebro de IA** | **Verificado (Alta)** | `sub_001DF440`, `sub_00205C60`, `sub_001E5F70`, `sub_001CCBB0`, `sub_001AF880` | `black-seamless-coop` instancia aliados (`0x1AF880`) para representar jogadores remotos e substitui o pensamento de IA (`0x1E5F70`) e atualização (`0x1CCBB0`) por snapshots de rede. `BLACKReloadedMod` instancia corpo em 3ª pessoa com 10 modelos e mira alinhada ao cano (< 1,5°). |
| **T2. Vida, dano e morte** | **Verificado (Alta)** | `sub_001DA240`, `sub_001DA1A0`, `sub_001D54C0`, `sub_001BD360`, `sub_001C6410` | Rotinas de baixa/morte de NPCs (`0x1DA240`, `0x1DA1A0`) e interceptação de morte do jogador local (`0x1D54C0`, `0x1BD360`, `0x1C6410`) para impedir tela de falha e permitir espectador/reviver. |
| **T3. Disparo e arma** | **Verificado (Alta)** | `sub_001DB7B0`, `sub_001DBD50` | Rotinas de disparo de projéteis do jogador; quando chamadas com origem/direção de um ator remoto, reproduzem balística, impacto, destruição de cenário, flash e som em todas as instâncias. |
| **T4. Fase e roteiro** | **Verificado (Média/Alta)** | `sub_001B73B0`, `sub_001C7620`, `sub_001F45C0`, `sub_001F45B0` | Avanço de unidade, ativação de áreas e requisições de fluxo de missão (completar/falhar/reiniciar). |
| **T5. Cadência de 30 FPS e Áudio** | **Verificado (Alta)** | `sub_0020F6A0` + `runtime/audio/xaudio2.cpp` | Loop de quadro principal em `0x20F6A0` (30 Hz). `BLACKReloadedMod` documenta prioridade de thread de áudio e reamostragem 48 kHz para eliminar engasgos sob carga de CPU. |
| **T6. Animação e Percepção** | **Verificado (Média/Alta)** | `sub_001CCBB0`, `sub_001AD440`, `sub_0002B830` | Atualização de locomoção/postura (`0x1CCBB0`), percepção de inimigos sobre múltiplos jogadores (`0x1AD440`) e câmera de cena (`0x02B830`). |
| **T7. Destruição de cenário** | **Verificado (Média)** | Integrado via `sub_001DB7B0` / `sub_001DBD50` | Reproduzir disparos pelas rotinas nativas de projétil aciona caixas, barris e objetos destrutíveis de forma consistente. |

---

## 3. Tabela de Funções Mapeadas (`default.xbe` — NTSC-U)

| Endereço | Nome Provisório | Assinatura Recompilada | Estruturas / Contexto (`X86Context`) | Comportamento Observado | Teste / Fonte de Reprodução | Confiança | Data e Autor |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `sub_0020F6A0` | `Game_FrameTick` | `void(X86Context& ctx, uint8_t* base)` | Sem argumentos de pilha críticos na borda | Executa um passo completo do quadro principal do jogo (30 Hz). Ponto ideal para drenar pacotes antes da simulação e emitir snapshots depois. | Gancho ativo em `src/game/hooks.cpp` e em `black-seamless-coop` (C1–C7). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001DB7B0` | `Player_FireBulletPrimary` | `void(X86Context& ctx, uint8_t* base)` | Contexto da arma/ator em registradores convidados | Dispara projétil usando o código balístico nativo do jogo (colisão, traçante, som, dano em cenário e atores). | Cenários C2, C3, C4, C7; usado para replicar tiros remotos em `coop_shots.cpp`. | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001DBD50` | `Player_FireBulletSecondary` | `void(X86Context& ctx, uint8_t* base)` | Contexto da arma/ator em registradores convidados | Caminho complementar de disparo de projétil/arma do jogador. | Cenários C2, C3, C4; pareado com `0x1DB7B0` em `coop_shots.cpp`. | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001AF880` | `Actor_SpawnSquadAlly` | `void(X86Context& ctx, uint8_t* base)` | Ponteiro/registro do aliado na memória convidada | Instancia um soldado aliado de esquadrão no mundo. Usado como corpo físico para jogadores remotos (com esqueleto completo de locomoção, agachamento e mira). | Teste #1 da Matriz da Fase 2 (`coop_npcs.cpp` e `BLACKReloadedMod` Squad/Body). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001DF440` | `Actor_SpawnCharacter` | `void(X86Context& ctx, uint8_t* base)` | Registro de spawner -> instância de ator | Criação de personagem/inimigo a partir do registro de spawner da fase. | Cenários C1, C3 (`coop_npcs.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_00205C60` | `Spawner_InitCharacter` | `void(X86Context& ctx, uint8_t* base)` | ID determinístico do spawner | Inicializa e vincula o ator recém-criado ao registro do spawner (mesmo identificador no host e nos clientes). | Sincronização de atores entre instâncias (`coop_npcs.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001E5F70` | `AI_ThinkTick` | `void(X86Context& ctx, uint8_t* base)` | Ponteiro do ator em atualização | Rotina de decisão da IA. Ao interceptar e retornar cedo para atores controlados remotamente, desliga o "cérebro de IA" mantendo o corpo sob controle externo. | Teste #2 da Matriz da Fase 2 ("Desligar o cérebro de IA"). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001CCBB0` | `Actor_UpdateFrame` | `void(X86Context& ctx, uint8_t* base)` | Posição, orientação, velocidade e flags de postura | Atualiza transformações e máquina de estados de locomoção do personagem a cada quadro. | Testes #3 e #4 da Matriz da Fase 2 (`coop_npcs.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001DA240` | `Actor_OnDeathA` | `void(X86Context& ctx, uint8_t* base)` | Ponteiro do ator vitimado | Processa morte/baixa de ator (animação de morte e transição de estado). | Cenário C4 e Teste #5 da Matriz da Fase 2. | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001DA1A0` | `Actor_OnDeathB` | `void(X86Context& ctx, uint8_t* base)` | Ponteiro do ator vitimado | Caminho complementar de baixa/morte de personagem. | Cenário C4 e Teste #5 da Matriz da Fase 2. | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001AD440` | `AI_TargetPerception` | `void(X86Context& ctx, uint8_t* base)` | Lista de alvos visíveis/audíveis | Determina qual alvo um ator hostil enxerga/ataca; permite incluir outros atores na linha de tiro e causar dano cruzado. | Cenário C5 e Teste #6 da Matriz da Fase 2. | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001D54C0` | `Player_OnDeath` | `void(X86Context& ctx, uint8_t* base)` | Estado de vida do jogador local | Rotina acionada quando a vida do jogador local chega a zero. Interceptada para impedir a tela de "Missão Fracassada" e acionar espera de respawn. | Cenário C6 e Testes #7 e #8 da Matriz da Fase 2 (`coop_life.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001BD360` | `Player_DownState` | `void(X86Context& ctx, uint8_t* base)` | Estado do jogador local | Transição de queda/abatido do jogador local. | Testes #7 e #8 da Matriz da Fase 2 (`coop_life.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001C6410` | `Player_DeathFlow` | `void(X86Context& ctx, uint8_t* base)` | Fluxo de falha / checkpoint | Impede recarga forçada da fase enquanto a partida multiplayer continua ativa. | Testes #7 e #8 da Matriz da Fase 2 (`coop_life.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_0002B830` | `Scene_UpdateCamera` | `void(X86Context& ctx, uint8_t* base)` | Matriz de câmera da cena | Controla a câmera principal (usada para câmera de espectador enquanto aguarda respawn ou câmera em 3ª pessoa). | Espectador pós-morte (`coop_life.cpp`). | **Alta** | 2026-11-14 · Equipe BE |
| `sub_001B73B0` | `Level_UnitAdvance` | `void(X86Context& ctx, uint8_t* base)` | Identificador de unidade/subárea | Avança a unidade/estágio interno da fase. | Alvo T4 (`coop_world.cpp`). | **Média** | 2026-11-14 · Equipe BE |
| `sub_001C7620` | `Level_AreaActivate` | `void(X86Context& ctx, uint8_t* base)` | Gatilho de área | Ativação de setores e portas de transição na fase. | Alvo T4 (`coop_world.cpp`). | **Média** | 2026-11-14 · Equipe BE |
| `sub_001F45C0` | `Level_FlowRequestA` | `void(X86Context& ctx, uint8_t* base)` | Código de requisição de fluxo | Dispara eventos de fim/reinício/continuação de missão. | Alvo T4 (`coop_world.cpp`). | **Média** | 2026-11-14 · Equipe BE |
| `sub_001F45B0` | `Level_FlowRequestB` | `void(X86Context& ctx, uint8_t* base)` | Código de requisição de fluxo | Variante complementar de requisição de fluxo da fase. | Alvo T4 (`coop_world.cpp`). | **Média** | 2026-11-14 · Equipe BE |

---

## 4. Mapa de Ponteiros Globais e Offsets de Estruturas (`src/game/black_addrs.h`)

Todos os deslocamentos abaixo foram verificados contra `default.xbe` NTSC-U (`title = 0x45410083`, `xdk = 5849`) e consolidados em [`src/game/black_addrs.h`](../src/game/black_addrs.h):

### 4.1 Globais do Jogo e do Mundo
* `0x002D45F0` (`kGame`): objeto global de sessão (`+0x234` índice da fase `0..7`, `+0x238` dificuldade `0..3`).
* `0x002D45D4` (`kWorld`): ponteiro para o mundo carregado (`+0x30` ponteiro do `Character` local; `+0x520` controlador de câmera/input do jogador; `+0x3E7D8` identificador da unidade atual; `+0x3E7FC` contador de quadros da fase).
* `0x002D1980` (`kPool`): pool global de entidades; em `*(uint32_t*)(base + 0x002D1980) + 0x79B0` reside a lista intrusiva duplamente encadeada de personagens vivos (`kAliveList`).
* `0x01C063EC` (`kSquadSlots`): quatro ponteiros consecutivos (`0..3`) de soldados aliados da cena.

### 4.2 Estrutura `Character` (Jogador e NPCs)
* `+0x000` (`next`) / `+0x004` (`prev`): elos na lista `kPool + 0x79B0`.
* `+0x008`: ponteiro para string do nome (`"Player"` para o jogador local; trocado por `"DeadPlayer"` em `0x001D54C0`).
* `+0x0A0`: posição no mundo (`float x, y, z, w` — 16 bytes).
* `+0x0D0`: direção de visão / frente (`float x, y, z, w`).
* `+0x29C`: ponteiro para a instância da arma equipada.
* `+0x2F0`: vida atual (`float`).
* `+0x324`: ponteiro para o controlador ativo (`PlayerController`, `AiController` ou `DeathController`).
* `+0x384`: estado de vida (`uint32_t`: `0` = normal, `2` = abatido/morto).
* `+0x3A1`: flag de postura agachada (`uint8_t`: `0` em pé, `1` agachado).
* `+0x3A7`: flag cinemático (`uint8_t`: `1` desliga gravidade/colisão de parede da IA para permitir interpolação suave de rede).
* `+0x630`: controlador de morte embutido (instalado em `+0x324` por `0x001D54C0`).

### 4.3 Estrutura `AiController` (em `*(uint32_t*)(character + 0x324)`)
* `+0x010` / `+0x014`: yaw alvo e yaw atual (`float`, radianos).
* `+0x018` / `+0x01C`: pitch alvo e pitch atual (`float`, radianos).
* `+0x020`: velocidade escalar de locomoção (`float`, `0.0f` parado, `1.0f` correndo).
* `+0x040`: flag de agachamento no controlador (`uint8_t`).
* `+0x060` e `+0x070`: vetores de deslocamento (`vec4`) usados pelo animador (`0x001CCBB0`).
* `+0x084`: taxa de blend de giro (`float`, `12.0f` para resposta imediata de rede).
* `+0x08C`: ponteiro reverso para o `Character` dono deste controlador.

