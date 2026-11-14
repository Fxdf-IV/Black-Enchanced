#pragma once
// Ponte entre o binário recompilado de BLACK e o núcleo de rede independente (libs/netcore).
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

// Papel atual da instância. Por padrão é OfflineCampaign (garante que a campanha nunca quebre).
Role CurrentRole();
void SetRole(Role r);

uint64_t FrameCount();

}  // namespace mp
