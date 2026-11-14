// Ganchos de funções recompiladas do default.xbe (BLACK, NTSC-U, XDK 5849).
// Uma definição forte X86_FUNC(sub_XXXXXXXX) substitui a função gerada pelo recompilador;
// __imp__sub_XXXXXXXX chama a implementação original.
//
// Endereços de referência verificados em docs/re-log.md (fontes: black-seamless-coop [MIT] e auditoria do XBE):
//   0x0020F6A0  Frame tick principal (entrada de rede antes, snapshot/estado depois)
//   0x001DB7B0  Disparo de projétil do jogador (primário)
//   0x001DBD50  Disparo de projétil do jogador (secundário / variante)
//   0x001DF440  Spawn de personagem (ator)
//   0x00205C60  Registro / inicialização de ator no spawner
//   0x001DA240  Morte / baixa de ator (rotina A)
//   0x001DA1A0  Morte / baixa de ator (rotina B)
//   0x001E5F70  AI think (cérebro da IA: desligado ou substituído para atores remotos)
//   0x001CCBB0  Atualização por quadro de personagens
//   0x001AF880  Spawn de aliado de esquadrão (base para corpo de jogador remoto)
//   0x001AD440  Percepção de inimigos sobre alvos
//   0x001D54C0  Morte do jogador local (interceptar em multiplayer para respawn sem tela de falha)
//   0x001BD360  Estado de queda / abatido do jogador
//   0x001C6410  Fluxo pós-morte / recarga de checkpoint
//   0x0002B830  Câmera da cena (espectador / 3ª pessoa)

#include "mp/mp_bridge.h"
#include "xdk/hle.h"

// Gancho de quadro principal (Critério de aceite da Fase 0: gancho de teste que dispara e registra no log,
// mantendo a campanha intacta quando Role == OfflineCampaign).
X86_EXTERN_FUNC(__imp__sub_0020F6A0);
X86_FUNC(sub_0020F6A0) {
    mp::OnPreFrame(base);
    __imp__sub_0020F6A0(ctx, base);
    mp::OnPostFrame(base);
}
