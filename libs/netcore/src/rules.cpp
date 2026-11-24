#include "netcore/rules.hpp"

#include <bit>
#include <cmath>

namespace netcore {

bool CaptureZone::contains(const Vec3& p) const {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(center.x) ||
        !std::isfinite(center.y) || !std::isfinite(center.z) || !std::isfinite(radius) || !std::isfinite(half_height) ||
        radius <= 0.0f || half_height <= 0.0f) {
        return false;
    }
    const float dx = p.x - center.x;
    const float dz = p.z - center.z;
    const float dy = std::fabs(p.y - center.y);
    return (dx * dx + dz * dz) <= (radius * radius) && dy <= half_height;
}

namespace {

uint32_t fnv1a_step(uint32_t h, uint8_t b) {
    return (h ^ b) * 16777619u;
}

uint32_t fnv1a_u32(uint32_t h, uint32_t v) {
    h = fnv1a_step(h, uint8_t(v));
    h = fnv1a_step(h, uint8_t(v >> 8));
    h = fnv1a_step(h, uint8_t(v >> 16));
    h = fnv1a_step(h, uint8_t(v >> 24));
    return h;
}

}  // namespace

RulesConfig RulesConfig::default_deathmatch() {
    RulesConfig cfg;
    cfg.weapons = {
        WeaponSpec{0, "Glock 19", 22, 5, 19, 80.0f, WeaponSlot::Secondary, 0},
        WeaponSpec{1, "DC3 Elite", 28, 5, 12, 85.0f, WeaponSlot::Secondary, 450},
        WeaponSpec{2, "Magnum", 55, 12, 6, 100.0f, WeaponSlot::Secondary, 700},
        WeaponSpec{3, "MP5", 20, 2, 30, 90.0f, WeaponSlot::Primary, 1250},
        WeaponSpec{4, "MAC-10", 18, 2, 32, 70.0f, WeaponSlot::Primary, 1050},
        WeaponSpec{5, "Uzi", 19, 2, 32, 75.0f, WeaponSlot::Primary, 1100},
        WeaponSpec{6, "P90", 21, 2, 50, 95.0f, WeaponSlot::Primary, 1800},
        WeaponSpec{7, "AK-47", 34, 3, 30, 160.0f, WeaponSlot::Primary, 2700},
        WeaponSpec{8, "M16A2", 30, 3, 30, 175.0f, WeaponSlot::Primary, 2900},
        WeaponSpec{9, "G36C", 29, 3, 30, 165.0f, WeaponSlot::Primary, 2400},
        WeaponSpec{10, "SPAS-12", 75, 15, 8, 35.0f, WeaponSlot::Primary, 1600},
        WeaponSpec{11, "Remington 870", 85, 18, 8, 32.0f, WeaponSlot::Primary, 1400},
        WeaponSpec{12, "M249 SAW", 32, 2, 100, 170.0f, WeaponSlot::Primary, 4500},
        WeaponSpec{13, "WA2000", 95, 20, 6, 300.0f, WeaponSlot::Primary, 4200},
        WeaponSpec{14, "M203", 100, 45, 1, 120.0f, WeaponSlot::Primary, 2200},
        WeaponSpec{15, "RPG-7", 100, 60, 1, 200.0f, WeaponSlot::Primary, 3600},
    };
    cfg.spawns = {
        SpawnPoint{Vec3{0.0f, 0.0f, 0.0f}, 0.0f},
        SpawnPoint{Vec3{32.0f, 0.0f, 0.0f}, 3.14159f},
        SpawnPoint{Vec3{0.0f, 0.0f, 32.0f}, -1.57079f},
        SpawnPoint{Vec3{32.0f, 0.0f, 32.0f}, 1.57079f},
    };
    return cfg;
}

RulesConfig RulesConfig::cs_loadout_deathmatch() {
    RulesConfig cfg = default_deathmatch();
    cfg.enforce_loadout = true;
    cfg.buy_window_ticks = 2700;  // 90 s a 30 Hz
    return cfg;
}

