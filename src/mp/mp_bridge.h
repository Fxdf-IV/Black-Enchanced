#pragma once
// Ponte entre o binário recompilado de BLACK (Xbox NTSC-U) e o núcleo de rede independente (libs/netcore).
// Arquivos mantidos em um único nível dentro de src/mp/ para casar com o glob src/*/*.cpp do SDK.

#include <cstdint>

namespace mp {

enum class Role : uint8_t {
    OfflineCampaign = 0,  // Campanha intacta (Black Enhanced): nenhuma lógica de multiplayer interfere
    Host = 1,             // Listen-server autoritativo (Black Enhanced Online)
    Client = 2,           // Cliente conectado a um host
};

// Inicializa a ponte multiplayer (lê flags/configurações sem alterar o modo campanha padrão).
void InitBridge();

// Chamado quando a imagem convidada (default.xbe) é carregada na memória base (g_base).
void OnImageLoaded(uint8_t* guest_base);

// Chamado antes e depois da função de quadro do jogo (sub_0020F6A0).
void OnPreFrame(uint8_t* guest_base);
void OnPostFrame(uint8_t* guest_base);

// Teste #1 / #2 da Fase 2: vincula um ator convidado (ex.: aliado criado via 0x1AF880) a um slot de jogador remoto.
void BindRemoteActor(uint8_t player_id, uint32_t actor_va);
void UnbindRemoteActor(uint8_t player_id);
bool IsRemoteActor(uint32_t actor_va);

// Teste #2 da Fase 2 (sub_001E5F70): retorna true se o controlador pertence a um ator remoto cujo cérebro
// de IA deve ser ignorado neste quadro.
bool ShouldSkipAiThink(uint32_t controller_va, const uint8_t* guest_base);

// Testes #7 e #8 da Fase 2 (sub_001D54C0 / sub_001BD360): em partida multiplayer, captura os dados do jogador
// local antes da morte e impede que o jogo dispare "Missão Fracassada" enquanto aguarda o RespawnEvent.
void OnLocalPlayerDying(uint8_t* guest_base);
bool ShouldSuppressMissionFailed(const uint8_t* guest_base);

// Papel atual da instância. Por padrão é OfflineCampaign (garante que a campanha nunca quebre).
Role CurrentRole();
void SetRole(Role r);

uint64_t FrameCount();

}  // namespace mp
