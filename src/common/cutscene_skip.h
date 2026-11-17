#pragma once

#include <atomic>

namespace common {

// Quando verdadeiro, o gancho de sub_00209D40 (máquina de estados de cutscene) encerra o vídeo XMV em
// reprodução marcando-o como terminado. Definido pelo cliente web (botão "Pular Intro / Start").
// Padrão falso: sem efeito no jogo nativo e na campanha.
inline std::atomic<bool> g_skip_cutscenes{false};

}  // namespace common
