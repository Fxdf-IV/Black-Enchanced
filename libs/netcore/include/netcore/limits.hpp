#pragma once
// Limites globais do protocolo. Mudar qualquer valor aqui exige subir kProtocolVersion.

#include <cstddef>
#include <cstdint>

namespace netcore {

// Versão do protocolo de rede. Cliente e host recusam a conexão quando divergem.
inline constexpr uint16_t kProtocolVersion = 3;

// Marca no início de todo pacote, para descartar lixo cedo.
inline constexpr uint16_t kPacketMagic = 0xB1AC;

// MVP 0.1: até 4 jogadores (host incluso).
inline constexpr std::size_t kMaxPlayers = 4;

// Teto de um pacote. 1200 bytes cabe no MTU típico de WebRTC/UDP sem fragmentação.
inline constexpr std::size_t kMaxPacketSize = 1200;

inline constexpr std::size_t kMaxNameLength = 16;
inline constexpr std::size_t kMaxWeapons = 32;
inline constexpr std::size_t kMaxSpawnPoints = 64;

// Coordenadas fora deste cubo são tratadas como inválidas (unidades do mundo do jogo).
inline constexpr float kWorldLimit = 100000.0f;
inline constexpr float kVelocityLimit = 1000.0f;
inline constexpr float kAngleLimit = 7.0f;  // radianos; folga para ângulos não normalizados

// "Nenhum jogador" / "Nenhuma arma".
inline constexpr uint8_t kNoPlayer = 0xFF;
inline constexpr uint8_t kNoWeapon = 0xFF;

}  // namespace netcore
