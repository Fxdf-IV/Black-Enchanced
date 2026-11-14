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
        WeaponSpec{0, "Glock 19", 22, 5, 19, 80.0f},
        WeaponSpec{1, "DC3 Elite", 28, 5, 12, 85.0f},
        WeaponSpec{2, "Magnum", 55, 12, 6, 100.0f},
        WeaponSpec{3, "MP5", 20, 2, 30, 90.0f},
        WeaponSpec{4, "MAC-10", 18, 2, 32, 70.0f},
        WeaponSpec{5, "Uzi", 19, 2, 32, 75.0f},
        WeaponSpec{6, "P90", 21, 2, 50, 95.0f},
        WeaponSpec{7, "AK-47", 34, 3, 30, 160.0f},
        WeaponSpec{8, "M16A2", 30, 3, 30, 175.0f},
        WeaponSpec{9, "G36C", 29, 3, 30, 165.0f},
        WeaponSpec{10, "SPAS-12", 75, 15, 8, 35.0f},
        WeaponSpec{11, "Remington 870", 85, 18, 8, 32.0f},
        WeaponSpec{12, "M249 SAW", 32, 2, 100, 170.0f},
        WeaponSpec{13, "WA2000", 95, 20, 6, 300.0f},
        WeaponSpec{14, "M203", 100, 45, 1, 120.0f},
        WeaponSpec{15, "RPG-7", 100, 60, 1, 200.0f},
    };
    cfg.spawns = {
        SpawnPoint{Vec3{0.0f, 0.0f, 0.0f}, 0.0f},
        SpawnPoint{Vec3{32.0f, 0.0f, 0.0f}, 3.14159f},
        SpawnPoint{Vec3{0.0f, 0.0f, 32.0f}, -1.57079f},
        SpawnPoint{Vec3{32.0f, 0.0f, 32.0f}, 1.57079f},
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
    for (const auto& w : weapons) {
        h = fnv1a_u32(h, w.id);
        h = fnv1a_u32(h, w.damage);
        h = fnv1a_u32(h, w.min_fire_interval_ticks);
        h = fnv1a_u32(h, w.magazine_size);
        h = fnv1a_u32(h, std::bit_cast<uint32_t>(w.max_range));
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
