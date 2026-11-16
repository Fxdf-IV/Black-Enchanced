#pragma once

#include <cstdint>

namespace input {
struct Pad;
}

namespace common {

// Flags de teclado para entrada via navegador (WebAssembly).
enum WebKeyFlag : uint32_t {
    kWebKeyW = 1u << 0,
    kWebKeyS = 1u << 1,
    kWebKeyA = 1u << 2,
    kWebKeyD = 1u << 3,
    kWebKeyUp = 1u << 4,
    kWebKeyDown = 1u << 5,
    kWebKeyLeft = 1u << 6,
    kWebKeyRight = 1u << 7,
    kWebKeyStart = 1u << 8,
    kWebKeyBack = 1u << 9,
    kWebKeyLShift = 1u << 10,
    kWebKeyCtrl = 1u << 11,
    kWebKeySpace = 1u << 12,
    kWebKeyC = 1u << 13,
    kWebKeyR = 1u << 14,
    kWebKeyE = 1u << 15,
    kWebKeyG = 1u << 16,
    kWebKeyQ = 1u << 17,
    kWebKeyV = 1u << 18,
    kWebKeyF = 1u << 19,
    kWebKeyY = 1u << 20,      // Swap de arma (alias do botão Y para roda do mouse / tecla)
    kWebKeyMelee = 1u << 21,  // Right Stick / Zoom-Focus / Melee
};

// Perfil "Enhanced Gameplay" (opt-in no navegador; padrão offline vanilla intacto quando desabilitado).
struct EnhancedConfig {
    bool mouse_direct = false;    // 1:1 linear sem deadzone/curva do analógico direito
    float mouse_sens = 0.085f;    // graus por pixel no modo 1:1
    float ads_sens_mult = 0.70f;  // multiplicador durante mira ADS
    bool invert_y = false;
    float fov_deg = 70.0f;        // 70 = vanilla Xbox; 70..110 = widescreen/PC
    bool hitmarker = true;
};

void SetEnhancedConfig(const EnhancedConfig& cfg);
EnhancedConfig GetEnhancedConfig();

// Consome deltas acumulados de mouse para rotação linear 1:1 no gancho sub_001BD3F0.
bool ConsumeDirectMouseLook(float& out_dx, float& out_dy);
void ClearDirectMouseLook();

// Atualiza estado de teclado e deltas acumulados de mouse enviados pelo navegador.
void SetWebKeyboardMouse(uint32_t key_mask, uint8_t mouse_buttons, float mouse_dx, float mouse_dy);

// Atualiza estado de gamepad (Gamepad API do navegador) na porta 0.
void SetWebGamepad(bool connected, uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly, int16_t rx,
                   int16_t ry);

// Preenche o controle da porta 0 com teclado/mouse no Windows / WebAssembly e, quando solicitado via
// BLACK_PAD_SCRIPT, injeta sequência determinística de teste sem alterar nenhum comportamento da campanha offline.
void PollPcPad(int port, input::Pad& pad);

}  // namespace common
