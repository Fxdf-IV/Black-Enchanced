#pragma once
// Regras de partida e balanceamento de armas para o Deathmatch (MVP 0.1).
// O hash FNV-1a das regras é enviado no Hello para garantir que cliente e host usam a mesma tabela.

#include <array>
#include <cstdint>
#include <vector>

#include "netcore/limits.hpp"
#include "netcore/protocol.hpp"

namespace netcore {

struct WeaponSpec {
    uint8_t id = 0;
    const char* name = "";
    uint16_t damage = 25;
    uint16_t min_fire_interval_ticks = 3;  // a 30 Hz: 3 ticks = 100 ms (600 RPM)
    uint16_t magazine_size = 30;
    float max_range = 250.0f;
};

struct SpawnPoint {
    Vec3 position;
    float yaw = 0.0f;
};

struct RulesConfig {
    uint16_t tick_rate_hz = 30;       // Taxa de simulação interna de BLACK (30 FPS)
    uint16_t snapshot_rate_hz = 15;   // Snapshots de mundo por segundo
    uint8_t max_players = 4;          // MVP 0.1: até 4 jogadores
    uint8_t max_health = 100;
    uint16_t kill_limit = 15;
    uint16_t time_limit_s = 600;      // 10 minutos
    uint16_t respawn_delay_ticks = 90;  // 3 segundos a 30 Hz
    float max_speed_units_per_sec = 18.0f;
    uint8_t arena_id = 0;

    // Limites de segurança contra abuso de mensagens por cliente (seção 9 do plano)
    uint16_t max_packets_per_window = 120;  // janela de 30 ticks (1 segundo)
    uint8_t max_violations_before_kick = 5;

    std::vector<WeaponSpec> weapons;
    std::vector<SpawnPoint> spawns;

    // Configuração padrão do MVP 0.1 com as armas clássicas de BLACK.
    static RulesConfig default_deathmatch();

    // Hash determinístico FNV-1a (32 bits) dos parâmetros que afetam a simulação.
    uint32_t compute_hash() const;

    const WeaponSpec* find_weapon(uint8_t weapon_id) const;
};

}  // namespace netcore
