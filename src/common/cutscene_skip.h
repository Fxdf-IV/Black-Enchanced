#pragma once

#include <atomic>
#include <cstdint>

namespace common {

// Modo de pulo de cutscenes XMV, definido pelo cliente web (botão "Pular Intro / Start"):
//   0 = desligado (padrão; sem efeito no jogo nativo e na campanha)
//   1 = pula todas: todo filme em reprodução é marcado como terminado
//   2 = pula uma: o próximo filme em reprodução é marcado como terminado e o modo volta a 0
// O gancho de sub_000C4570 (atualização por quadro do player XMV) aplica o modo.
inline constexpr uint32_t kSkipOff = 0;
inline constexpr uint32_t kSkipAll = 1;
inline constexpr uint32_t kSkipOnce = 2;
inline std::atomic<uint32_t> g_skip_cutscenes{kSkipOff};

}  // namespace common
