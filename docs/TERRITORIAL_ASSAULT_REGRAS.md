# Territorial Assault — decisões de regra, protocolo e integração Web

Implementação distribuída em três camadas (sem efeito na campanha offline):
- Lógica pura (`libs/netcore`, C++20 puro): [territorial.hpp](../libs/netcore/include/netcore/territorial.hpp) / [territorial.cpp](../libs/netcore/src/territorial.cpp), testes em [test_territorial.cpp](../libs/netcore/tests/test_territorial.cpp).
- Protocolo `0xB1AC` (v4), regras de Supply Points e integração autoritativa Host/Cliente: [protocol.hpp](../libs/netcore/include/netcore/protocol.hpp), [protocol.cpp](../libs/netcore/src/protocol.cpp), [rules.hpp](../libs/netcore/include/netcore/rules.hpp), [rules.cpp](../libs/netcore/src/rules.cpp), [session.hpp](../libs/netcore/include/netcore/session.hpp), [session.cpp](../libs/netcore/src/session.cpp), testes e fuzzing em [test_netcore.cpp](../libs/netcore/tests/test_netcore.cpp).
- Ponte do jogo, exportações WASM e Portal Web: [mp_bridge.h](../src/mp/mp_bridge.h), [mp_bridge.cpp](../src/mp/mp_bridge.cpp), [web_main.cpp](../src/web/web_main.cpp), [CMakeLists.txt](../CMakeLists.txt), [web/index.html](../web/index.html) e [web/app.js](../web/app.js) (`_BlackWebSetRoleMode`, `_BlackWebTerritorialAction`, seletor de modo, convite `?room=...&mode=ta`, HUD tático `#ta-hud`, Armaria `15 SP` e placar por time).
Todos os valores abaixo são hipóteses de balanceamento e ficam em `ta::Config` / `RulesConfig::territorial_assault()`.

Plataforma-alvo: recompilação do **Xbox NTSC-U** (não PS2). Equipamentos: somente armas e itens **nativos de BLACK**
(AGENTS.md §2.2). A economia de Supply Points (`15 SP`, `RulesConfig::territorial_assault()`) vale só para este modo; o Deathmatch mantém a Loja `B` com créditos (`RulesConfig::cs_loadout_deathmatch()`).

## Mensagens `0xB1AC` (protocolo v4)

- `TeamSelectRequest` (20, cliente → host, `Reliable`): escolha de equipe no Lobby (`team` 0 ou 1). Ignorado após o início da partida.
- `TerritorialState` (21, host → clientes, `Unreliable`): estado autoritativo para o HUD (fase, equipe atacante, objetivo atual, progresso `0..1000`, tempo restante, overtime, ondas, placar R1/R2, times por slot, centro/raio da zona ativa para bússola/distância, ETA da próxima onda e contagem de jogadores na zona). Clientes que tentarem enviar esta mensagem são penalizados/expulsos (`KickReason::Malformed`).
- `TerritorialEvent` (22, host → clientes, `Reliable`): eventos discretos (mudança de fase, objetivo capturado, onda de respawn, overtime, fim de rodada). Exclusivo do host.

## Lacunas do GDD resolvidas (ajustáveis após playtest)

| Ponto | Decisão adotada |
|---|---|
| Overtime sem fim (§7.2) | Disputa válida = pelo menos um atacante vivo na zona. Overtime com teto (`overtime_cap_ticks`, 90 s); ao estourar, a rodada termina (`OvertimeCap`). Captura em overtime concede bônus e sai dele. |
| Rodada 2 (§2, §8) | Termina antes se a equipe superar em objetivos o resultado da rodada 1 (`round2_early_end`). Se a rodada 1 foi completa, o tempo dela é o "tempo a bater": a rodada 2 não passa dele e não tem overtime. |
| Todos mortos ao mesmo tempo (§7.3) | Sem reforços para ambos: atacantes falham (defensores vencem a rodada). |
| Consumo de onda (§4.3) | Relógio da onda só corre se há morto na equipe; ao fim do intervalo, se há onda disponível, consome 1 e revive todos. Sem mortos, nada é consumido. Sem ondas, o relógio fica pronto e a onda sai assim que reforços forem concedidos. |
| Progresso nunca regride (§3.3) | Mantido (só congela); os defensores não recapturam. |
| Autoridade da captura (§11) | O módulo recebe `set_in_zone` já validado pelo host (posição e velocidade). Cliente nunca informa ocupação. |
| Ondas 5/15 (§4.2) | Configuráveis; teto `max_waves`. Para 1v1, reduzir os valores. |
| Desconexão (§7.3) | Equipe sem jogadores perde a partida (`Abandonment`). |

## Desempate (§8.2)

1. mais objetivos; 2. maior progresso do próximo objetivo no encerramento; 3. menor tempo de conclusão (se ambas completaram);
4. empate.

## Próximos passos

- Validar in-level em 2+ abas WebGPU (ambos os jogadores dentro da Missão 1 simultaneamente) a captura de zona A -> B com distância/bússola, compra de kit `15 SP` na Preparação/Intervalo e respawn em onda coletiva após eliminação.
- Calibrar zonas de captura e spawns fixos por missão (hoje derivados automaticamente da posição inicial + `kAliveList` da fase carregada) conforme o playtest real em cada mapa de BLACK.
