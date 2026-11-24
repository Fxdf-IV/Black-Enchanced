# Territorial Assault — decisões de regra (Fase 1 do GDD)

Implementação: [territorial.hpp](../libs/netcore/include/netcore/territorial.hpp) / [territorial.cpp](../libs/netcore/src/territorial.cpp),
testes em [test_territorial.cpp](../libs/netcore/tests/test_territorial.cpp). Lógica pura (C++20), relógio injetado
(`tick()` a 30 Hz), sem protocolo novo, sem ligação com o motor e sem efeito na campanha offline.
Todos os valores abaixo são hipóteses de balanceamento e ficam em `ta::Config`.

Plataforma-alvo: recompilação do **Xbox NTSC-U** (não PS2). Equipamentos: somente armas e itens **nativos de BLACK**
(AGENTS.md §2.2). A economia de Supply Points vale só para este modo; o Deathmatch mantém a Loja `B` com créditos.

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

- Mensagens `0xB1AC` do modo (com validação de tamanho/limites e fuzzing) e integração no host.
- Fase 2 em diante (mapas, spawns, zonas de captura) só depois que a base provar controle de entidades e combate local.