RulesConfig RulesConfig::territorial_assault() {
    RulesConfig cfg = default_deathmatch();
    cfg.game_mode = GameMode::TerritorialAssault;
    cfg.friendly_fire = false;
    cfg.kill_limit = 0;  // vitória é exclusivamente territorial (GDD §8.3)
    cfg.time_limit_s = 600;
    cfg.fixed_budget_mode = true;
    cfg.enforce_loadout = true;
    cfg.starting_credits = 15;
    cfg.max_credits = 15;  // Supply Points individuais fixos (GDD §5.1)
    cfg.kill_reward_credits = 0;
    cfg.death_bonus_credits = 0;
    cfg.armor_price = 3;
    cfg.tactical_price = 2;
    cfg.buy_window_ticks = 0;  // controlado por fase (Preparation/Intermission abertos; combate bloqueado)

    // Preços em Supply Points para o arsenal nativo de BLACK (AGENTS.md §2.2).
    const uint16_t sp_prices[16] = {
        0,   // 0: Glock 19 (secundária padrão)
        1,   // 1: DC3 Elite
        2,   // 2: Magnum
        4,   // 3: MP5
        3,   // 4: MAC-10
        3,   // 5: Uzi
        5,   // 6: P90
        7,   // 7: AK-47
        7,   // 8: M16A2
        6,   // 9: G36C
        5,   // 10: SPAS-12
        4,   // 11: Remington 870
        9,   // 12: M249 SAW
        9,   // 13: WA2000
        8,   // 14: M203
        10,  // 15: RPG-7
    };
    for (auto& w : cfg.weapons) {
        if (w.id < 16) w.price = sp_prices[w.id];
    }

    cfg.territorial = ta::Config{};
    cfg.territorial.objective_count = 2;  // protótipo inicial: 2 objetivos sequenciais (GDD §17)
    cfg.territorial.supply_points = 15;
    cfg.objective_zones = {
        CaptureZone{Vec3{20.0f, 0.0f, 0.0f}, 8.0f, 5.0f},
        CaptureZone{Vec3{50.0f, 0.0f, 0.0f}, 8.0f, 5.0f},
    };
    cfg.attacker_spawns = {
        SpawnPoint{Vec3{0.0f, 0.0f, -4.0f}, 0.0f},
        SpawnPoint{Vec3{0.0f, 0.0f, 4.0f}, 0.0f},
    };
    cfg.defender_spawns = {
        SpawnPoint{Vec3{70.0f, 0.0f, -4.0f}, 3.14159f},
        SpawnPoint{Vec3{70.0f, 0.0f, 4.0f}, 3.14159f},
    };
    return cfg;
}

uint32_t RulesConfig::compute_hash() const {
    uint32_t h = 2166136261u;
    h = fnv1a_u32(h, tick_rate_hz);
    h = fnv1a_u32(h, snapshot_rate_hz);
    h = fnv1a_u32(h, max_players);
    h = fnv1a_u32(h, max_health);
    h = fnv1a_u32(h, kill_limit);
    h = fnv1a_u32(h, time_limit_s);
    h = fnv1a_u32(h, respawn_delay_ticks);
    h = fnv1a_u32(h, std::bit_cast<uint32_t>(max_speed_units_per_sec));
    h = fnv1a_u32(h, arena_id);
    h = fnv1a_u32(h, static_cast<uint32_t>(game_mode));
    h = fnv1a_u32(h, friendly_fire ? 1u : 0u);
    h = fnv1a_u32(h, fixed_budget_mode ? 1u : 0u);
    h = fnv1a_u32(h, enforce_loadout ? 1u : 0u);
    h = fnv1a_u32(h, starting_credits);
    h = fnv1a_u32(h, max_credits);
    h = fnv1a_u32(h, kill_reward_credits);
    h = fnv1a_u32(h, death_bonus_credits);
    h = fnv1a_u32(h, armor_price);
    h = fnv1a_u32(h, armor_bonus_health);
    h = fnv1a_u32(h, tactical_price);
    h = fnv1a_u32(h, buy_window_ticks);
    for (const auto& w : weapons) {
        h = fnv1a_u32(h, w.id);
        h = fnv1a_u32(h, w.damage);
        h = fnv1a_u32(h, w.min_fire_interval_ticks);
        h = fnv1a_u32(h, w.magazine_size);
        h = fnv1a_u32(h, std::bit_cast<uint32_t>(w.max_range));
        h = fnv1a_u32(h, static_cast<uint8_t>(w.slot));
        h = fnv1a_u32(h, w.price);
    }
    if (game_mode == GameMode::TerritorialAssault) {
        h = fnv1a_u32(h, territorial.objective_count);
        h = fnv1a_u32(h, territorial.capture_ticks);
        h = fnv1a_u32(h, territorial.attacker_initial_waves);
        h = fnv1a_u32(h, territorial.defender_initial_waves);
        h = fnv1a_u32(h, territorial.wave_interval_ticks);
        h = fnv1a_u32(h, territorial.waves_per_objective);
        h = fnv1a_u32(h, territorial.max_waves);
        h = fnv1a_u32(h, territorial.initial_time_ticks);
        h = fnv1a_u32(h, territorial.bonus_time_ticks);
        h = fnv1a_u32(h, territorial.max_time_ticks);
        h = fnv1a_u32(h, territorial.overtime_enabled ? 1u : 0u);
        h = fnv1a_u32(h, territorial.overtime_cap_ticks);
        h = fnv1a_u32(h, territorial.round2_early_end ? 1u : 0u);
        h = fnv1a_u32(h, territorial.supply_points);
    }
    return h;
}

const WeaponSpec* RulesConfig::find_weapon(uint8_t weapon_id) const {
    for (const auto& w : weapons) {
        if (w.id == weapon_id) return &w;
    }
    return nullptr;
}

}  // namespace netcore
