#pragma once
// Regras de partida e balanceamento de armas para o Deathmatch (MVP 0.1).
// O hash FNV-1a das regras é enviado no Hello para garantir que cliente e host usam a mesma tabela.

#include <array>
#include <cstdint>
#include <vector>

#include "netcore/limits.hpp"
#include "netcore/protocol.hpp"
#include "netcore/territorial.hpp"

namespace netcore {

enum class WeaponSlot : uint8_t {
    Secondary = 0,
    Primary = 1,
    Tactical = 2,
};

struct WeaponSpec {
    uint8_t id = 0;
    const char* name = "";
    uint16_t damage = 25;
    uint16_t min_fire_interval_ticks = 3;  // a 30 Hz: 3 ticks = 100 ms (600 RPM)
    uint16_t magazine_size = 30;
    float max_range = 250.0f;
    WeaponSlot slot = WeaponSlot::Primary;
    uint16_t price = 0;
};

struct SpawnPoint {
    Vec3 position;
    float yaw = 0.0f;
};

// Região espacial de um objetivo territorial (separada da lógica de modo, configurável por mapa).
struct CaptureZone {
    Vec3 center{};
    float radius = 10.0f;       // raio horizontal no plano XZ
    float half_height = 5.0f;   // tolerância vertical no eixo Y

    bool contains(const Vec3& p) const;
};

enum class GameMode : uint8_t {
    PvP = 0,
    Coop = 1,
    TerritorialAssault = 2,
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
    GameMode game_mode = GameMode::PvP;
    bool friendly_fire = false;

    // Economia e Loadout estilo CS (teto e compra validados pelo host)
    bool fixed_budget_mode = false;
    bool enforce_loadout = false;
    uint16_t starting_credits = 1500;
    uint16_t max_credits = 8000;
    uint16_t kill_reward_credits = 450;
    uint16_t death_bonus_credits = 600;
    uint16_t armor_price = 650;
    uint8_t armor_bonus_health = 25;
    uint16_t tactical_price = 300;
    uint16_t buy_window_ticks = 450;  // 15 s a 30 Hz após spawn (0 = sempre aberto)

    // Limites de segurança contra abuso de mensagens por cliente (seção 9 do plano)
    uint16_t max_packets_per_window = 120;  // janela de 30 ticks (1 segundo)
    uint8_t max_violations_before_kick = 5;

    std::vector<WeaponSpec> weapons;
    std::vector<SpawnPoint> spawns;

    // Configuração específica do modo Territorial Assault (usada somente quando game_mode == TerritorialAssault).
    ta::Config territorial{};
    uint32_t ta_intermission_ticks = 0;  // 0 = avanço manual via begin_next_round(); >0 = automático
    std::vector<CaptureZone> objective_zones;
    std::vector<SpawnPoint> attacker_spawns;
    std::vector<SpawnPoint> defender_spawns;

    // Configuração padrão do MVP 0.1 com as armas clássicas de BLACK.
    static RulesConfig default_deathmatch();

    // Variante com economia e posse de loadout estritamente impostas no host.
    static RulesConfig cs_loadout_deathmatch();

    // Variante Territorial Assault com armas nativas de BLACK precificadas em Supply Points (15 SP).
    static RulesConfig territorial_assault();

    // Hash determinístico FNV-1a (32 bits) dos parâmetros que afetam a simulação.
    uint32_t compute_hash() const;

    const WeaponSpec* find_weapon(uint8_t weapon_id) const;
};

}  // namespace netcore
