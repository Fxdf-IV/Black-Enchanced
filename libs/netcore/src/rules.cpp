#include "netcore/rules.hpp"

#include <bit>

namespace netcore {

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
    return h;
}

const WeaponSpec* RulesConfig::find_weapon(uint8_t weapon_id) const {
    for (const auto& w : weapons) {
        if (w.id == weapon_id) return &w;
    }
    return nullptr;
}

}  // namespace netcore
