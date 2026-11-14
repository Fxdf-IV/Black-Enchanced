// Ganchos de funções recompiladas do default.xbe (BLACK, NTSC-U, XDK 5849).
// Uma definição forte X86_FUNC(sub_XXXXXXXX) substitui a função gerada pelo recompilador;
// __imp__sub_XXXXXXXX chama a implementação original.
//
// Regra de ouro: quando mp::CurrentRole() == mp::Role::OfflineCampaign (padrão), todo gancho
// delega imediatamente para a função original sem alterar o estado da campanha.

#include "game/black_addrs.h"
#include "mp/mp_bridge.h"
#include "xdk/hle.h"

// 1. Quadro principal (0x0020F6A0): drena pacotes e atualiza atores remotos antes da simulação;
//    lê e transmite o estado do jogador local depois da simulação.
X86_EXTERN_FUNC(__imp__sub_0020F6A0);
X86_FUNC(sub_0020F6A0) {
    mp::OnPreFrame(base);
    __imp__sub_0020F6A0(ctx, base);
    mp::OnPostFrame(base);
}

// 2. Cérebro de IA (0x001E5F70 — vf0 do controlador de IA):
//    Teste #2 da Matriz da Fase 2 ("Desligar o cérebro de IA").
//    Se o controlador em ECX pertence a um ator controlado por jogador remoto, retorna sem executar a IA.
X86_EXTERN_FUNC(__imp__sub_001E5F70);
X86_FUNC(sub_001E5F70) {
    if (mp::ShouldSkipAiThink(ctx.ecx, base)) {
        // Plain ret (desempilha o endereço de retorno de 32 bits da pilha convidada)
        ::kernel::Return(ctx, ctx.eax, 0);
        return;
    }
    __imp__sub_001E5F70(ctx, base);
}

// 3. Morte do jogador local (0x001D54C0 — evento 5 do controlador do jogador):
//    Teste #7 e #8 da Matriz da Fase 2. Salva o controlador e o nome do jogador antes que 0x001D54C0
//    os substitua pelo controlador de morte (+0x630), permitindo respawn limpo sem recarregar a fase.
X86_EXTERN_FUNC(__imp__sub_001D54C0);
X86_FUNC(sub_001D54C0) {
    mp::OnLocalPlayerDying(base);
    __imp__sub_001D54C0(ctx, base);
}

// 4. Verificação por quadro do estado de morte do jogador (0x001BD360 — vf1 do controlador):
//    Quando o jogador está com state == 2 (morto), 0x001BD360 pede "Missão Fracassada" (0x001F45C0).
//    Em partida multiplayer ativa, suprimimos essa chamada para aguardar o RespawnEvent.
X86_EXTERN_FUNC(__imp__sub_001BD360);
X86_FUNC(sub_001BD360) {
    if (mp::ShouldSuppressMissionFailed(base)) {
        const uint32_t player = black::LocalPlayerAddr(base);
        if (player && black::R32(base, player + black::kCharState) == 2u) {
            ::kernel::Return(ctx, ctx.eax, 0);
            return;
        }
    }
    __imp__sub_001BD360(ctx, base);
}
