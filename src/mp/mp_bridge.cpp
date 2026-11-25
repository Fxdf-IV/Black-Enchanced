#include "mp/mp_bridge.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/pc_input.h"
#include "core/log.h"
#include "game/black_addrs.h"
#include "kernel/memory.h"
#ifndef __EMSCRIPTEN__
#include "mp/mp_udp.h"
#endif
#include "mp/mp_webrtc.h"
#include "netcore/limits.hpp"
#include "netcore/rules.hpp"
#include "netcore/session.hpp"
#include "netcore/transport.hpp"

namespace mp {

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;

Role g_role = Role::OfflineCampaign;
uint64_t g_frame_count = 0;
uint8_t* g_guest_base = nullptr;

// Ator convidado (VA de 32 bits) associado a cada slot de jogador (0..kMaxPlayers-1).
std::array<uint32_t, netcore::kMaxPlayers> g_remote_actor_va{};
std::array<bool, netcore::kMaxPlayers> g_remote_spawned_dedicated{};
std::array<bool, netcore::kMaxPlayers> g_remote_dedicated_failed{};
std::array<netcore::SnapshotPlayer, netcore::kMaxPlayers> g_last_applied_snapshot{};
std::array<bool, netcore::kMaxPlayers> g_has_applied_snapshot{};
std::array<uint8_t, netcore::kMaxPlayers> g_remote_applied_weapon{
    netcore::kNoWeapon, netcore::kNoWeapon, netcore::kNoWeapon, netcore::kNoWeapon};
std::array<uint8_t, netcore::kMaxPlayers> g_pending_remote_weapon{
    netcore::kNoWeapon, netcore::kNoWeapon, netcore::kNoWeapon, netcore::kNoWeapon};
uint8_t g_pending_remote_spawn_mask = 0;

// Memória temporária alinhada no guest (alloc preguiçosa e reutilizada) para chamadas ::hle::Call.
uint32_t g_guest_scratch_va = 0;
uint32_t g_spawn_scratch_va = 0;

// Dados salvos do jogador local para restauração no respawn sem recarregar a fase (Testes #7 e #8 da Fase 2).
uint32_t g_saved_player_controller = 0;
uint32_t g_saved_player_name[2] = {};
float g_local_max_health = 100.0f;
bool g_local_waiting_respawn = false;
bool g_awaiting_alive_snapshot = false;
bool g_level_spawns_initialized = false;
bool g_client_join_logged = false;
uint64_t g_last_client_hello_frame = 0;
netcore::Vec3 g_last_local_pos{};
bool g_has_last_local_pos = false;
bool g_prev_local_firing = false;
uint32_t g_last_local_fire_tick = 0;
bool g_in_local_weapon_fire = false;
bool g_saw_weapon_fire_hook = false;
uint8_t g_local_shot_claimed_target = netcore::kNoPlayer;
uint32_t g_local_weapon_va = 0;
std::vector<netcore::FireEvent> g_pending_remote_fires;

std::unique_ptr<netcore::LoopbackEndpoint> g_local_endpoint;
#ifndef __EMSCRIPTEN__
std::unique_ptr<UdpTransport> g_udp_transport;
#endif
std::unique_ptr<WebRtcTransport> g_webrtc_transport;
std::unique_ptr<netcore::HostSession> g_host_session;
std::unique_ptr<netcore::ClientSession> g_client_session;

std::recursive_mutex g_bridge_mu;
std::atomic<uint64_t> g_hitmarker_seq{0};

bool g_pending_weapon_apply = false;
uint8_t g_pending_equip_slot = 0;
uint8_t g_last_synced_primary = netcore::kNoWeapon;
uint8_t g_last_synced_secondary = 0;
uint8_t g_last_synced_tactical = 0;
bool g_local_fps_control_started = false;
std::chrono::steady_clock::time_point g_last_host_tick_tp{};
double g_host_tick_accum_sec = 0.0;
std::chrono::steady_clock::time_point g_last_local_pos_tp{};
std::chrono::steady_clock::time_point g_client_tick_origin_tp{};
bool g_has_pending_slot_switch = false;
uint8_t g_pending_slot_switch = 0;
uint32_t g_last_weapon_def_dump_lvl = 0xFFFFFFFFu;
std::string g_active_player_name = "Operador";

// Estado de pré-lobby / carregamento direto de mapa (usado apenas no Multiplayer; nunca afeta Campanha Solo).
std::atomic<bool> g_direct_launch_pending{false};
bool g_prematch_configured = false;
uint8_t g_prematch_mission_idx = 0;
uint8_t g_prematch_team = 0;
uint8_t g_prematch_primary = 7;    // AK-47 padrão nativo
uint8_t g_prematch_secondary = 0;  // Glock 19
uint8_t g_prematch_tactical = 0;
uint32_t g_direct_launch_fe_frames = 0;
bool g_prematch_team_applied = false;
bool g_prematch_kit_applied = false;
uint32_t g_host_auto_round_frames = 0;
bool g_host_auto_round_started = false;

float clamp_angle_rad(float rad) {
    if (!std::isfinite(rad)) return 0.0f;
    rad = std::fmod(rad, 6.28318530f);
    if (rad > 3.14159265f) rad -= 6.28318530f;
    if (rad < -3.14159265f) rad += 6.28318530f;
    return rad;
}

bool IsGuestCommitted(uint32_t va, uint32_t bytes) {
    if (bytes == 0 || va < mem::kUserLow) return false;
    const uint64_t end = uint64_t(va) + bytes - 1;
    if (end > 0xFFFFFFFFull) return false;
    if (va >= mem::kContiguousBase && end < uint64_t(mem::kContiguousBase) + mem::kPhysicalSize) {
        return true;
    }
    if (va >= mem::kWriteCombineBase && end < uint64_t(mem::kWriteCombineBase) + mem::kPhysicalSize) {
        return true;
    }
    const uint32_t first_page = va & ~(mem::kPage - 1u);
    const uint32_t last_page = static_cast<uint32_t>(end) & ~(mem::kPage - 1u);
    for (uint32_t p = first_page;; p += mem::kPage) {
        if (!mem::IsCommitted(p)) return false;
        if (p == last_page) break;
    }
    return true;
}

netcore::Vec3 BuildLocalCameraDirection(const netcore::PlayerState& state) {
    const float cp = std::cos(state.pitch);
    return netcore::Vec3{
        cp * std::sin(state.yaw),
        -std::sin(state.pitch),
        cp * std::cos(state.yaw),
    };
}

bool ReadLocalAimOriginAndDir(const uint8_t* base, netcore::Vec3& out_origin, netcore::Vec3& out_dir) {
    if (!black::IsInLevel(base)) return false;
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!IsGuestCommitted(player, 0x3C0)) return false;
    const float x = black::RF32(base, player + black::kCharPos + 0);
    const float y = black::RF32(base, player + black::kCharPos + 4);
    const float z = black::RF32(base, player + black::kCharPos + 8);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;

    float yaw_rad = 0.0f;
    float pitch_rad = 0.0f;
    const uint32_t world = black::R32(base, black::kWorld);
    const uint32_t ctrl = world ? (world + black::kWorldController) : 0;
    if (IsGuestCommitted(ctrl, 0x20)) {
        yaw_rad = clamp_angle_rad(black::RF32(base, ctrl + 0x18) * kDegToRad);
        pitch_rad = clamp_angle_rad(black::RF32(base, ctrl + 0x1C) * kDegToRad);
    }
    netcore::PlayerState tmp{};
    tmp.yaw = yaw_rad;
    tmp.pitch = pitch_rad;
    out_origin = netcore::Vec3{x, y, z};
    out_dir = BuildLocalCameraDirection(tmp);
    return true;
}

uint8_t FindRemoteSlotByActor(uint32_t actor_va) {
    if (actor_va == 0) return netcore::kNoPlayer;
    for (std::size_t i = 0; i < netcore::kMaxPlayers; ++i) {
        if (g_remote_actor_va[i] == actor_va) {
            return static_cast<uint8_t>(i);
        }
    }
    return netcore::kNoPlayer;
}

void ReleaseDedicatedRemoteActor(uint8_t* base, uint8_t slot) {
    if (!base || slot >= netcore::kMaxPlayers) return;
    const uint32_t actor_va = g_remote_actor_va[slot];
    if (actor_va && g_remote_spawned_dedicated[slot] && IsGuestCommitted(actor_va, 0x3C0)) {
        const uint32_t ctrl = black::R32(base, actor_va + black::kCharControl);
        if (ctrl && IsGuestCommitted(ctrl, 0x1FD4)) {
            // Sinaliza ao motor (sub_001DEC20) que este AIController deve voltar para a lista livre (+0x22CDC).
            black::W8(base, ctrl + 0x1FD0u, 1u);
        }
        black::W8(base, actor_va + black::kCharKinematic, 0u);
        black::W32(base, actor_va + black::kCharState, 2u);
        black::W32(base, actor_va + black::kCharName + 0u, 0u);
        black::W32(base, actor_va + black::kCharName + 4u, 0u);
        LOGR("mp_bridge: released dedicated actor 0x%08X for slot %u", actor_va, static_cast<unsigned>(slot));
    }
    g_remote_spawned_dedicated[slot] = false;
    g_remote_dedicated_failed[slot] = false;
    g_pending_remote_spawn_mask &= static_cast<uint8_t>(~(1u << slot));
}

void ResetLevelState() {
    g_remote_actor_va.fill(0);
    g_remote_spawned_dedicated.fill(false);
    g_remote_dedicated_failed.fill(false);
    g_pending_remote_spawn_mask = 0;
    g_has_applied_snapshot.fill(false);
    g_last_applied_snapshot = {};
    g_remote_applied_weapon.fill(netcore::kNoWeapon);
    g_pending_remote_weapon.fill(netcore::kNoWeapon);
    g_saved_player_controller = 0;
    g_saved_player_name[0] = 0;
    g_saved_player_name[1] = 0;
    g_local_max_health = 100.0f;
    g_local_waiting_respawn = false;
    g_awaiting_alive_snapshot = false;
    g_level_spawns_initialized = false;
    g_has_last_local_pos = false;
    g_prev_local_firing = false;
    g_last_local_fire_tick = 0;
    g_in_local_weapon_fire = false;
    g_saw_weapon_fire_hook = false;
    g_local_shot_claimed_target = netcore::kNoPlayer;
    g_local_weapon_va = 0;
    g_pending_remote_fires.clear();
    g_pending_weapon_apply = false;
    g_pending_equip_slot = 0;
    g_has_pending_slot_switch = false;
    g_pending_slot_switch = 0;
    g_last_weapon_def_dump_lvl = 0xFFFFFFFFu;
    g_last_synced_primary = netcore::kNoWeapon;
    g_last_synced_secondary = 0;
    g_last_synced_tactical = 0;
    g_local_fps_control_started = false;
    g_last_host_tick_tp = {};
    g_host_tick_accum_sec = 0.0;
    g_last_local_pos_tp = {};
    g_client_tick_origin_tp = {};
}

netcore::RulesConfig BuildActiveRules(bool cs_loadout, bool fixed_budget, uint16_t budget_cap, uint8_t game_mode = 0) {
    if (game_mode == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault)) {
        netcore::RulesConfig cfg = netcore::RulesConfig::territorial_assault();
        return cfg;
    }
    netcore::RulesConfig cfg = cs_loadout ? netcore::RulesConfig::cs_loadout_deathmatch()
                                          : netcore::RulesConfig::default_deathmatch();
    if (game_mode == static_cast<uint8_t>(netcore::GameMode::Coop)) {
        cfg.game_mode = netcore::GameMode::Coop;
        cfg.friendly_fire = false;
    }
    // 20 minutos para partidas web sem interrupção prematura durante navegação/duelos.
    cfg.time_limit_s = 1200;
    cfg.fixed_budget_mode = fixed_budget;
    if (fixed_budget) {
        cfg.buy_window_ticks = 0;
    }
    if (budget_cap >= 450u) {
        cfg.max_credits = budget_cap;
        if (fixed_budget) cfg.starting_credits = budget_cap;
    }
    return cfg;
}

static constexpr const char* kCatalogNativeTags[16] = {
    "BG1_GK1",  // 0: Glock 19
    "BG1_PST",  // 1: DC3 Elite
    "BG1_MGN",  // 2: Magnum
    "BG1_MP1",  // 3: MP5
    "BG1_SM3",  // 4: MAC-10
    "BG1_SMG",  // 5: Uzi
    "BG1_P90",  // 6: P90
    "BG1_AK1",  // 7: AK-47
    "BG1_M16",  // 8: M16A2
    "BG1_ASR",  // 9: G36C
    "BG1_SHG",  // 10: SPAS-12
    "BG1_RM1",  // 11: Remington 870
    "BG1_HVY",  // 12: M249 SAW
    "BG1_SNR",  // 13: WA2000
    "BG1_GRL",  // 14: M203
    "BG1_RPG",  // 15: RPG-7
};

void DecodeRadix40Tag(uint32_t lo, uint32_t hi, char out_buf[13]) {
    uint64_t v = (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
    char tmp[12];
    for (int i = 11; i >= 0; --i) {
        const uint32_t r = static_cast<uint32_t>(v % 40u);
        v /= 40u;
        char c = ' ';
        if (r == 0x27u) c = '_';
        else if (r >= 0x0Du) c = static_cast<char>(r + 0x34u);
        else if (r >= 0x03u) c = static_cast<char>(r + 0x2Du);
        else if (r == 2u) c = '/';
        else if (r == 1u) c = '-';
        tmp[i] = c;
    }
    int len = 12;
    while (len > 0 && tmp[len - 1] == ' ') --len;
    int start = 0;
    while (start < len && tmp[start] == ' ') ++start;
    int out_i = 0;
    for (int i = start; i < len && out_i < 12; ++i) {
        out_buf[out_i++] = tmp[i];
    }
    out_buf[out_i] = '\0';
}

uint32_t ComputeLoadedCatalogMapping(const uint8_t* base,
                                     uint8_t out_cat_to_eng[16],
                                     uint8_t out_eng_to_cat[32],
                                     uint32_t* out_def_count = nullptr) {
    for (int c = 0; c < 16; ++c) out_cat_to_eng[c] = 0xFFu;
    for (int e = 0; e < 32; ++e) out_eng_to_cat[e] = 0xFFu;
    if (out_def_count) *out_def_count = 0;
    if (!base || !IsGuestCommitted(black::kWeaponSystem, 4)) return 0xFFFFFFFFu;
    const uint32_t ws = black::R32(base, black::kWeaponSystem);
    if (!ws || !IsGuestCommitted(ws, 12)) return 0xFFFFFFFFu;
    const uint32_t def_hdr = black::R32(base, ws);
    if (!def_hdr || !IsGuestCommitted(def_hdr, 8)) return 0xFFFFFFFFu;
    const uint32_t cnt = static_cast<uint32_t>(black::R8(base, def_hdr + 1u));
    if (out_def_count) *out_def_count = cnt;
    if (cnt == 0) return 0xFFFFFFFFu;
    const uint32_t arr = black::R32(base, def_hdr + 4u);
    if (!arr || !IsGuestCommitted(arr, std::min<uint32_t>(cnt, 32u) * 32u)) return 0xFFFFFFFFu;

    uint32_t mask = 0;
    const uint32_t limit = std::min<uint32_t>(cnt, 32u);
    for (uint32_t i = 0; i < limit; ++i) {
        const uint32_t d = arr + i * 32u;
        const uint32_t lo = black::R32(base, d);
        const uint32_t hi = black::R32(base, d + 4u);
        char tag[13];
        DecodeRadix40Tag(lo, hi, tag);
        for (int c = 0; c < 16; ++c) {
            if (out_cat_to_eng[c] == 0xFFu && std::strcmp(tag, kCatalogNativeTags[c]) == 0) {
                out_cat_to_eng[c] = static_cast<uint8_t>(i);
                out_eng_to_cat[i] = static_cast<uint8_t>(c);
                mask |= (1u << c);
                break;
            }
        }
    }
    // Se BG1_M16 (8) faltar mas BG1_BNS existir na tabela, vincula apenas como reserva M16A2.
    if (out_cat_to_eng[8] == 0xFFu) {
        for (uint32_t i = 0; i < limit; ++i) {
            const uint32_t d = arr + i * 32u;
            char tag[13];
            DecodeRadix40Tag(black::R32(base, d), black::R32(base, d + 4u), tag);
            if (std::strcmp(tag, "BG1_BNS") == 0) {
                out_cat_to_eng[8] = static_cast<uint8_t>(i);
                out_eng_to_cat[i] = 8u;
                mask |= (1u << 8);
                break;
            }
        }
    }
    return mask == 0 ? 0xFFFFFFFFu : mask;
}

uint32_t ReadLoadedWeaponDefCount(const uint8_t* base) {
    uint32_t cnt = 0;
    uint8_t c2e[16]{};
    uint8_t e2c[32]{};
    const uint32_t mask = ComputeLoadedCatalogMapping(base, c2e, e2c, &cnt);
    static uint32_t s_last_logged_world = 0;
    if (cnt > 0 && black::IsInLevel(base)) {
        const uint32_t world = IsGuestCommitted(black::kWorld, 4) ? black::R32(base, black::kWorld) : 0u;
        if (world != 0 && world != s_last_logged_world) {
            s_last_logged_world = world;
            const uint8_t lvl = black::R8(base, world + black::kWorldLevel);
            LOGR("mp_bridge: level=%u loaded %u native WeaponDef(s) (catalog_mask=0x%08X)",
                 static_cast<unsigned>(lvl), cnt, mask);
            const uint32_t ws = black::R32(base, black::kWeaponSystem);
            const uint32_t def_hdr = ws ? black::R32(base, ws) : 0u;
            const uint32_t arr = def_hdr ? black::R32(base, def_hdr + 4u) : 0u;
            for (uint32_t i = 0; i < std::min<uint32_t>(cnt, 20u) && arr; ++i) {
                const uint32_t d = arr + i * 32u;
                char tag[13]{};
                DecodeRadix40Tag(black::R32(base, d), black::R32(base, d + 4u), tag);
                const uint32_t w2 = black::R32(base, d + 8u);
                const uint32_t w3 = black::R32(base, d + 12u);
                const uint32_t w4 = black::R32(base, d + 16u);
                const uint32_t w5 = black::R32(base, d + 20u);
                const uint32_t w6 = black::R32(base, d + 24u);
                const uint32_t w7 = black::R32(base, d + 28u);
                LOGR("  WDEF[%u] %s w2=0x%08X w3=0x%08X w4=0x%08X w5=0x%08X w6=0x%08X w7=0x%08X",
                     i, tag, w2, w3, w4, w5, w6, w7);
            }
        }
    } else if (!base || !black::IsInLevel(base)) {
        s_last_logged_world = 0;
    }
    return cnt;
}

// Mapeia o ID do catálogo (0..15) exatamente pelo tag Radix-40 BG1_XXX carregado na fase atual.
uint8_t MapCatalogToEngineWeaponId(const uint8_t* base, uint8_t catalog_id, uint32_t def_count) {
    if (catalog_id >= 16u || def_count == 0 || !base) return 0xFFu;
    uint8_t c2e[16]{};
    uint8_t e2c[32]{};
    (void)ComputeLoadedCatalogMapping(base, c2e, e2c);
    return c2e[catalog_id];
}

uint8_t ReadLocalEquippedCatalogWeapon(const uint8_t* base) {
    if (!base || !black::IsInLevel(base)) return 0;
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!IsGuestCommitted(player, 0x2C0)) return 0;

    uint8_t prim = netcore::kNoWeapon;
    uint8_t sec = 0;
    bool loadout_active = false;
    if (g_role == Role::Host && g_host_session && g_host_session->rules().enforce_loadout) {
        const auto ls = g_host_session->slot_loadout_state(0);
        prim = ls.primary_weapon;
        sec = ls.secondary_weapon;
        loadout_active = true;
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined() &&
               g_client_session->local_loadout().max_credits > 0) {
        const auto ls = g_client_session->local_loadout();
        prim = ls.primary_weapon;
        sec = ls.secondary_weapon;
        loadout_active = true;
    }

    if (loadout_active) {
        const uint8_t slot = black::R8(base, player + black::kCharActiveSlot);
        if (prim != netcore::kNoWeapon && sec != netcore::kNoWeapon) {
            return (slot == 1u) ? sec : prim;
        }
        if (prim != netcore::kNoWeapon) return prim;
        if (sec != netcore::kNoWeapon) return sec;
        return 0;
    }

    const uint8_t raw_eng = black::R8(base, player + black::kCharActiveWeaponId);
    if (raw_eng < 32u) {
        uint8_t c2e[16]{};
        uint8_t e2c[32]{};
        uint32_t cnt = 0;
        (void)ComputeLoadedCatalogMapping(base, c2e, e2c, &cnt);
        if (cnt > 0 && e2c[raw_eng] < netcore::kMaxWeapons) {
            return e2c[raw_eng];
        }
    }
    return (raw_eng < netcore::kMaxWeapons) ? raw_eng : static_cast<uint8_t>(0);
}

void EmitLocalFireEvent(const uint8_t* base, uint32_t tick, uint8_t weapon_id, uint8_t claimed_target) {
    netcore::Vec3 origin{};
    netcore::Vec3 dir{};
    if (!ReadLocalAimOriginAndDir(base, origin, dir)) return;

    uint8_t shooter_id = 0;
    if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        shooter_id = g_client_session->local_player_id();
    }
    const netcore::FireEvent ev{
        .client_tick = tick,
        .shooter = shooter_id,
        .weapon = weapon_id < netcore::kMaxWeapons ? weapon_id : static_cast<uint8_t>(0),
        .origin = origin,
        .direction = dir,
        .claimed_target =
            (claimed_target < netcore::kMaxPlayers && claimed_target != shooter_id) ? claimed_target : netcore::kNoPlayer,
    };
    if (g_role == Role::Host && g_host_session) {
        if (g_host_session->local_fire(ev)) {
            g_last_local_fire_tick = tick;
        }
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        if (g_client_session->send_fire(ev)) {
            g_last_local_fire_tick = tick;
        }
    }
}

// Lê o estado atual do jogador local na memória convidada e devolve um PlayerState validado.
bool ReadLocalPlayerFromGuest(const uint8_t* base, uint32_t tick, netcore::PlayerState& out) {
    if (!black::IsInLevel(base)) return false;
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!player) return false;

    const float x = black::RF32(base, player + black::kCharPos + 0);
    const float y = black::RF32(base, player + black::kCharPos + 4);
    const float z = black::RF32(base, player + black::kCharPos + 8);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;

    float yaw_rad = 0.0f;
    float pitch_rad = 0.0f;
    const uint32_t world = black::R32(base, black::kWorld);
    // world + kWorldController é um PlayerController embutido na struct World (não é um ponteiro).
    const uint32_t ctrl = world ? (world + black::kWorldController) : 0;
    if (ctrl) {
        yaw_rad = clamp_angle_rad(black::RF32(base, ctrl + 0x18) * kDegToRad);
        pitch_rad = clamp_angle_rad(black::RF32(base, ctrl + 0x1C) * kDegToRad);
    }

    const auto now_tp = std::chrono::steady_clock::now();
    netcore::Vec3 vel{};
    if (g_has_last_local_pos && g_last_local_pos_tp.time_since_epoch().count() != 0) {
        const double dt = std::chrono::duration<double>(now_tp - g_last_local_pos_tp).count();
        if (dt > 0.002 && dt < 0.75) {
            const float inv_dt = static_cast<float>(1.0 / dt);
            vel.x = (x - g_last_local_pos.x) * inv_dt;
            vel.y = (y - g_last_local_pos.y) * inv_dt;
            vel.z = (z - g_last_local_pos.z) * inv_dt;
        }
    }
    g_last_local_pos = netcore::Vec3{x, y, z};
    g_has_last_local_pos = true;
    g_last_local_pos_tp = now_tp;

    const bool alive = (black::R32(base, player + black::kCharState) == 0);
    const bool crouched = (black::R8(base, player + black::kCharCrouch) != 0);
    const bool aiming = (ctrl != 0 && black::R8(base, ctrl + black::kControlZoom) != 0);
    const bool reloading = (ctrl != 0 && black::R8(base, ctrl + black::kControlReload) != 0);
    const uint8_t flags = static_cast<uint8_t>(
        (alive ? netcore::kFlagAlive : 0) |
        (crouched ? netcore::kFlagCrouched : 0) |
        (aiming ? netcore::kFlagAiming : 0) |
        (reloading ? netcore::kFlagReloading : 0));

    const uint8_t active_weapon = ReadLocalEquippedCatalogWeapon(base);
    out = netcore::PlayerState{
        .sequence = 0,
        .client_tick = tick,
        .position = netcore::Vec3{x, y, z},
        .yaw = yaw_rad,
        .pitch = pitch_rad,
        .velocity = vel,
        .anim_state = static_cast<uint8_t>(crouched ? 1 : 0),
        .weapon = active_weapon,
        .flags = flags,
    };
    return true;
}

// Aplica o SnapshotPlayer recebido da rede ao ator convidado correspondente (Testes #1, #2 e #3 da Fase 2).
void ApplySnapshotToActor(uint8_t* base, uint32_t actor_va, const netcore::SnapshotPlayer& sp) {
    if (!base || !IsGuestCommitted(actor_va, 0x3C0)) return;

    if (sp.player_id < netcore::kMaxPlayers) {
        g_last_applied_snapshot[sp.player_id] = sp;
        g_has_applied_snapshot[sp.player_id] = true;
    }

    // Marca como cinemático (+0x3A7 = 1) para a física local não sobrescrever a posição replicada
    black::W8(base, actor_va + black::kCharKinematic, 1);

    // Em PvP marca team=1 para colisão balística; em Co-op marca team=0 (aliado dos jogadores).
    const bool is_coop =
        (g_role == Role::Host && g_host_session && g_host_session->rules().game_mode == netcore::GameMode::Coop) ||
        (g_role == Role::Client && g_client_session && g_client_session->game_mode() == static_cast<uint8_t>(netcore::GameMode::Coop));
    black::W32(base, actor_va + black::kCharTeam, is_coop ? 0u : 1u);

    // Posição dos pés (+0xA0)
    black::WF32(base, actor_va + black::kCharPos + 0, sp.position.x);
    black::WF32(base, actor_va + black::kCharPos + 4, sp.position.y);
    black::WF32(base, actor_va + black::kCharPos + 8, sp.position.z);

    // Vida (+0x2F0) escalada pela vida máxima do nível e estado de vida (+0x384)
    const float scaled_hp = (static_cast<float>(sp.health) / 100.0f) * g_local_max_health;
    black::WF32(base, actor_va + black::kCharHealth, scaled_hp);
    const bool alive = (sp.flags & netcore::kFlagAlive) != 0;
    black::W32(base, actor_va + black::kCharState, alive ? 0u : 2u);

    if (alive && sp.player_id < netcore::kMaxPlayers && sp.weapon != netcore::kNoWeapon &&
        sp.weapon != g_remote_applied_weapon[sp.player_id]) {
        g_pending_remote_weapon[sp.player_id] = sp.weapon;
    }

    const bool crouched = (sp.flags & netcore::kFlagCrouched) != 0;
    black::W8(base, actor_va + black::kCharCrouch, crouched ? 1u : 0u);

    // Controlador de IA (+0x324): alimenta ângulos em graus, flags e vetores de locomoção
    const uint32_t ctrl = black::R32(base, actor_va + black::kCharControl);
    if (IsGuestCommitted(ctrl, 0x90)) {
        const float yaw_deg = sp.yaw * kRadToDeg;
        const float pitch_deg = sp.pitch * kRadToDeg;
        black::WF32(base, ctrl + black::kControlAngles + 0, yaw_deg);    // yaw
        black::WF32(base, ctrl + black::kControlAngles + 4, yaw_deg);    // torso yaw
        black::WF32(base, ctrl + black::kControlAngles + 8, yaw_deg);    // aim yaw
        black::WF32(base, ctrl + black::kControlAngles + 12, pitch_deg); // pitch

        const float speed =
            std::sqrt(sp.velocity.x * sp.velocity.x + sp.velocity.z * sp.velocity.z);
        // Zera velocidade do controlador durante a simulação; kFnAnimSelect injeta velocidade horizontal no hook de animação
        black::WF32(base, ctrl + black::kControlSpeed, 0.0f);
        black::W8(base, ctrl + black::kControlCrouch, crouched ? 1u : 0u);
        black::W8(base, ctrl + black::kControlFire, 0u);
        black::W8(base, ctrl + black::kControlZoom, (sp.flags & netcore::kFlagAiming) ? 1u : 0u);
        black::W8(base, ctrl + black::kControlReload, (sp.flags & netcore::kFlagReloading) ? 1u : 0u);

        if (alive && speed > 0.05f) {
            const float inv = 1.0f / speed;
            const float dx = sp.velocity.x * inv;
            const float dz = sp.velocity.z * inv;
            black::WF32(base, ctrl + black::kControlMove + 0, dx);
            black::WF32(base, ctrl + black::kControlMove + 4, 0.0f);
            black::WF32(base, ctrl + black::kControlMove + 8, dz);
            black::WF32(base, ctrl + black::kControlMoveDir + 0, dx);
            black::WF32(base, ctrl + black::kControlMoveDir + 4, 0.0f);
            black::WF32(base, ctrl + black::kControlMoveDir + 8, dz);
            black::WF32(base, ctrl + black::kControlRate, std::clamp(speed / 5.0f, 0.25f, 1.3f));
            black::W8(base, ctrl + black::kControlLocomotion, 1u);
        } else {
            black::WF32(base, ctrl + black::kControlRate, 0.0f);
            black::W8(base, ctrl + black::kControlLocomotion, 0u);
        }
    }
}

// Restaura a vida, controlador e posição do jogador local sem recarregar a fase (Teste #8 da Fase 2).
void RestoreAndTeleportLocalPlayer(uint8_t* base, const netcore::Vec3& pos, float yaw_rad) {
    if (!black::IsInLevel(base)) return;
    const uint32_t world = black::R32(base, black::kWorld);
    if (!world) return;
    const uint32_t player = world + black::kWorldPlayer;

    const uint32_t player_ctrl = world + black::kWorldController;
    const uint32_t restored_ctrl = g_saved_player_controller != 0 ? g_saved_player_controller : player_ctrl;
    black::W32(base, player + black::kCharControl, restored_ctrl);

    if (g_saved_player_name[0] != 0 || g_saved_player_name[1] != 0) {
        black::W32(base, player + black::kCharName + 0, g_saved_player_name[0]);
        black::W32(base, player + black::kCharName + 4, g_saved_player_name[1]);
    }

    black::W32(base, player + black::kCharState, 0u);
    black::WF32(base, player + black::kCharHealth, g_local_max_health);

    // Atualiza tanto a posição atual (+0xA0) quanto a do quadro anterior (world + 0x1C0) para evitar sweep falso
    black::WF32(base, player + black::kCharPos + 0, pos.x);
    black::WF32(base, player + black::kCharPos + 4, pos.y);
    black::WF32(base, player + black::kCharPos + 8, pos.z);
    black::WF32(base, world + black::kWorldLastPos + 0, pos.x);
    black::WF32(base, world + black::kWorldLastPos + 4, pos.y);
    black::WF32(base, world + black::kWorldLastPos + 8, pos.z);

    black::WF32(base, player_ctrl + 0x18, yaw_rad * kRadToDeg);
    black::WF32(base, player_ctrl + 0x1C, 0.0f);

    g_last_local_pos = pos;
    g_has_last_local_pos = true;
    g_last_local_pos_tp = std::chrono::steady_clock::now();

    g_last_local_pos = pos;
    g_local_waiting_respawn = false;
    if ((g_role == Role::Host && g_host_session && g_host_session->rules().enforce_loadout) ||
        (g_role == Role::Client && g_client_session && g_client_session->local_loadout().max_credits > 0)) {
        g_pending_weapon_apply = true;
        g_pending_equip_slot = 0;
    }
    LOGR("mp_bridge: local player respawned at (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
}

// Vincula automaticamente um ator livre do pool (kPool -> kAliveList) ao slot remoto na fase.
void EnsureRemoteActorForSlot(uint8_t* base, uint8_t slot) {
    if (!base || slot >= netcore::kMaxPlayers || !black::IsInLevel(base)) return;

    const uint32_t existing = g_remote_actor_va[slot];
    if (existing != 0) {
        if (IsGuestCommitted(existing, 0x3C0)) {
            const uint32_t ectrl = black::R32(base, existing + black::kCharControl);
            if (IsGuestCommitted(ectrl, 0x90) && black::R32(base, ectrl + black::kControlChar) == existing) {
                return;
            }
        }
        UnbindRemoteActor(slot);
    }

    const uint32_t pool = black::R32(base, black::kPool);
    if (!IsGuestCommitted(pool, black::kAliveList + 16)) return;
    const uint32_t list_base = pool + black::kAliveList;
    const uint32_t cap = black::R32(base, list_base + 4);
    const uint32_t arr = black::R32(base, list_base + 0x0C);
    if (!arr || cap == 0 || cap > 256 || !IsGuestCommitted(arr, cap * 4)) return;

    const uint32_t world = black::R32(base, black::kWorld);
    const uint32_t local_player = world ? (world + black::kWorldPlayer) : 0;
    const uint32_t local_ctrl = world ? (world + black::kWorldController) : 0;

    uint32_t best_enemy_va = 0;
    uint32_t best_alive_va = 0;
    uint32_t fallback_va = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        const uint32_t a = black::R32(base, arr + i * 4);
        if (!a || a == local_player || FindRemoteSlotByActor(a) != netcore::kNoPlayer) continue;
        if (!IsGuestCommitted(a, 0x3C0)) continue;
        const uint32_t ctrl = black::R32(base, a + black::kCharControl);
        if (!ctrl || ctrl == local_ctrl || !IsGuestCommitted(ctrl, 0x90)) continue;
        if (black::R32(base, ctrl + black::kControlChar) != a) continue;

        const uint32_t st = black::R32(base, a + black::kCharState);
        const uint32_t team = black::R32(base, a + black::kCharTeam);
        const float hp = black::RF32(base, a + black::kCharHealth);
        if (st == 0u) {
            if (team != 0u && std::isfinite(hp) && hp < 1e6f) {
                best_enemy_va = a;
                break;
            }
            if (best_alive_va == 0) best_alive_va = a;
        } else if (fallback_va == 0) {
            fallback_va = a;
        }
    }

    const uint32_t chosen = best_enemy_va ? best_enemy_va : (best_alive_va ? best_alive_va : fallback_va);
    if (chosen != 0) {
        BindRemoteActor(slot, chosen);
    }
}

bool HasDedicatedSpawnHeadroom(const uint8_t* base) {
    if (!base) return false;
    const uint32_t ctrl_mgr = IsGuestCommitted(0x002D1988u, 4) ? black::R32(base, 0x002D1988u) : 0u;
    if (!ctrl_mgr || !IsGuestCommitted(ctrl_mgr + 0x22CDCu, 4) || black::R32(base, ctrl_mgr + 0x22CDCu) == 0u) {
        return false;
    }
    const uint32_t char_mgr = IsGuestCommitted(0x002D45F4u, 4) ? black::R32(base, 0x002D45F4u) : 0u;
    if (!char_mgr || !IsGuestCommitted(char_mgr + 0x2960u, 20u)) return false;
    for (uint32_t i = 0; i < 20u; ++i) {
        if (black::R8(base, char_mgr + 0x2960u + i) == 0u) return true;
    }
    return false;
}

void AcquireOrQueueRemoteActor(uint8_t* base, uint8_t slot) {
    if (!base || slot >= netcore::kMaxPlayers || !black::IsInLevel(base)) return;

    const uint32_t existing = g_remote_actor_va[slot];
    if (existing != 0) {
        if (IsGuestCommitted(existing, 0x3C0)) {
            const uint32_t ectrl = black::R32(base, existing + black::kCharControl);
            if (IsGuestCommitted(ectrl, 0x90) && black::R32(base, ectrl + black::kControlChar) == existing) {
                return;
            }
        }
        ReleaseDedicatedRemoteActor(base, slot);
        UnbindRemoteActor(slot);
    }

    if (!g_remote_dedicated_failed[slot] && HasDedicatedSpawnHeadroom(base)) {
        g_pending_remote_spawn_mask |= static_cast<uint8_t>(1u << slot);
        return;
    }

    const bool is_coop =
        (g_role == Role::Host && g_host_session && g_host_session->rules().game_mode == netcore::GameMode::Coop) ||
        (g_role == Role::Client && g_client_session &&
         g_client_session->game_mode() == static_cast<uint8_t>(netcore::GameMode::Coop));
    if (!is_coop) {
        EnsureRemoteActorForSlot(base, slot);
    }
}

void InitializeLevelContextIfReady(const uint8_t* base) {
    if (g_level_spawns_initialized || !black::IsInLevel(base)) return;
    const uint32_t world = black::R32(base, black::kWorld);
    if (!world) return;
    const uint32_t player = world + black::kWorldPlayer;
    if (!IsGuestCommitted(player, 0x3C0)) return;

    const float raw_hp = black::RF32(base, player + black::kCharHealth);
    if (std::isfinite(raw_hp) && raw_hp >= 50.0f && raw_hp <= 10000.0f) {
        g_local_max_health = raw_hp;
    } else {
        g_local_max_health = 100.0f;
    }

    const uint32_t n0 = black::R32(base, player + black::kCharName + 0);
    const uint32_t n1 = black::R32(base, player + black::kCharName + 4);
    if (n0 != 0 || n1 != 0) {
        g_saved_player_name[0] = n0;
        g_saved_player_name[1] = n1;
    }
    const uint32_t emb_ctrl = world + black::kWorldController;
    const uint32_t pctrl = black::R32(base, player + black::kCharControl);
    if (pctrl == emb_ctrl) {
        g_saved_player_controller = pctrl;
    } else {
        g_saved_player_controller = emb_ctrl;
    }

    size_t spawn_count = 0;
    if (g_role == Role::Host && g_host_session) {
        const float px = black::RF32(base, player + black::kCharPos + 0);
        const float py = black::RF32(base, player + black::kCharPos + 4);
        const float pz = black::RF32(base, player + black::kCharPos + 8);
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) return;

        const float pyaw = IsGuestCommitted(emb_ctrl, 0x20)
                               ? clamp_angle_rad(black::RF32(base, emb_ctrl + 0x18) * kDegToRad)
                               : 0.0f;
        const float safe_yaw = std::isfinite(pyaw) ? pyaw : 0.0f;
        std::vector<netcore::SpawnPoint> spawns;
        spawns.push_back(netcore::SpawnPoint{netcore::Vec3{px, py, pz}, safe_yaw});
        const float opp_yaw = clamp_angle_rad(safe_yaw + 3.14159265f);
        spawns.push_back(netcore::SpawnPoint{
            netcore::Vec3{px + std::sin(safe_yaw) * 1.8f, py, pz + std::cos(safe_yaw) * 1.8f},
            opp_yaw});

        const uint32_t pool = black::R32(base, black::kPool);
        if (IsGuestCommitted(pool, black::kAliveList + 16)) {
            const uint32_t list_base = pool + black::kAliveList;
            const uint32_t cap = black::R32(base, list_base + 4);
            const uint32_t arr = black::R32(base, list_base + 0x0C);
            if (arr && cap > 0 && cap <= 256 && IsGuestCommitted(arr, cap * 4)) {
                for (uint32_t i = 0; i < cap && spawns.size() < netcore::kMaxSpawnPoints; ++i) {
                    const uint32_t a = black::R32(base, arr + i * 4);
                    if (!a || a == player || !IsGuestCommitted(a, 0x3C0)) continue;
                    const float ax = black::RF32(base, a + black::kCharPos + 0);
                    const float ay = black::RF32(base, a + black::kCharPos + 4);
                    const float az = black::RF32(base, a + black::kCharPos + 8);
                    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) continue;
                    if (std::fabs(ay - py) > 1.5f) continue;
                    bool far_enough = true;
                    for (const auto& existing_sp : spawns) {
                        const float dx = ax - existing_sp.position.x;
                        const float dz = az - existing_sp.position.z;
                        if (dx * dx + dz * dz < 4.0f) {
                            far_enough = false;
                            break;
                        }
                    }
                    if (far_enough) {
                        spawns.push_back(netcore::SpawnPoint{netcore::Vec3{ax, ay, az}, safe_yaw});
                    }
                }
            }
        }
        spawn_count = spawns.size();
        if (g_host_session->rules().game_mode == netcore::GameMode::TerritorialAssault) {
            // Ordena pontos reais da fase pela distância a partir do início da missão para posicionar
            // spawns de ataque, Objetivos A/B sequenciais e spawns de defesa na geometria real do mapa.
            std::vector<netcore::SpawnPoint> sorted = spawns;
            std::sort(sorted.begin(), sorted.end(), [&](const netcore::SpawnPoint& a, const netcore::SpawnPoint& b) {
                const float da = (a.position.x - px) * (a.position.x - px) + (a.position.z - pz) * (a.position.z - pz);
                const float db = (b.position.x - px) * (b.position.x - px) + (b.position.z - pz) * (b.position.z - pz);
                return da < db;
            });
            std::vector<netcore::SpawnPoint> atk_spawns;
            std::vector<netcore::SpawnPoint> def_spawns;
            std::vector<netcore::CaptureZone> zones;
            atk_spawns.push_back(sorted[0]);
            if (sorted.size() >= 2) atk_spawns.push_back(sorted[1]);

            const float fwd_x = std::sin(safe_yaw);
            const float fwd_z = std::cos(safe_yaw);
            if (sorted.size() >= 5) {
                const std::size_t idx_a = sorted.size() * 2 / 5;
                const std::size_t idx_b = sorted.size() * 4 / 5;
                zones.push_back(netcore::CaptureZone{sorted[idx_a].position, 7.5f, 5.0f});
                zones.push_back(netcore::CaptureZone{sorted[idx_b].position, 7.5f, 5.0f});
                def_spawns.push_back(sorted[sorted.size() - 1]);
                def_spawns.push_back(sorted[sorted.size() - 2]);
            } else {
                zones.push_back(netcore::CaptureZone{
                    netcore::Vec3{px + fwd_x * 14.0f, py, pz + fwd_z * 14.0f}, 7.5f, 5.0f});
                zones.push_back(netcore::CaptureZone{
                    netcore::Vec3{px + fwd_x * 28.0f, py, pz + fwd_z * 28.0f}, 7.5f, 5.0f});
                def_spawns.push_back(netcore::SpawnPoint{
                    netcore::Vec3{px + fwd_x * 36.0f, py, pz + fwd_z * 36.0f}, opp_yaw});
                def_spawns.push_back(netcore::SpawnPoint{
                    netcore::Vec3{px + fwd_x * 38.0f, py, pz + fwd_z * 38.0f}, opp_yaw});
            }
            g_host_session->set_spawn_points(std::move(spawns));
            g_host_session->set_territorial_map(std::move(zones), std::move(atk_spawns), std::move(def_spawns));
            if (!g_prematch_configured && !g_host_session->territorial_match() && g_host_session->player_count() >= 2) {
                (void)g_host_session->enter_preparation();
            }
        } else {
            g_host_session->set_spawn_points(std::move(spawns));
            g_host_session->start_match();
        }
    }

    g_level_spawns_initialized = true;
    LOGR("mp_bridge: level initialized (local_max_hp=%.1f, spawns=%zu, match_tick=%u)",
         g_local_max_health, spawn_count, g_host_session ? g_host_session->current_tick() : 0u);
}

void SyncLocalPlayerHealthAndLife(uint8_t* base, uint8_t health, bool alive) {
    if (!black::IsInLevel(base)) return;
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!IsGuestCommitted(player, 0x3C0)) return;

    static uint8_t s_prev_hp = 255;
    static bool s_prev_alive = true;
    if (health != s_prev_hp || alive != s_prev_alive) {
        s_prev_hp = health;
        s_prev_alive = alive;
        LOGR("mp_bridge: sync local health=%u alive=%d", static_cast<unsigned>(health), alive ? 1 : 0);
    }

    if (alive) {
        g_awaiting_alive_snapshot = false;
        if (!g_local_waiting_respawn) {
            const float guest_hp = (static_cast<float>(health) / 100.0f) * g_local_max_health;
            black::WF32(base, player + black::kCharHealth, guest_hp);
        }
    } else if (!g_awaiting_alive_snapshot && !g_local_waiting_respawn) {
        OnLocalPlayerDying(base);
        black::WF32(base, player + black::kCharHealth, 0.0f);
        black::W32(base, player + black::kCharState, 2u);
    }
}

// Proteção simples contra volumes de personagens sobrepostos exatamente na mesma coordenada (prevenção de divisão por zero).
void PreventSamePointCollisionCrash(uint8_t* base) {
    if (!black::IsInLevel(base)) return;
    const uint32_t pool = black::R32(base, black::kPool);
    if (!pool) return;

    const uint32_t list_base = pool + black::kAliveList;
    const uint32_t cap = black::R32(base, list_base + 4);
    const uint32_t arr = black::R32(base, list_base + 0x0C);
    if (!arr || cap == 0 || cap > 256) return;

    for (uint32_t i = 0; i < cap; ++i) {
        const uint32_t a = black::R32(base, arr + i * 4);
        if (!a) continue;
        const float ax = black::RF32(base, a + black::kCharPos + 0);
        const float az = black::RF32(base, a + black::kCharPos + 8);
        for (uint32_t j = i + 1; j < cap; ++j) {
            const uint32_t b = black::R32(base, arr + j * 4);
            if (!b || b == a) continue;
            const float bx = black::RF32(base, b + black::kCharPos + 0);
            const float bz = black::RF32(base, b + black::kCharPos + 8);
            const float dx = bx - ax;
            const float dz = bz - az;
            if (dx * dx + dz * dz < 1e-6f) {
                black::WF32(base, b + black::kCharPos + 0, bx + 0.05f);
                black::WF32(base, b + black::kCharPos + 8, bz + 0.05f);
            }
        }
    }
}

uint16_t ParsePortOrDefault(const std::string& text, uint16_t fallback) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    const unsigned long val = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || val == 0 || val > 65535) return fallback;
    return static_cast<uint16_t>(val);
}

void SplitHostPort(const std::string& spec, std::string& out_host, uint16_t& inout_port) {
    const std::size_t colon = spec.rfind(':');
    if (colon == std::string::npos) {
        out_host = spec;
        return;
    }
    out_host = spec.substr(0, colon);
    inout_port = ParsePortOrDefault(spec.substr(colon + 1), inout_port);
}

#ifdef _WIN32
std::vector<std::string> SplitCommandLineWindows(const char* cmdline) {
    std::vector<std::string> args;
    if (!cmdline) return args;
    const char* p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        std::string cur;
        bool in_quotes = false;
        while (*p) {
            if (*p == '"') {
                in_quotes = !in_quotes;
                ++p;
                continue;
            }
            if (!in_quotes && (*p == ' ' || *p == '\t')) break;
            cur.push_back(*p++);
        }
        args.push_back(std::move(cur));
    }
    return args;
}
#endif

}  // namespace

void InitBridgeFromArgs(int argc, const char* const* argv) {
    g_frame_count = 0;
    g_remote_actor_va.fill(0);

    bool want_host = false;
    bool want_client = false;
    std::string join_target;
#ifndef __EMSCRIPTEN__
    uint16_t port = kDefaultUdpPort;
#else
    uint16_t port = 45410;
#endif
    std::string name;

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::string arg = argv[i];
        if (arg == "--mp-host") {
            want_host = true;
            if (i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-') {
                port = ParsePortOrDefault(argv[i + 1], port);
                ++i;
            }
        } else if (arg == "--mp-join" && i + 1 < argc && argv[i + 1]) {
            want_client = true;
            SplitHostPort(argv[i + 1], join_target, port);
            ++i;
        } else if (arg == "--mp-port" && i + 1 < argc && argv[i + 1]) {
            port = ParsePortOrDefault(argv[i + 1], port);
            ++i;
        } else if (arg == "--mp-name" && i + 1 < argc && argv[i + 1]) {
            name = argv[i + 1];
            ++i;
        }
    }

    if (want_host) {
        if (!StartHostUdp(port, name.empty() ? "Host" : name)) {
            LOGE("mp_bridge: failed to start UDP host on port %u; staying OfflineCampaign",
                 static_cast<unsigned>(port));
        }
    } else if (want_client && !join_target.empty()) {
        if (!StartClientUdp(join_target, port, name.empty() ? "Player" : name)) {
            LOGE("mp_bridge: failed to start UDP client to %s:%u; staying OfflineCampaign",
                 join_target.c_str(), static_cast<unsigned>(port));
        }
    } else {
        StopMultiplayer();
    }

    LOGR("mp_bridge: initialized (role=%u, protocol_v=%u)", static_cast<unsigned>(g_role),
         static_cast<unsigned>(netcore::kProtocolVersion));
}

void InitBridge() {
#ifdef _WIN32
    const auto args = SplitCommandLineWindows(GetCommandLineA());
    std::vector<const char*> ptrs;
    ptrs.reserve(args.size());
    for (const auto& s : args) ptrs.push_back(s.c_str());
    InitBridgeFromArgs(static_cast<int>(ptrs.size()), ptrs.data());
#else
    const char* argv0[] = {"black_enhanced"};
    InitBridgeFromArgs(1, argv0);
#endif
}

bool StartHostUdp(uint16_t port, const std::string& player_name) {
#ifdef __EMSCRIPTEN__
    (void)port;
    (void)player_name;
    return false;
#else
    StopMultiplayer();
    auto transport = UdpTransport::CreateHost(port);
    if (!transport || !transport->is_valid()) {
        return false;
    }
    g_udp_transport = std::move(transport);
    g_host_session = std::make_unique<netcore::HostSession>(
        *g_udp_transport, netcore::RulesConfig::default_deathmatch(),
        player_name.empty() ? "Host" : player_name);
    g_host_session->start_match();
    g_role = Role::Host;
    LOGI("mp_bridge: UDP host started on port %u", static_cast<unsigned>(g_udp_transport->local_port()));
    return true;
#endif
}

bool StartClientUdp(const std::string& remote_host, uint16_t port, const std::string& player_name) {
#ifdef __EMSCRIPTEN__
    (void)remote_host;
    (void)port;
    (void)player_name;
    return false;
#else
    StopMultiplayer();
    auto transport = UdpTransport::CreateClient(remote_host, port);
    if (!transport || !transport->is_valid()) {
        return false;
    }
    const netcore::PeerId host_peer = transport->host_peer_id();
    g_udp_transport = std::move(transport);
    g_client_session = std::make_unique<netcore::ClientSession>(
        *g_udp_transport, host_peer, netcore::RulesConfig::default_deathmatch(),
        player_name.empty() ? "Player" : player_name);
    g_client_session->send_hello();
    g_role = Role::Client;
    LOGI("mp_bridge: UDP client connecting to %s:%u (local port %u)", remote_host.c_str(),
         static_cast<unsigned>(port), static_cast<unsigned>(g_udp_transport->local_port()));
    return true;
#endif
}

bool StartHostWebRtc(const std::string& player_name, bool cs_loadout, bool fixed_budget, uint16_t budget_cap,
                     uint8_t game_mode) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    StopMultiplayer();
    g_active_player_name = player_name.empty() ? "Host" : player_name;
    g_webrtc_transport = std::make_unique<WebRtcTransport>();
    const auto rules = BuildActiveRules(cs_loadout, fixed_budget, budget_cap, game_mode);
    g_host_session = std::make_unique<netcore::HostSession>(*g_webrtc_transport, rules, g_active_player_name);
    if (rules.game_mode != netcore::GameMode::TerritorialAssault) {
        g_host_session->start_match();
    }
    g_role = Role::Host;
    if (cs_loadout || rules.game_mode == netcore::GameMode::TerritorialAssault) {
        g_pending_weapon_apply = true;
        g_pending_equip_slot = 0;
    }
    LOGR("mp_bridge: WebRTC host session ready (%s, mode=%u, cs_loadout=%d, fixed=%d, cap=%u)",
         g_active_player_name.c_str(), static_cast<unsigned>(game_mode), cs_loadout ? 1 : 0, fixed_budget ? 1 : 0,
         static_cast<unsigned>(budget_cap));
    return true;
}

bool StartClientWebRtc(const std::string& player_name, uint32_t host_peer, bool cs_loadout, bool fixed_budget,
                       uint16_t budget_cap, uint8_t game_mode) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    StopMultiplayer();
    g_active_player_name = player_name.empty() ? "Player" : player_name;
    g_webrtc_transport = std::make_unique<WebRtcTransport>();
    g_webrtc_transport->push_incoming_connected(host_peer);
    g_client_session = std::make_unique<netcore::ClientSession>(
        *g_webrtc_transport, host_peer, BuildActiveRules(cs_loadout, fixed_budget, budget_cap, game_mode),
        g_active_player_name);
    g_client_session->send_hello();
    g_role = Role::Client;
    LOGR("mp_bridge: WebRTC client connecting to peer=%u (%s, mode=%u, cs_loadout=%d, fixed=%d, cap=%u)",
         host_peer, g_active_player_name.c_str(), static_cast<unsigned>(game_mode), cs_loadout ? 1 : 0,
         fixed_budget ? 1 : 0, static_cast<unsigned>(budget_cap));
    return true;
}

bool SelectTerritorialTeam(uint8_t team) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    const auto t = (team == 0u) ? netcore::ta::Team::A : netcore::ta::Team::B;
    if (g_role == Role::Host && g_host_session) {
        return g_host_session->set_player_team(0, t);
    }
    if (g_role == Role::Client && g_client_session) {
        return g_client_session->send_team_select(t);
    }
    return false;
}

bool EnterTerritorialPreparation() {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::Host && g_host_session) {
        return g_host_session->enter_preparation();
    }
    return false;
}

bool BeginTerritorialRound() {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::Host && g_host_session) {
        if (!g_host_session->territorial_match()) {
            if (!g_host_session->enter_preparation()) return false;
        }
        return g_host_session->begin_next_round();
    }
    return false;
}

WebRtcTransport* ActiveWebRtcTransport() {
    return g_webrtc_transport.get();
}

BridgeTelemetry GetTelemetry(const uint8_t* guest_base) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    BridgeTelemetry t{};
    t.role = static_cast<uint8_t>(g_role);
    t.in_level = guest_base && black::IsInLevel(guest_base);
    t.hitmarker_seq = g_hitmarker_seq.load(std::memory_order_relaxed);
    t.engine_weapon_count = ReadLoadedWeaponDefCount(guest_base);
    if (t.in_level) {
        const uint32_t player = black::LocalPlayerAddr(guest_base);
        if (IsGuestCommitted(player, 0x2C0)) {
            t.active_slot = black::R8(guest_base, player + black::kCharActiveSlot);
            t.active_engine_weapon_id = black::R8(guest_base, player + black::kCharActiveWeaponId);
        }
        t.active_weapon = ReadLocalEquippedCatalogWeapon(guest_base);
    }

    if (t.in_level) {
        netcore::Vec3 cpos{};
        netcore::Vec3 cdir{};
        if (ReadLocalAimOriginAndDir(guest_base, cpos, cdir) &&
            std::isfinite(cpos.x) && std::isfinite(cpos.y) && std::isfinite(cpos.z) &&
            std::isfinite(cdir.x) && std::isfinite(cdir.y) && std::isfinite(cdir.z)) {
            t.has_local_cam = true;
            t.cam_x = cpos.x;
            t.cam_y = cpos.y;
            t.cam_z = cpos.z;
            t.cam_fx = cdir.x;
            t.cam_fy = cdir.y;
            t.cam_fz = cdir.z;
        }
    }

    auto fill_ta_beacon = [&](const netcore::TerritorialState& ta) {
        t.ta_zone_x = ta.active_zone_center.x;
        t.ta_zone_y = ta.active_zone_center.y;
        t.ta_zone_z = ta.active_zone_center.z;
        t.ta_zone_radius = ta.active_zone_radius;
        if (ta.active_zone_radius > 0.1f && t.in_level && t.has_local_cam) {
            const float dx = ta.active_zone_center.x - t.cam_x;
            const float dy = (ta.active_zone_center.y + 1.6f) - t.cam_y;
            const float dz = ta.active_zone_center.z - t.cam_z;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            if (std::isfinite(horiz)) {
                t.ta_has_beacon = true;
                t.ta_dist_m = horiz;
                t.ta_in_zone = (horiz <= ta.active_zone_radius && std::fabs(ta.active_zone_center.y - t.cam_y) <= 5.5f);
                const float f_len = std::sqrt(t.cam_fx * t.cam_fx + t.cam_fz * t.cam_fz);
                if (f_len > 1e-4f && horiz > 0.05f) {
                    const float fx = t.cam_fx / f_len;
                    const float fz = t.cam_fz / f_len;
                    // Em Direct3D (+X direita, +Z frente), right = (fz, -fx) para fwd = (fx, fz).
                    const float rx = fz;
                    const float rz = -fx;
                    const float dot_f = dx * fx + dz * fz;
                    const float dot_r = dx * rx + dz * rz;
                    const float deg = std::atan2(dot_r, dot_f) * kRadToDeg;
                    if (std::isfinite(deg)) {
                        t.ta_bearing_deg = static_cast<int16_t>(std::lround(deg));
                    }
                }
                const float target_pitch = std::atan2(dy, std::max(0.25f, horiz));
                const float cam_pitch = std::atan2(t.cam_fy, std::max(1e-4f, f_len));
                const float edeg = (target_pitch - cam_pitch) * kRadToDeg;
                if (std::isfinite(edeg)) {
                    t.ta_elev_deg = static_cast<int16_t>(std::lround(std::clamp(edeg, -89.0f, 89.0f)));
                }
            }
        }
    };

    if (g_role == Role::Host && g_host_session) {
        t.local_slot = 0;
        t.tick = g_host_session->current_tick();
        t.arena_id = g_host_session->active_arena_id();
        t.game_mode = static_cast<uint8_t>(g_host_session->rules().game_mode);
        t.available_weapon_mask = g_host_session->available_weapon_mask();
        if (g_webrtc_transport) t.connected_peers = static_cast<uint32_t>(g_webrtc_transport->peer_count());
        const auto ls = g_host_session->slot_loadout_state(0);
        t.primary_weapon = ls.primary_weapon;
        t.secondary_weapon = ls.secondary_weapon;
        t.tactical_flags = ls.tactical_flags;
        t.last_loadout_result = ls.last_result;
        t.credits = ls.credits;
        t.max_credits = ls.max_credits;
        t.kit_cost = ls.kit_cost;
        t.buy_ticks_left = ls.buy_ticks_left;
        t.fixed_budget_mode = g_host_session->rules().fixed_budget_mode;
        t.enforce_loadout = g_host_session->rules().enforce_loadout;

        const auto& slots = g_host_session->slots();
        t.local_health = slots[0].health;
        t.local_alive = (slots[0].flags & netcore::kFlagAlive) != 0;
        t.local_kills = slots[0].kills;
        t.local_deaths = slots[0].deaths;
        for (std::size_t i = 0; i < netcore::kMaxPlayers; ++i) {
            const auto& s = slots[i];
            const auto sls = g_host_session->slot_loadout_state(static_cast<uint8_t>(i));
            t.players[i] = HudPlayerRow{
                .player_id = s.player_id,
                .team = s.team,
                .occupied = s.occupied,
                .alive = (s.flags & netcore::kFlagAlive) != 0,
                .health = s.health,
                .weapon = s.weapon,
                .primary_weapon = sls.primary_weapon,
                .secondary_weapon = sls.secondary_weapon,
                .tactical_flags = sls.tactical_flags,
                .kills = s.kills,
                .deaths = s.deaths,
                .pos_x = s.position.x,
                .pos_y = s.position.y,
                .pos_z = s.position.z,
                .name = s.name,
            };
        }
        auto fill_ta_telemetry = [&](const netcore::TerritorialState& ta, uint8_t local_team) {
            t.ta_active = true;
            t.ta_match_state = ta.match_state;
            t.ta_attacker_team = ta.attacker_team;
            t.ta_local_team = local_team;
            t.ta_current_objective = ta.current_objective;
            t.ta_objective_count = ta.objective_count;
            t.ta_captured_count = ta.captured_count;
            t.ta_current_obj_state = ta.current_obj_state;
            t.ta_progress_permille = ta.progress_permille;
            t.ta_time_left_s = ta.time_left_s;
            t.ta_overtime = ta.overtime != 0;
            t.ta_attacker_waves = ta.attacker_waves;
            t.ta_defender_waves = ta.defender_waves;
            t.ta_winner_team = ta.winner_team;
            t.ta_r1_captured = ta.r1_captured;
            t.ta_r1_progress_permille = ta.r1_progress_permille;
            t.ta_r1_time_s = ta.r1_time_s;
            t.ta_r2_captured = ta.r2_captured;
            t.ta_r2_progress_permille = ta.r2_progress_permille;
            t.ta_r2_time_s = ta.r2_time_s;
            t.ta_atk_wave_eta_s = ta.atk_wave_eta_s;
            t.ta_def_wave_eta_s = ta.def_wave_eta_s;
            t.ta_zone_attackers = ta.zone_attackers;
            t.ta_zone_defenders = ta.zone_defenders;
            fill_ta_beacon(ta);
        };
        if (g_host_session->rules().game_mode == netcore::GameMode::TerritorialAssault) {
            fill_ta_telemetry(g_host_session->current_territorial_state(), slots[0].team);
        }
    } else if (g_role == Role::Client && g_client_session) {
        t.local_slot = g_client_session->local_player_id();
        t.client_joined = g_client_session->is_joined();
        t.tick = g_client_session->latest_snapshot().tick;
        t.arena_id = g_client_session->active_arena_id();
        t.game_mode = g_client_session->game_mode();
        t.available_weapon_mask = g_client_session->available_weapon_mask();
        if (g_webrtc_transport) t.connected_peers = static_cast<uint32_t>(g_webrtc_transport->peer_count());
        const auto ls = g_client_session->local_loadout();
        const auto& all_ls = g_client_session->loadouts();
        t.primary_weapon = ls.primary_weapon;
        t.secondary_weapon = ls.secondary_weapon;
        t.tactical_flags = ls.tactical_flags;
        t.last_loadout_result = ls.last_result;
        t.credits = ls.credits;
        const bool is_ta_client = (t.game_mode == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault));
        t.max_credits = ls.max_credits > 0 ? ls.max_credits : (is_ta_client ? 15u : 8000u);
        t.kit_cost = ls.kit_cost;
        t.buy_ticks_left = ls.buy_ticks_left;
        t.fixed_budget_mode = is_ta_client;
        t.enforce_loadout = ls.max_credits > 0 || is_ta_client;

        const auto& opt_ta = g_client_session->latest_territorial_state();
        if (t.game_mode == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault) && opt_ta.has_value()) {
            const auto& ta = *opt_ta;
            t.ta_active = true;
            t.ta_match_state = ta.match_state;
            t.ta_attacker_team = ta.attacker_team;
            t.ta_local_team = (t.local_slot < netcore::kMaxPlayers && ta.player_teams[t.local_slot] <= 1u)
                                  ? ta.player_teams[t.local_slot]
                                  : 0u;
            t.ta_current_objective = ta.current_objective;
            t.ta_objective_count = ta.objective_count;
            t.ta_captured_count = ta.captured_count;
            t.ta_current_obj_state = ta.current_obj_state;
            t.ta_progress_permille = ta.progress_permille;
            t.ta_time_left_s = ta.time_left_s;
            t.ta_overtime = ta.overtime != 0;
            t.ta_attacker_waves = ta.attacker_waves;
            t.ta_defender_waves = ta.defender_waves;
            t.ta_winner_team = ta.winner_team;
            t.ta_r1_captured = ta.r1_captured;
            t.ta_r1_progress_permille = ta.r1_progress_permille;
            t.ta_r1_time_s = ta.r1_time_s;
            t.ta_r2_captured = ta.r2_captured;
            t.ta_r2_progress_permille = ta.r2_progress_permille;
            t.ta_r2_time_s = ta.r2_time_s;
            t.ta_atk_wave_eta_s = ta.atk_wave_eta_s;
            t.ta_def_wave_eta_s = ta.def_wave_eta_s;
            t.ta_zone_attackers = ta.zone_attackers;
            t.ta_zone_defenders = ta.zone_defenders;
            fill_ta_beacon(ta);
        }

        for (const auto& sp : g_client_session->latest_snapshot().players) {
            if (sp.player_id >= netcore::kMaxPlayers) continue;
            if (sp.player_id == t.local_slot) {
                t.local_health = sp.health;
                t.local_alive = (sp.flags & netcore::kFlagAlive) != 0;
                t.local_kills = sp.kills;
                t.local_deaths = sp.deaths;
            }
            const auto& pls = all_ls[sp.player_id];
            std::string roster_name = g_client_session->player_name(sp.player_id);
            if (roster_name.empty()) {
                roster_name = (sp.player_id == t.local_slot) ? g_active_player_name
                                                             : ("Player " + std::to_string(sp.player_id));
            }
            const uint8_t pteam = (opt_ta.has_value() && opt_ta->player_teams[sp.player_id] <= 1u)
                                      ? opt_ta->player_teams[sp.player_id]
                                      : 0u;
            t.players[sp.player_id] = HudPlayerRow{
                .player_id = sp.player_id,
                .team = pteam,
                .occupied = true,
                .alive = (sp.flags & netcore::kFlagAlive) != 0,
                .health = sp.health,
                .weapon = sp.weapon,
                .primary_weapon = pls.primary_weapon,
                .secondary_weapon = pls.secondary_weapon,
                .tactical_flags = pls.tactical_flags,
                .kills = sp.kills,
                .deaths = sp.deaths,
                .pos_x = sp.position.x,
                .pos_y = sp.position.y,
                .pos_z = sp.position.z,
                .name = std::move(roster_name),
            };
        }
    } else {
        // Campanha Solo: zero sistema de loja/créditos (armas e fluxo originais da missão intactos).
        t.primary_weapon = netcore::kNoWeapon;
        t.secondary_weapon = netcore::kNoWeapon;
        t.tactical_flags = 0;
        t.last_loadout_result = 0;
        t.credits = 0;
        t.max_credits = 0;
        t.fixed_budget_mode = false;
        t.buy_ticks_left = 0;
        t.kit_cost = 0;
        if (t.in_level) {
            const uint32_t player = black::LocalPlayerAddr(guest_base);
            if (IsGuestCommitted(player, 0x3C0)) {
                t.local_health = static_cast<uint8_t>(std::clamp(
                    static_cast<int>(std::lround(black::RF32(guest_base, player + black::kCharHealth))), 0, 255));
            }
        }
    }
    return t;
}

bool BuyLoadout(uint8_t primary_weapon, uint8_t secondary_weapon, uint8_t tactical_flags) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    // Loadout/Loja estilo CS existe apenas em partidas Multiplayer (Host ou Client conectado).
    if (g_role == Role::OfflineCampaign) {
        return false;
    }
    uint32_t avail_mask = 0xFFFFFFFFu;
    if (g_role == Role::Host && g_host_session) {
        avail_mask = g_host_session->available_weapon_mask();
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        avail_mask = g_client_session->available_weapon_mask();
    }
    if (primary_weapon != netcore::kNoWeapon && primary_weapon < 32u && (avail_mask & (1u << primary_weapon)) == 0) {
        return false;
    }
    if (secondary_weapon != netcore::kNoWeapon && secondary_weapon < 32u && (avail_mask & (1u << secondary_weapon)) == 0) {
        return false;
    }
    const netcore::LoadoutRequest req{
        .primary_weapon = primary_weapon,
        .secondary_weapon = secondary_weapon,
        .tactical_flags = tactical_flags,
    };

    if (g_role == Role::Host && g_host_session) {
        const uint8_t prev_p = g_host_session->slot_loadout_state(0).primary_weapon;
        const bool ok = g_host_session->local_buy_loadout(req);
        if (ok) {
            g_pending_weapon_apply = true;
            g_pending_equip_slot = (primary_weapon != netcore::kNoWeapon && primary_weapon != prev_p) ? 0u : 1u;
            if (primary_weapon != netcore::kNoWeapon && secondary_weapon == netcore::kNoWeapon) g_pending_equip_slot = 0u;
        }
        return ok;
    }
    if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        return g_client_session->send_loadout_request(req);
    }

    return false;
}

bool RequestEquipSlot(uint8_t slot_idx) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::OfflineCampaign || slot_idx > 1u) return false;
    g_pending_slot_switch = slot_idx;
    g_has_pending_slot_switch = true;
    return true;
}

std::string GetWeaponCatalogJson(const uint8_t* guest_base) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    netcore::RulesConfig rules = netcore::RulesConfig::cs_loadout_deathmatch();
    uint8_t arena_id = 0;
    uint8_t game_mode = static_cast<uint8_t>(rules.game_mode);
    uint32_t session_mask = 0xFFFFFFFFu;
    if (g_role == Role::Host && g_host_session) {
        rules = g_host_session->rules();
        arena_id = g_host_session->active_arena_id();
        game_mode = static_cast<uint8_t>(rules.game_mode);
        session_mask = g_host_session->available_weapon_mask();
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        arena_id = g_client_session->active_arena_id();
        game_mode = g_client_session->game_mode();
        session_mask = g_client_session->available_weapon_mask();
        if (game_mode == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault)) {
            rules = netcore::RulesConfig::territorial_assault();
        }
    }
    uint32_t def_count = 0;
    uint8_t c2e[16]{};
    uint8_t e2c[32]{};
    const uint32_t local_mask = ComputeLoadedCatalogMapping(guest_base, c2e, e2c, &def_count);
    const uint32_t effective_mask = (def_count > 0) ? (session_mask & local_mask) : session_mask;
    std::string out;
    out.reserve(4096);
    out += "{";
    char hdr[320];
    std::snprintf(hdr, sizeof(hdr),
                  "\"maxCredits\":%u,\"armorPrice\":%u,\"armorBonus\":%u,\"tacticalPrice\":%u,\"buyWindowTicks\":%u,"
                  "\"engineWeaponCount\":%u,\"arenaId\":%u,\"gameMode\":%u,\"availableMask\":%u,\"weapons\":[",
                  static_cast<unsigned>(rules.max_credits), static_cast<unsigned>(rules.armor_price),
                  static_cast<unsigned>(rules.armor_bonus_health), static_cast<unsigned>(rules.tactical_price),
                  static_cast<unsigned>(rules.buy_window_ticks), static_cast<unsigned>(def_count),
                  static_cast<unsigned>(arena_id), static_cast<unsigned>(game_mode),
                  static_cast<unsigned>(effective_mask));
    out += hdr;
    for (std::size_t i = 0; i < rules.weapons.size(); ++i) {
        const auto& w = rules.weapons[i];
        const bool avail = (w.id >= 32u) || ((effective_mask & (1u << w.id)) != 0);
        const int eng_id = (def_count > 0 && w.id < 16u && c2e[w.id] != 0xFFu) ? static_cast<int>(c2e[w.id]) : -1;
        const char* tag = (w.id < 16u) ? kCatalogNativeTags[w.id] : "";
        if (i > 0) out += ",";
        char row[320];
        std::snprintf(row, sizeof(row),
                      "{\"id\":%u,\"name\":\"%s\",\"slot\":%u,\"price\":%u,\"damage\":%u,\"mag\":%u,\"range\":%.0f,"
                      "\"fireTicks\":%u,\"available\":%s,\"engineId\":%d,\"tag\":\"%s\"}",
                      static_cast<unsigned>(w.id), w.name ? w.name : "Weapon",
                      static_cast<unsigned>(w.slot), static_cast<unsigned>(w.price),
                      static_cast<unsigned>(w.damage), static_cast<unsigned>(w.magazine_size),
                      static_cast<double>(w.max_range), static_cast<unsigned>(w.min_fire_interval_ticks),
                      avail ? "true" : "false", eng_id, tag);
        out += row;
    }
    out += "]}";
    return out;
}

bool ConsumeWeaponApplyRequest(const uint8_t* base, uint8_t& out_primary_eng_id,
                               uint8_t& out_secondary_eng_id, uint8_t& out_equip_slot,
                               uint8_t& out_tactical_flags) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (!g_pending_weapon_apply || !base || !black::IsInLevel(base)) return false;
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!IsGuestCommitted(player, 0x3C0)) return false;
    if (black::R32(base, player + black::kCharState) != 0) return false;
    const uint32_t slots_arr = black::R32(base, player + black::kCharSlotsPtr);
    if (!IsGuestCommitted(slots_arr, 8)) return false;

    const uint32_t def_count = ReadLoadedWeaponDefCount(base);
    if (def_count == 0) return false;

    uint8_t prim = netcore::kNoWeapon;
    uint8_t sec = 0;
    uint8_t tact = 0;
    if (g_role == Role::Host && g_host_session) {
        const auto ls = g_host_session->slot_loadout_state(0);
        prim = ls.primary_weapon;
        sec = ls.secondary_weapon;
        tact = ls.tactical_flags;
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        const auto ls = g_client_session->local_loadout();
        prim = ls.primary_weapon;
        sec = ls.secondary_weapon;
        tact = ls.tactical_flags;
    } else {
        g_pending_weapon_apply = false;
        return false;
    }

    g_pending_weapon_apply = false;
    out_primary_eng_id = MapCatalogToEngineWeaponId(base, prim, def_count);
    out_secondary_eng_id = MapCatalogToEngineWeaponId(base, sec, def_count);
    out_equip_slot = g_pending_equip_slot;
    out_tactical_flags = tact;
    LOGR("mp_bridge: applying XBE loadout prim=%u(eng=%u) sec=%u(eng=%u) tact=0x%02X equip=%u defs=%u",
         static_cast<unsigned>(prim), static_cast<unsigned>(out_primary_eng_id),
         static_cast<unsigned>(sec), static_cast<unsigned>(out_secondary_eng_id),
         static_cast<unsigned>(tact), static_cast<unsigned>(out_equip_slot), static_cast<unsigned>(def_count));
    return (out_primary_eng_id != 0xFFu || out_secondary_eng_id != 0xFFu);
}

bool ConsumeSlotSwitchRequest(const uint8_t* base, uint8_t& out_slot_idx) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (!g_has_pending_slot_switch || !base || !black::IsInLevel(base)) return false;
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!IsGuestCommitted(player, 0x3C0) || black::R32(base, player + black::kCharState) != 0) return false;
    g_has_pending_slot_switch = false;
    out_slot_idx = g_pending_slot_switch;
    return true;
}

bool ConsumeRemoteWeaponApplyRequest(const uint8_t* base, uint32_t& out_actor_va, uint8_t& out_eng_weapon_id) {
    out_actor_va = 0;
    out_eng_weapon_id = 0xFFu;
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::OfflineCampaign || !base || !black::IsInLevel(base)) return false;
    const uint32_t def_count = ReadLoadedWeaponDefCount(base);
    if (def_count == 0) return false;

    for (std::size_t slot = 0; slot < netcore::kMaxPlayers; ++slot) {
        const uint8_t cat_id = g_pending_remote_weapon[slot];
        if (cat_id == netcore::kNoWeapon) continue;

        const uint32_t actor_va = g_remote_actor_va[slot];
        if (!IsGuestCommitted(actor_va, 0x3C0)) {
            g_pending_remote_weapon[slot] = netcore::kNoWeapon;
            g_remote_applied_weapon[slot] = netcore::kNoWeapon;
            continue;
        }
        if (black::R32(base, actor_va + black::kCharState) != 0) {
            continue;
        }

        const uint32_t slots_arr = black::R32(base, actor_va + black::kCharSlotsPtr);
        const uint8_t slot_count = black::R8(base, actor_va + black::kCharSlotCount);
        const uint8_t eng_id = MapCatalogToEngineWeaponId(base, cat_id, def_count);
        g_pending_remote_weapon[slot] = netcore::kNoWeapon;
        g_remote_applied_weapon[slot] = cat_id;

        if (!IsGuestCommitted(slots_arr, 4) || slot_count == 0 || eng_id == 0xFFu) {
            continue;
        }
        const uint32_t cur_wpn = black::R32(base, actor_va + black::kCharWeapon);
        if (IsGuestCommitted(cur_wpn, 4) && black::R8(base, cur_wpn) == eng_id) {
            continue;
        }

        out_actor_va = actor_va;
        out_eng_weapon_id = eng_id;
        return true;
    }
    return false;
}

void NotifyHitmarker(uint8_t target_slot, bool kill) {
    (void)target_slot;
    (void)kill;
    g_hitmarker_seq.fetch_add(1u, std::memory_order_relaxed);
}

uint64_t HitmarkerSeq() {
    return g_hitmarker_seq.load(std::memory_order_relaxed);
}

void StopMultiplayer() {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    g_role = Role::OfflineCampaign;
    g_client_session.reset();
    g_host_session.reset();
#ifndef __EMSCRIPTEN__
    g_udp_transport.reset();
#endif
    g_webrtc_transport.reset();
    g_local_endpoint.reset();
    ResetLevelState();
    g_client_join_logged = false;
    g_last_client_hello_frame = 0;
    g_direct_launch_pending.store(false, std::memory_order_release);
    g_prematch_configured = false;
    g_direct_launch_fe_frames = 0;
    g_prematch_team_applied = false;
    g_prematch_kit_applied = false;
    g_host_auto_round_frames = 0;
    g_host_auto_round_started = false;
}

void ConfigurePreMatchLaunch(uint8_t mission_idx, uint8_t local_team, uint8_t primary_weapon,
                             uint8_t secondary_weapon, uint8_t tactical_flags) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::OfflineCampaign) return;
    g_prematch_configured = true;
    g_prematch_mission_idx = static_cast<uint8_t>(mission_idx & 7u);
    g_prematch_team = static_cast<uint8_t>(local_team & 1u);
    g_prematch_primary = primary_weapon;
    g_prematch_secondary = secondary_weapon;
    g_prematch_tactical = tactical_flags;
    g_direct_launch_fe_frames = 0;
    g_prematch_team_applied = false;
    g_prematch_kit_applied = false;
    g_host_auto_round_frames = 0;
    g_host_auto_round_started = false;
    g_direct_launch_pending.store(true, std::memory_order_release);
    if (g_role == Role::Host && g_host_session) {
        g_host_session->set_player_team(0, g_prematch_team == 0u ? netcore::ta::Team::A : netcore::ta::Team::B);
        g_prematch_team_applied = true;
    }
    LOGR("mp_bridge: pre-match launch configured (mission=%u internal=%u team=%u kit=%u/%u/0x%02X)",
         static_cast<unsigned>(g_prematch_mission_idx),
         static_cast<unsigned>(black::kMissionInternalLevelIds[g_prematch_mission_idx]),
         static_cast<unsigned>(g_prematch_team), static_cast<unsigned>(g_prematch_primary),
         static_cast<unsigned>(g_prematch_secondary), static_cast<unsigned>(g_prematch_tactical));
}

bool IsDirectMapLaunchPending() {
    return g_role != Role::OfflineCampaign && g_direct_launch_pending.load(std::memory_order_acquire);
}

bool IsClientJoined() {
    return g_client_session && g_client_session->is_joined();
}

void OnImageLoaded(uint8_t* guest_base) {
    g_guest_base = guest_base;
    g_guest_scratch_va = 0;
    g_spawn_scratch_va = 0;
    LOGI("mp_bridge: guest image mapped at %p", static_cast<void*>(guest_base));
}

void OnPreFrame(uint8_t* guest_base) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    g_guest_base = guest_base;
    if (!black::IsInLevel(guest_base)) {
        common::ClearDirectMouseLook();
    }
    if (g_role == Role::OfflineCampaign) return;

    const bool in_level = black::IsInLevel(guest_base);
    if (!in_level && (g_level_spawns_initialized || g_has_last_local_pos ||
                      g_remote_actor_va[0] != 0 || g_remote_actor_va[1] != 0 ||
                      g_remote_actor_va[2] != 0 || g_remote_actor_va[3] != 0)) {
        ResetLevelState();
    }

    // Carregamento direto do mapa escolhido no pré-lobby: assim que o FrontEnd (game+0x20220) estabilizar,
    // dispara o gatilho oficial de transição (sub_001132E0 -> FE+0x3880..0x3888 -> sub_0020DCA0) sem menu Xbox.
    if (!in_level && g_direct_launch_pending.load(std::memory_order_acquire)) {
        const uint32_t game = IsGuestCommitted(black::kGame, 4) ? black::R32(guest_base, black::kGame) : 0u;
        const uint32_t fe = IsGuestCommitted(black::kFrontEnd, 4) ? black::R32(guest_base, black::kFrontEnd) : 0u;
        if (IsGuestCommitted(game, 0x21080) && IsGuestCommitted(fe, 0x3910) &&
            black::R32(guest_base, game + black::kGameState) == game + black::kFrontEndState) {
            ++g_direct_launch_fe_frames;
            if (g_direct_launch_fe_frames == 12u ||
                (g_direct_launch_fe_frames > 12u && (g_direct_launch_fe_frames % 120u) == 0u)) {
                const uint8_t internal_lvl = black::kMissionInternalLevelIds[g_prematch_mission_idx & 7u];
                black::W8(guest_base, game + black::kGameTargetLevel, internal_lvl);
                black::W8(guest_base, game + black::kGameTargetDiff, 1u);
                black::W8(guest_base, game + black::kGameTargetUnit, 1u);
                black::W32(guest_base, fe + black::kFeNextState, game + black::kInLevelState);
                black::W32(guest_base, fe + black::kFeTriggerA, 1u);
                black::W32(guest_base, fe + black::kFeTriggerB, 1u);
                LOGR("mp_bridge: direct map launch triggered (mission=%u internal=%u fe_frames=%u)",
                     static_cast<unsigned>(g_prematch_mission_idx), static_cast<unsigned>(internal_lvl),
                     g_direct_launch_fe_frames);
            }
        }
    } else if (in_level && g_direct_launch_pending.load(std::memory_order_acquire)) {
        g_direct_launch_pending.store(false, std::memory_order_release);
        LOGR("mp_bridge: direct map launch arrived in-level (mission=%u)",
             static_cast<unsigned>(g_prematch_mission_idx));
    }

    auto apply_prematch_kit = [&](uint32_t avail_mask) {
        const netcore::RulesConfig ta_rules = netcore::RulesConfig::territorial_assault();
        uint8_t prim = g_prematch_primary;
        uint8_t sec = g_prematch_secondary;
        uint8_t tact = g_prematch_tactical;
        auto is_avail = [&](uint8_t wid) {
            return wid < 32u && (avail_mask & (1u << wid)) != 0;
        };
        if (sec != netcore::kNoWeapon && !is_avail(sec)) sec = 0;
        if (prim == netcore::kNoWeapon || !is_avail(prim)) {
            prim = netcore::kNoWeapon;
            for (const auto& w : ta_rules.weapons) {
                if (w.slot == netcore::WeaponSlot::Primary && is_avail(w.id) && w.price <= ta_rules.max_credits) {
                    prim = w.id;
                    break;
                }
            }
        }
        auto kit_cost = [&](uint8_t p, uint8_t s, uint8_t t) -> uint32_t {
            uint32_t c = 0;
            if (const auto* wp = ta_rules.find_weapon(p)) c += wp->price;
            if (const auto* ws = ta_rules.find_weapon(s)) c += ws->price;
            if (t & netcore::kLoadoutArmor) c += ta_rules.armor_price;
            if (t & netcore::kLoadoutTactical) c += ta_rules.tactical_price;
            return c;
        };
        if (kit_cost(prim, sec, tact) > ta_rules.max_credits) tact &= static_cast<uint8_t>(~netcore::kLoadoutTactical);
        if (kit_cost(prim, sec, tact) > ta_rules.max_credits) tact &= static_cast<uint8_t>(~netcore::kLoadoutArmor);
        if (kit_cost(prim, sec, tact) > ta_rules.max_credits) sec = 0;
        if (prim != netcore::kNoWeapon) {
            (void)BuyLoadout(prim, sec, tact);
        }
    };

    if (g_role == Role::Host && g_host_session) {
        if (in_level && !g_level_spawns_initialized) {
            InitializeLevelContextIfReady(guest_base);
            g_last_host_tick_tp = {};
            g_host_tick_accum_sec = 0.0;
        }

        // Simula o HostSession na cadência exata (30 Hz) usando relógio monotônico real,
        // sem acelerar os timers de compra/partida quando o renderizador WebGPU roda a 60+ FPS.
        const auto now_tp = std::chrono::steady_clock::now();
        if (g_last_host_tick_tp.time_since_epoch().count() == 0) {
            g_last_host_tick_tp = now_tp;
            g_host_session->tick();
        } else {
            double dt = std::chrono::duration<double>(now_tp - g_last_host_tick_tp).count();
            g_last_host_tick_tp = now_tp;
            if (dt < 0.0) dt = 0.0;
            if (dt > 0.1) dt = 0.1;
            g_host_tick_accum_sec += dt;
            const double step =
                1.0 / static_cast<double>(std::max<uint16_t>(1, g_host_session->rules().tick_rate_hz));
            int steps = 0;
            while (g_host_tick_accum_sec >= step && steps < 2) {
                g_host_session->tick();
                g_host_tick_accum_sec -= step;
                ++steps;
            }
        }

        if (in_level) {
            uint8_t c2e[16]{};
            uint8_t e2c[32]{};
            uint32_t def_cnt = 0;
            const uint32_t mask = ComputeLoadedCatalogMapping(guest_base, c2e, e2c, &def_cnt);
            const uint32_t world = IsGuestCommitted(black::kWorld, 4) ? black::R32(guest_base, black::kWorld) : 0u;
            const uint8_t arena_id = (world && IsGuestCommitted(world + black::kWorldLevel, 4))
                                         ? black::R8(guest_base, world + black::kWorldLevel)
                                         : 0u;
            g_host_session->set_level_context(arena_id, mask);
            // Fluxo automático do Pré-Lobby Territorial Assault: aplica time/kit pré-escolhidos e inicia Rodada 1.
            if (g_prematch_configured &&
                g_host_session->rules().game_mode == netcore::GameMode::TerritorialAssault) {
                if (!g_prematch_team_applied) {
                    g_host_session->set_player_team(0, g_prematch_team == 0u ? netcore::ta::Team::A : netcore::ta::Team::B);
                    g_prematch_team_applied = true;
                }
                if (!g_host_session->territorial_match()) {
                    const std::size_t expected_players =
                        1u + (g_webrtc_transport ? g_webrtc_transport->peer_count() : 0u);
                    const auto& sl = g_host_session->slots();
                    int cnt_a = 0, cnt_b = 0;
                    for (const auto& s : sl) {
                        if (!s.occupied) continue;
                        if (s.team == 0) ++cnt_a;
                        else ++cnt_b;
                    }
                    if (g_host_session->player_count() >= expected_players && cnt_a > 0 && cnt_b > 0) {
                        (void)g_host_session->enter_preparation();
                    }
                }
                if (g_host_session->territorial_match() &&
                    g_host_session->territorial_match()->state() == netcore::ta::MatchState::Preparation) {
                    if (!g_prematch_kit_applied) {
                        apply_prematch_kit(mask);
                        g_prematch_kit_applied = true;
                    }
                    ++g_host_auto_round_frames;
                    bool all_have_primary = true;
                    for (std::size_t i = 0; i < netcore::kMaxPlayers; ++i) {
                        if (!g_host_session->slots()[i].occupied) continue;
                        if (g_host_session->slot_loadout_state(static_cast<uint8_t>(i)).primary_weapon ==
                            netcore::kNoWeapon) {
                            all_have_primary = false;
                            break;
                        }
                    }
                    if (!g_host_auto_round_started &&
                        ((all_have_primary && g_host_auto_round_frames >= 15u) || g_host_auto_round_frames >= 75u)) {
                        if (g_host_session->begin_next_round()) {
                            g_host_auto_round_started = true;
                            LOGR("mp_bridge: auto-started Territorial Assault Round 1 (frames=%u, all_kits=%d)",
                                 g_host_auto_round_frames, all_have_primary ? 1 : 0);
                        }
                    }
                }
            }
            if (!g_local_fps_control_started) {
                const uint32_t world = black::R32(guest_base, black::kWorld);
                const uint32_t player = world ? (world + black::kWorldPlayer) : 0;
                const uint32_t emb_ctrl = world ? (world + black::kWorldController) : 0;
                const uint32_t pctrl =
                    (player && IsGuestCommitted(player, 0x3C0))
                        ? black::R32(guest_base, player + black::kCharControl)
                        : 0;
                if (emb_ctrl != 0 && pctrl == emb_ctrl) {
                    g_local_fps_control_started = true;
                    g_host_session->refresh_buy_window(0);
                    LOGR("mp_bridge: playable FPS control active (host buy window refreshed)");
                }
            }
            const auto& slots = g_host_session->slots();
            // Respawn do host local (slot 0) caso tenha acabado de reviver no HostSession
            if (g_local_waiting_respawn && (slots[0].flags & netcore::kFlagAlive) != 0) {
                RestoreAndTeleportLocalPlayer(guest_base, slots[0].position, slots[0].yaw);
            }
            SyncLocalPlayerHealthAndLife(guest_base, slots[0].health,
                                         (slots[0].flags & netcore::kFlagAlive) != 0);

            // Auto-vincula/spawna dedicados e aplica estado replicado aos atores remotos (slots 1..3)
            for (std::size_t i = 1; i < netcore::kMaxPlayers; ++i) {
                const uint8_t slot_id = static_cast<uint8_t>(i);
                if (!slots[i].occupied) {
                    if (g_remote_actor_va[i] != 0) {
                        ReleaseDedicatedRemoteActor(guest_base, slot_id);
                        UnbindRemoteActor(slot_id);
                    }
                    g_pending_remote_spawn_mask &= static_cast<uint8_t>(~(1u << slot_id));
                    continue;
                }
                AcquireOrQueueRemoteActor(guest_base, slot_id);
                if (g_remote_actor_va[i] == 0) continue;
                netcore::SnapshotPlayer sp{
                    .player_id = slots[i].player_id,
                    .position = slots[i].position,
                    .yaw = slots[i].yaw,
                    .pitch = slots[i].pitch,
                    .velocity = slots[i].velocity,
                    .anim_state = slots[i].anim_state,
                    .weapon = slots[i].weapon,
                    .health = slots[i].health,
                    .flags = slots[i].flags,
                    .kills = slots[i].kills,
                    .deaths = slots[i].deaths,
                };
                ApplySnapshotToActor(guest_base, g_remote_actor_va[i], sp);
            }
            for (const auto& fev : g_host_session->recent_fires()) {
                if (fev.shooter != 0 && g_pending_remote_fires.size() < 16) {
                    g_pending_remote_fires.push_back(fev);
                }
            }
            g_host_session->clear_events();
        } else {
            g_host_session->clear_events();
        }
    } else if (g_role == Role::Client && g_client_session) {
        g_client_session->poll();

        if (g_client_session->is_rejected() || g_client_session->is_kicked()) {
            LOGE("mp_bridge: UDP client rejected or kicked from host; returning to OfflineCampaign");
            StopMultiplayer();
            return;
        }

        if (!g_client_session->is_joined()) {
            if (g_frame_count == 5 || g_frame_count >= g_last_client_hello_frame + 30) {
                g_client_session->send_hello();
                g_last_client_hello_frame = g_frame_count;
            }
        } else {
            if (!g_client_join_logged) {
                LOGR("mp_bridge: client joined match as player_id=%u",
                     static_cast<unsigned>(g_client_session->local_player_id()));
                g_client_join_logged = true;
            }
            if (g_prematch_configured && !g_prematch_team_applied &&
                g_client_session->game_mode() == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault)) {
                if (g_client_session->send_team_select(g_prematch_team == 0u ? netcore::ta::Team::A
                                                                              : netcore::ta::Team::B)) {
                    g_prematch_team_applied = true;
                }
            }
            if (g_prematch_configured && !g_prematch_kit_applied &&
                g_client_session->game_mode() == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault)) {
                const auto& opt_ta = g_client_session->latest_territorial_state();
                if (opt_ta.has_value() && opt_ta->match_state == 1u) {
                    apply_prematch_kit(g_client_session->available_weapon_mask());
                    g_prematch_kit_applied = true;
                }
            }

            if (in_level) {
                if (!g_level_spawns_initialized) {
                    InitializeLevelContextIfReady(guest_base);
                }
                const uint8_t local_id = g_client_session->local_player_id();
                for (const auto& resp : g_client_session->recent_respawns()) {
                    if (resp.player == local_id) {
                        const uint32_t world = black::R32(guest_base, black::kWorld);
                        const uint32_t player = world ? (world + black::kWorldPlayer) : 0;
                        const uint32_t emb_ctrl = world ? (world + black::kWorldController) : 0;
                        const uint32_t pctrl =
                            (player && IsGuestCommitted(player, 0x3C0))
                                ? black::R32(guest_base, player + black::kCharControl)
                                : 0;
                        if (g_local_waiting_respawn || (emb_ctrl != 0 && pctrl == emb_ctrl)) {
                            RestoreAndTeleportLocalPlayer(guest_base, resp.position, resp.yaw);
                            g_awaiting_alive_snapshot = true;
                        } else {
                            LOGI("mp_bridge: skipped mid-cutscene respawn event (pos=(%.1f,%.1f,%.1f))",
                                 resp.position.x, resp.position.y, resp.position.z);
                        }
                    }
                }
                for (const auto& fev : g_client_session->recent_fires()) {
                    if (fev.shooter != local_id && g_pending_remote_fires.size() < 16) {
                        g_pending_remote_fires.push_back(fev);
                    }
                }
                for (const auto& dmg : g_client_session->recent_damages()) {
                    if (dmg.attacker == local_id && dmg.target != local_id) {
                        NotifyHitmarker(dmg.target, dmg.health_after == 0);
                    }
                }
                for (const auto& ls : g_client_session->recent_loadouts()) {
                    if (ls.player_id == local_id) {
                        LOGR("mp_bridge: client loadout_state prim=%u sec=%u tact=0x%02X credits=%u buy=%u res=%u",
                             static_cast<unsigned>(ls.primary_weapon), static_cast<unsigned>(ls.secondary_weapon),
                             static_cast<unsigned>(ls.tactical_flags), static_cast<unsigned>(ls.credits),
                             static_cast<unsigned>(ls.buy_ticks_left), static_cast<unsigned>(ls.last_result));
                    }
                    if (ls.player_id == local_id &&
                        ls.last_result == static_cast<uint8_t>(netcore::LoadoutResult::Ok) &&
                        (ls.primary_weapon != g_last_synced_primary ||
                         ls.secondary_weapon != g_last_synced_secondary ||
                         ls.tactical_flags != g_last_synced_tactical)) {
                        const bool bought_primary = (ls.primary_weapon != netcore::kNoWeapon &&
                                                     ls.primary_weapon != g_last_synced_primary);
                        g_last_synced_primary = ls.primary_weapon;
                        g_last_synced_secondary = ls.secondary_weapon;
                        g_last_synced_tactical = ls.tactical_flags;
                        g_pending_equip_slot = bought_primary ? 0u : 1u;
                        g_pending_weapon_apply = true;
                    }
                }
                g_client_session->clear_events();

                const auto& snap = g_client_session->latest_snapshot();
                std::array<bool, netcore::kMaxPlayers> seen_remote{};
                for (const auto& sp : snap.players) {
                    if (sp.player_id == local_id) {
                        SyncLocalPlayerHealthAndLife(
                            guest_base, sp.health, (sp.flags & netcore::kFlagAlive) != 0);
                        continue;
                    }
                    if (sp.player_id < netcore::kMaxPlayers) {
                        seen_remote[sp.player_id] = true;
                        AcquireOrQueueRemoteActor(guest_base, sp.player_id);
                        if (g_remote_actor_va[sp.player_id] != 0) {
                            ApplySnapshotToActor(guest_base, g_remote_actor_va[sp.player_id], sp);
                        }
                    }
                }
                if (!snap.players.empty()) {
                    for (uint8_t slot = 0; slot < netcore::kMaxPlayers; ++slot) {
                        if (slot != local_id && !seen_remote[slot] && g_remote_actor_va[slot] != 0) {
                            ReleaseDedicatedRemoteActor(guest_base, slot);
                            UnbindRemoteActor(slot);
                        } else if (slot != local_id && !seen_remote[slot]) {
                            g_pending_remote_spawn_mask &= static_cast<uint8_t>(~(1u << slot));
                        }
                    }
                }
            } else {
                g_client_session->clear_events();
            }
        }
    }

    if (in_level) {
        PreventSamePointCollisionCrash(guest_base);
    }
}

void ProbeGuestRuntimeState(const uint8_t* base, uint64_t frame_count) {
    static const bool s_probe_on = (std::getenv("BLACK_RE_PROBE") != nullptr);
    if (!s_probe_on || !base) return;

    auto safe32 = [&](uint32_t va) -> uint32_t {
        return IsGuestCommitted(va, 4) ? black::R32(base, va) : 0u;
    };
    auto safe8 = [&](uint32_t va) -> uint8_t {
        return IsGuestCommitted(va, 1) ? black::R8(base, va) : 0u;
    };
    auto safef32 = [&](uint32_t va) -> float {
        return IsGuestCommitted(va, 4) ? black::RF32(base, va) : 0.0f;
    };

    const uint32_t game = safe32(black::kGame);
    const uint32_t state = game ? safe32(game + black::kGameState) : 0;
    const uint32_t expected_in_level = game ? (game + black::kInLevelState) : 0;
    const uint32_t world = safe32(black::kWorld);
    const uint32_t kframe = safe32(black::kFrameCounter);
    const bool in_level = (game != 0 && state == expected_in_level);

    static uint32_t last_game = 0xFFFFFFFFu;
    static uint32_t last_state = 0xFFFFFFFFu;
    static uint32_t last_world = 0xFFFFFFFFu;
    static bool last_in_level = false;

    const bool changed = (game != last_game || state != last_state || world != last_world || in_level != last_in_level);
    if (changed) {
        LOGI("re_probe: state frame=%llu kFrame=%u game=0x%08X state=0x%08X state_vt=0x%08X inLevelPtr=0x%08X inLevel=%d world=0x%08X",
             static_cast<unsigned long long>(frame_count), kframe, game, state, safe32(state), expected_in_level,
             in_level ? 1 : 0, world);
        last_game = game;
        last_state = state;
        last_world = world;
        last_in_level = in_level;
    }

    if (!world || !(changed || (frame_count % 60 == 0))) return;

    const uint8_t lvl8 = safe8(world + black::kWorldLevel);
    const uint32_t lvl32 = safe32(world + black::kWorldLevel);
    const uint32_t unit = safe32(world + black::kWorldUnit);
    const uint32_t player = world + black::kWorldPlayer;
    const uint32_t p_vt = safe32(player);
    const uint32_t n0 = safe32(player + black::kCharName + 0);
    const uint32_t n1 = safe32(player + black::kCharName + 4);
    const float px = safef32(player + black::kCharPos + 0);
    const float py = safef32(player + black::kCharPos + 4);
    const float pz = safef32(player + black::kCharPos + 8);
    const float hp = safef32(player + black::kCharHealth);
    const uint32_t pstate = safe32(player + black::kCharState);
    const uint32_t team = safe32(player + black::kCharTeam);
    const uint8_t crouch = safe8(player + black::kCharCrouch);
    const uint32_t pctrl = safe32(player + black::kCharControl);
    const uint32_t emb_ctrl = world + black::kWorldController;
    const uint32_t active_ctrl = pctrl ? pctrl : emb_ctrl;
    const uint32_t ctrl_vt = safe32(active_ctrl);
    const uint32_t ctrl_owner = safe32(active_ctrl + black::kControlChar);
    const float yaw = safef32(active_ctrl + black::kControlAngles + 8);
    const float pitch = safef32(active_ctrl + black::kControlAngles + 12);
    const uint32_t weapon = safe32(player + black::kCharWeapon);

    LOGI("re_probe: player f=%llu lvl8=%u lvl32=0x%08X unit=%u player=0x%08X(vt=0x%08X) name=%08X:%08X pos=(%.2f,%.2f,%.2f) hp=%.1f st=%u team=%u crouch=%u ctrl=0x%08X(emb=0x%08X,vt=0x%08X,own=0x%08X) yaw=%.1f pitch=%.1f sens=(%.2f,%.2f) wpn=0x%08X",
         static_cast<unsigned long long>(frame_count), static_cast<unsigned>(lvl8), lvl32, unit, player, p_vt, n0, n1,
         px, py, pz, hp, pstate, team, static_cast<unsigned>(crouch), pctrl, emb_ctrl, ctrl_vt, ctrl_owner, yaw, pitch,
         safef32(active_ctrl + black::kControlSensYaw), safef32(active_ctrl + black::kControlSensPitch), weapon);

    if (in_level && g_last_weapon_def_dump_lvl != lvl32) {
        const uint32_t ws = safe32(black::kWeaponSystem);
        const uint32_t def_hdr = ws ? safe32(ws) : 0;
        const uint32_t def_cnt = def_hdr ? static_cast<uint32_t>(safe8(def_hdr + 1u)) : 0;
        const uint32_t def_arr = def_hdr ? safe32(def_hdr + 4u) : 0;
        if (def_cnt > 0 && def_cnt <= 32 && def_arr && IsGuestCommitted(def_arr, def_cnt * 32u)) {
            g_last_weapon_def_dump_lvl = lvl32;
            LOGI("re_probe: WeaponDefs count=%u arr=0x%08X slotsPtr=0x%08X slotCnt=%u activeSlot=%u activeId=%u",
                 def_cnt, def_arr, safe32(player + black::kCharSlotsPtr),
                 static_cast<unsigned>(safe8(player + black::kCharSlotCount)),
                 static_cast<unsigned>(safe8(player + black::kCharActiveSlot)),
                 static_cast<unsigned>(safe8(player + black::kCharActiveWeaponId)));
            for (uint32_t wi = 0; wi < def_cnt; ++wi) {
                const uint32_t wdef = def_arr + wi * 32u;
                const uint32_t h0 = safe32(wdef + 0u);
                const uint32_t h1 = safe32(wdef + 4u);
                char tag[9]{};
                for (int c = 0; c < 8; ++c) {
                    const uint8_t ch = safe8(wdef + static_cast<uint32_t>(c));
                    tag[c] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '.';
                }
                LOGI("re_probe:   def[%02u] id=%08X:%08X '%s' w2=0x%08X w3=0x%08X",
                     wi, h0, h1, tag, safe32(wdef + 8u), safe32(wdef + 12u));
            }
        }
    }

    const uint32_t pool = safe32(black::kPool);
    if (!pool) return;
    const uint32_t list_base = pool + black::kAliveList;
    const uint32_t flags = safe32(list_base + 0);
    const uint32_t cap = safe32(list_base + 4);
    const uint32_t count_field = safe32(list_base + 8);
    const uint32_t arr = safe32(list_base + 0x0C);
    LOGI("re_probe: pool=0x%08X list flags=0x%08X cap=%u cnt=%u arr=0x%08X", pool, flags, cap, count_field, arr);
    if (!arr || cap == 0 || cap > 256) return;

    uint32_t shown = 0;
    uint32_t total_valid = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        const uint32_t a = safe32(arr + i * 4);
        if (!IsGuestCommitted(a, 0x3C0)) continue;
        ++total_valid;
        if (shown < 6) {
            const uint32_t actrl = safe32(a + black::kCharControl);
            const uint32_t awpn = safe32(a + black::kCharWeapon);
            LOGI("re_probe: actor[%u]=0x%08X vt=0x%08X team=%u hp=%.1f st=%u kin=%u hit=%u wpn=0x%08X ctrl=0x%08X(vt=0x%08X,own=0x%08X) pos=(%.2f,%.2f,%.2f)",
                 i, a, safe32(a), safe32(a + black::kCharTeam), safef32(a + black::kCharHealth),
                 safe32(a + black::kCharState), static_cast<unsigned>(safe8(a + black::kCharKinematic)),
                 static_cast<unsigned>(safe8(a + black::kCharHit)), awpn, actrl, safe32(actrl),
                 safe32(actrl + black::kControlChar), safef32(a + black::kCharPos + 0),
                 safef32(a + black::kCharPos + 4), safef32(a + black::kCharPos + 8));
            ++shown;
        }
    }
    LOGI("re_probe: pool valid actors=%u/%u", total_valid, cap);
}

void OnPostFrame(uint8_t* guest_base) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    ++g_frame_count;
    if (g_frame_count == 1) {
        LOGR("mp_bridge: first guest frame hook fired (sub_0020F6A0)");
    }
    ProbeGuestRuntimeState(guest_base, g_frame_count);
    if (g_role == Role::OfflineCampaign) return;

    netcore::PlayerState local_state{};
    uint32_t tick = 0;
    if (g_host_session) {
        tick = g_host_session->current_tick();
    } else {
        const auto now_tp = std::chrono::steady_clock::now();
        if (g_client_tick_origin_tp.time_since_epoch().count() == 0) {
            g_client_tick_origin_tp = now_tp;
        }
        const double elapsed_s = std::max(0.0, std::chrono::duration<double>(now_tp - g_client_tick_origin_tp).count());
        tick = static_cast<uint32_t>(elapsed_s * 30.0) + 1u;
    }
    if (!ReadLocalPlayerFromGuest(guest_base, tick, local_state)) return;

    if (g_role == Role::Host && g_host_session) {
        g_host_session->set_local_player_state(local_state);
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        g_client_session->send_local_state(local_state);
    }

    if (!g_saw_weapon_fire_hook) {
        const uint32_t world = black::R32(guest_base, black::kWorld);
        const uint32_t ctrl = world ? (world + black::kWorldController) : 0;
        const bool firing = (ctrl != 0 && (local_state.flags & netcore::kFlagAlive) != 0 &&
                             black::R8(guest_base, ctrl + black::kControlFire) != 0);
        if (firing && (!g_prev_local_firing || tick >= g_last_local_fire_tick + 5u)) {
            EmitLocalFireEvent(guest_base, tick, local_state.weapon, netcore::kNoPlayer);
        }
        g_prev_local_firing = firing;
    } else {
        g_prev_local_firing = false;
    }
}

void BindRemoteActor(uint8_t player_id, uint32_t actor_va) {
    if (player_id < netcore::kMaxPlayers) {
        g_remote_actor_va[player_id] = actor_va;
        g_has_applied_snapshot[player_id] = false;
        g_remote_applied_weapon[player_id] = netcore::kNoWeapon;
        g_pending_remote_weapon[player_id] = netcore::kNoWeapon;
        LOGR("mp_bridge: bound player_id=%u to guest actor 0x%08X", static_cast<unsigned>(player_id), actor_va);
    }
}

void UnbindRemoteActor(uint8_t player_id) {
    if (player_id < netcore::kMaxPlayers) {
        g_remote_actor_va[player_id] = 0;
        g_has_applied_snapshot[player_id] = false;
        g_last_applied_snapshot[player_id] = {};
        g_remote_applied_weapon[player_id] = netcore::kNoWeapon;
        g_pending_remote_weapon[player_id] = netcore::kNoWeapon;
    }
}

bool IsRemoteActor(uint32_t actor_va) {
    if (actor_va == 0) return false;
    for (uint32_t va : g_remote_actor_va) {
        if (va == actor_va) return true;
    }
    return false;
}

bool PrepareRemoteActorSpawn(uint8_t* guest_base, uint8_t& out_slot, uint32_t& out_name_va,
                             uint32_t& out_seq_va, uint32_t& out_arch, uint32_t& out_whash_lo,
                             uint32_t& out_whash_hi, uint32_t& out_spawner_va) {
    out_slot = 0;
    out_name_va = 0;
    out_seq_va = 0;
    out_arch = 0;
    out_whash_lo = 0;
    out_whash_hi = 0;
    out_spawner_va = 0;
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) {
        g_pending_remote_spawn_mask = 0;
        return false;
    }

    uint8_t target_slot = netcore::kNoPlayer;
    for (uint8_t s = 0; s < netcore::kMaxPlayers; ++s) {
        if ((g_pending_remote_spawn_mask & (1u << s)) != 0) {
            g_pending_remote_spawn_mask &= static_cast<uint8_t>(~(1u << s));
            if (g_remote_actor_va[s] == 0 && !g_remote_dedicated_failed[s]) {
                target_slot = s;
                break;
            }
        }
    }
    if (target_slot == netcore::kNoPlayer) return false;

    if (!HasDedicatedSpawnHeadroom(guest_base)) {
        g_remote_dedicated_failed[target_slot] = true;
        return false;
    }

    const uint32_t pool = IsGuestCommitted(black::kPool, 4) ? black::R32(guest_base, black::kPool) : 0u;
    if (!pool || !IsGuestCommitted(pool + 0x7A20u, 44u * 0x40u)) {
        g_remote_dedicated_failed[target_slot] = true;
        return false;
    }
    // Arquétipo 0 é o corpo do próprio jogador (sem malha visível em terceira pessoa): o ator
    // remoto nascia "invisível". Usa o primeiro arquétipo humano carregado (aliado/inimigo).
    uint32_t valid_arch = 0xFFFFFFFFu;
    for (uint32_t a = 1; a < 44u; ++a) {
        if (black::R32(guest_base, pool + 0x7A20u + a * 0x40u) != 0u && a != 0x27u) {
            valid_arch = a;
            break;
        }
    }
    if (valid_arch == 0xFFFFFFFFu) {
        g_remote_dedicated_failed[target_slot] = true;
        return false;
    }

    if (!IsGuestCommitted(black::kWeaponSystem, 4)) {
        g_remote_dedicated_failed[target_slot] = true;
        return false;
    }
    const uint32_t ws = black::R32(guest_base, black::kWeaponSystem);
    const uint32_t def_hdr = ws && IsGuestCommitted(ws, 8) ? black::R32(guest_base, ws) : 0u;
    const uint32_t def_cnt = def_hdr && IsGuestCommitted(def_hdr, 8) ? black::R8(guest_base, def_hdr + 1u) : 0u;
    const uint32_t def_arr = def_cnt > 0 && IsGuestCommitted(def_hdr + 4u, 4) ? black::R32(guest_base, def_hdr + 4u) : 0u;
    if (!def_arr || !IsGuestCommitted(def_arr, 32u)) {
        g_remote_dedicated_failed[target_slot] = true;
        return false;
    }

    if (g_spawn_scratch_va == 0) {
        g_spawn_scratch_va = mem::AllocSystem(256, 16);
    }
    if (!IsGuestCommitted(g_spawn_scratch_va, 128u)) {
        g_remote_dedicated_failed[target_slot] = true;
        return false;
    }

    uint32_t wh_lo = black::R32(guest_base, def_arr + 0u);
    uint32_t wh_hi = black::R32(guest_base, def_arr + 4u);
    if (g_has_applied_snapshot[target_slot]) {
        const uint8_t eng = MapCatalogToEngineWeaponId(guest_base, g_last_applied_snapshot[target_slot].weapon, def_cnt);
        if (eng < def_cnt) {
            wh_lo = black::R32(guest_base, def_arr + eng * 32u + 0u);
            wh_hi = black::R32(guest_base, def_arr + eng * 32u + 4u);
        }
    }

    // Posição inicial: snapshot se disponível, senão ao lado do jogador local.
    netcore::Vec3 pos{0.0f, 0.0f, 0.0f};
    if (g_has_applied_snapshot[target_slot]) {
        pos = g_last_applied_snapshot[target_slot].position;
    } else {
        const uint32_t lp = black::LocalPlayerAddr(guest_base);
        if (IsGuestCommitted(lp, 0xB0u)) {
            pos.x = black::RF32(guest_base, lp + black::kCharPos + 0u) + 1.8f * static_cast<float>(target_slot + 1u);
            pos.y = black::RF32(guest_base, lp + black::kCharPos + 4u);
            pos.z = black::RF32(guest_base, lp + black::kCharPos + 8u) + 1.2f;
        }
    }
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        pos = netcore::Vec3{0.0f, 0.0f, 0.0f};
    }

    const uint32_t name_va = g_spawn_scratch_va + 0x00u;     // +0x08/+0x0C: name_lo/hi
    const uint32_t seq_va = g_spawn_scratch_va + 0x10u;      // uint32_t counter incrementado por sub_001AF880
    const uint32_t spawner_va = g_spawn_scratch_va + 0x20u;  // spawner+0x10 (offset +0x30) = vec4 16B aligned
    std::memset(guest_base + black::FoldVa(name_va), 0, 128u);

    const uint32_t lp = black::LocalPlayerAddr(guest_base);
    const uint32_t nlo = (lp && IsGuestCommitted(lp + black::kCharName, 8))
                             ? black::R32(guest_base, lp + black::kCharName + 0u)
                             : 0x5245594Cu;  // "PLYR"
    const uint32_t nhi = (lp && IsGuestCommitted(lp + black::kCharName, 8))
                             ? black::R32(guest_base, lp + black::kCharName + 4u)
                             : (0x3030u + target_slot);
    black::W32(guest_base, name_va + 0x08u, nlo);
    black::W32(guest_base, name_va + 0x0Cu, nhi);
    static uint32_t s_spawn_seq = 100u;
    black::W32(guest_base, seq_va, s_spawn_seq++);
    black::WF32(guest_base, spawner_va + 0x10u, pos.x);
    black::WF32(guest_base, spawner_va + 0x14u, pos.y);
    black::WF32(guest_base, spawner_va + 0x18u, pos.z);
    black::WF32(guest_base, spawner_va + 0x1Cu, 1.0f);

    out_slot = target_slot;
    out_name_va = name_va;
    out_seq_va = seq_va;
    out_arch = valid_arch;
    out_whash_lo = wh_lo;
    out_whash_hi = wh_hi;
    out_spawner_va = spawner_va;
    return true;
}

void CompleteRemoteActorSpawn(uint8_t* guest_base, uint8_t slot, uint32_t ai_controller_va) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (!guest_base || slot >= netcore::kMaxPlayers) return;
    if (ai_controller_va == 0u || !IsGuestCommitted(ai_controller_va, 0x90u)) {
        g_remote_dedicated_failed[slot] = true;
        LOGR("mp_bridge: dedicated spawn returned null/uncommitted for slot %u; using pool fallback",
             static_cast<unsigned>(slot));
        EnsureRemoteActorForSlot(guest_base, slot);
        return;
    }

    const uint32_t actor_va = black::R32(guest_base, ai_controller_va + black::kControlChar);
    if (!actor_va || !IsGuestCommitted(actor_va, 0x3C0u)) {
        g_remote_dedicated_failed[slot] = true;
        EnsureRemoteActorForSlot(guest_base, slot);
        return;
    }

    // Remove referência ao scratch de spawn (+0x31C) e zera flag de reciclagem do controlador.
    black::W32(guest_base, actor_va + 0x31Cu, 0u);
    if (IsGuestCommitted(ai_controller_va, 0x1FD4u)) {
        black::W8(guest_base, ai_controller_va + 0x1FD0u, 0u);
    }

    const bool is_coop =
        (g_role == Role::Host && g_host_session && g_host_session->rules().game_mode == netcore::GameMode::Coop) ||
        (g_role == Role::Client && g_client_session &&
         g_client_session->game_mode() == static_cast<uint8_t>(netcore::GameMode::Coop));
    black::W32(guest_base, actor_va + black::kCharTeam, is_coop ? 0u : 1u);

    g_remote_spawned_dedicated[slot] = true;
    g_remote_dedicated_failed[slot] = false;
    BindRemoteActor(slot, actor_va);
    if (g_has_applied_snapshot[slot]) {
        ApplySnapshotToActor(guest_base, actor_va, g_last_applied_snapshot[slot]);
    }
    LOGR("mp_bridge: spawned dedicated actor 0x%08X (ai_ctrl=0x%08X) for slot %u",
         actor_va, ai_controller_va, static_cast<unsigned>(slot));
}

bool ShouldSkipAiThink(uint32_t controller_va, const uint8_t* guest_base) {
    if (g_role == Role::OfflineCampaign || !guest_base || !controller_va) return false;
    // Em Territorial Assault (PvP por objetivos), neutraliza toda a IA da campanha para uma arena PvP limpa,
    // preservando os atores no pool para vínculo de jogadores remotos (Campanha Solo e Co-op permanecem intactos).
    const bool is_ta_pvp =
        (g_role == Role::Host && g_host_session &&
         g_host_session->rules().game_mode == netcore::GameMode::TerritorialAssault) ||
        (g_role == Role::Client && g_client_session &&
         g_client_session->game_mode() == static_cast<uint8_t>(netcore::GameMode::TerritorialAssault));
    if (is_ta_pvp) return true;
    const uint32_t actor_va = black::R32(guest_base, controller_va + black::kControlChar);
    return IsRemoteActor(actor_va);
}

void OnLocalPlayerDying(uint8_t* guest_base) {
    if (g_role == Role::OfflineCampaign || !black::IsInLevel(guest_base)) return;
    const uint32_t world = black::R32(guest_base, black::kWorld);
    if (!world) return;
    const uint32_t player = world + black::kWorldPlayer;

    const uint32_t char_ctrl = black::R32(guest_base, player + black::kCharControl);
    if (char_ctrl && char_ctrl != player + black::kCharDeathControl) {
        g_saved_player_controller = char_ctrl;
    } else if (g_saved_player_controller == 0) {
        g_saved_player_controller = world + black::kWorldController;
    }
    const uint32_t n0 = black::R32(guest_base, player + black::kCharName + 0);
    const uint32_t n1 = black::R32(guest_base, player + black::kCharName + 4);
    if (n0 != 0 || n1 != 0) {
        g_saved_player_name[0] = n0;
        g_saved_player_name[1] = n1;
    }
    g_local_weapon_va = 0;
    g_local_waiting_respawn = true;
}

bool ShouldSuppressMissionFailed(const uint8_t* guest_base) {
    if (g_role == Role::OfflineCampaign || !black::IsInLevel(guest_base)) return false;
    if (g_role == Role::Host && g_host_session) {
        return g_host_session->phase() == netcore::MatchPhase::Running;
    }
    if (g_role == Role::Client && g_client_session) {
        return g_client_session->phase() == netcore::MatchPhase::Running;
    }
    return false;
}

namespace {

uint8_t ResolvePlayerSlotByActor(const uint8_t* base, uint32_t actor_va) {
    if (!base || actor_va == 0) return netcore::kNoPlayer;
    if (actor_va == black::LocalPlayerAddr(base)) {
        if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
            return g_client_session->local_player_id();
        }
        return 0;
    }
    for (std::size_t i = 0; i < netcore::kMaxPlayers; ++i) {
        if (g_remote_actor_va[i] != 0 && g_remote_actor_va[i] == actor_va) {
            return static_cast<uint8_t>(i);
        }
    }
    return netcore::kNoPlayer;
}

}  // namespace

bool OnCharacterTakeBulletDamage(uint8_t* guest_base, uint32_t victim_va, uint32_t attacker_va, float raw_damage) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return false;

    const uint8_t victim_slot = ResolvePlayerSlotByActor(guest_base, victim_va);
    if (victim_slot == netcore::kNoPlayer) return false;

    const uint8_t attacker_slot = ResolvePlayerSlotByActor(guest_base, attacker_va);

    if (g_in_local_weapon_fire) {
        uint8_t local_slot = 0;
        if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
            local_slot = g_client_session->local_player_id();
        }
        if (attacker_slot == local_slot && attacker_slot != victim_slot &&
            std::isfinite(raw_damage) && raw_damage > 0.0f) {
            const bool host_can_apply =
                (g_role != Role::Host) ||
                (g_host_session && g_host_session->phase() == netcore::MatchPhase::Running &&
                 victim_slot < netcore::kMaxPlayers &&
                 (g_host_session->slots()[victim_slot].flags & netcore::kFlagAlive) != 0);
            if (host_can_apply) {
                g_local_shot_claimed_target = victim_slot;
                NotifyHitmarker(victim_slot, false);
            }
            static int s_claim_logs = 0;
            if (s_claim_logs++ < 15) {
                LOGR("mp_bridge: claimed local bullet hit attacker=%u victim=%u raw=%.1f applied=%d",
                     static_cast<unsigned>(attacker_slot), static_cast<unsigned>(victim_slot), raw_damage,
                     host_can_apply ? 1 : 0);
            }
            if (g_role == Role::Host && host_can_apply) {
                black::W8(guest_base, victim_va + black::kCharHit, 1u);
            }
        }
        return true;
    }

    if (g_role == Role::Client) {
        if (g_client_session && g_client_session->is_joined() &&
            attacker_slot == g_client_session->local_player_id() && attacker_slot != victim_slot &&
            std::isfinite(raw_damage) && raw_damage > 0.0f) {
            black::W8(guest_base, victim_va + black::kCharHit, 1u);
            NotifyHitmarker(victim_slot, false);
            const uint32_t tick = static_cast<uint32_t>(g_frame_count);
            const bool duplicate_local_shot =
                g_saw_weapon_fire_hook && tick <= g_last_local_fire_tick + 8u;
            netcore::Vec3 origin{};
            netcore::Vec3 dir{};
            const uint8_t wpn_id = ReadLocalEquippedCatalogWeapon(guest_base);
            if (!duplicate_local_shot && ReadLocalAimOriginAndDir(guest_base, origin, dir)) {
                const netcore::FireEvent ev{
                    .client_tick = tick,
                    .shooter = attacker_slot,
                    .weapon = wpn_id,
                    .origin = origin,
                    .direction = dir,
                    .claimed_target = victim_slot,
                };
                if (g_client_session->send_fire(ev)) {
                    g_last_local_fire_tick = tick;
                    g_prev_local_firing = true;
                }
            }
        }
        return true;
    }

    if (g_role == Role::Host && g_host_session) {
        if (attacker_slot != netcore::kNoPlayer && attacker_slot != victim_slot &&
            std::isfinite(raw_damage) && raw_damage > 0.0f) {
            const uint32_t tick = g_host_session->current_tick();
            const bool duplicate_local_shot =
                (attacker_slot == 0 && g_saw_weapon_fire_hook && tick <= g_last_local_fire_tick + 8u);
            if (!duplicate_local_shot) {
                const uint8_t wpn_id = (attacker_slot < netcore::kMaxPlayers) ? g_host_session->slots()[attacker_slot].weapon : 0;
                const auto* wspec = g_host_session->rules().find_weapon(wpn_id);
                const float scale = (g_local_max_health > 1.0f) ? (100.0f / g_local_max_health) : 1.0f;
                const int fallback_dmg = static_cast<int>(std::lround(raw_damage * scale));
                const uint16_t dmg = static_cast<uint16_t>(
                    std::clamp<int>(wspec ? static_cast<int>(wspec->damage) : fallback_dmg, 1, 100));
                if (g_host_session->apply_damage(attacker_slot, victim_slot, dmg, wpn_id)) {
                    black::W8(guest_base, victim_va + black::kCharHit, 1u);
                    const uint8_t hp = g_host_session->slots()[victim_slot].health;
                    if (attacker_slot == 0) {
                        NotifyHitmarker(victim_slot, hp == 0);
                    }
                    static int s_dmg_logs = 0;
                    if (s_dmg_logs++ < 30) {
                        LOGR("mp_bridge: host bullet dmg attacker=%u victim=%u wpn=%u dmg=%u hp_after=%u",
                             static_cast<unsigned>(attacker_slot), static_cast<unsigned>(victim_slot),
                             static_cast<unsigned>(wpn_id), static_cast<unsigned>(dmg), static_cast<unsigned>(hp));
                    }
                    if (victim_slot == 0) {
                        const float guest_hp = (static_cast<float>(hp) / 100.0f) * g_local_max_health;
                        black::WF32(guest_base, victim_va + black::kCharHealth, guest_hp);
                        if (hp == 0) {
                            OnLocalPlayerDying(guest_base);
                            black::W32(guest_base, victim_va + black::kCharState, 2u);
                        }
                    }
                }
            }
        }
        return true;
    }
    return false;
}

bool OnCharacterTakeBlastDamage(uint8_t* guest_base, uint32_t victim_va, float raw_damage, uint32_t blast_info_va) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return false;

    const uint8_t victim_slot = ResolvePlayerSlotByActor(guest_base, victim_va);
    if (victim_slot == netcore::kNoPlayer) return false;

    uint8_t local_slot = 0;
    if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        local_slot = g_client_session->local_player_id();
    }

    netcore::Vec3 origin{0.0f, 0.0f, 0.0f};
    uint32_t raw_attacker_va = 0;
    if (blast_info_va && IsGuestCommitted(blast_info_va, 0x18u)) {
        origin.x = black::RF32(guest_base, blast_info_va + 0u);
        origin.y = black::RF32(guest_base, blast_info_va + 4u);
        origin.z = black::RF32(guest_base, blast_info_va + 8u);
        raw_attacker_va = black::R32(guest_base, blast_info_va + 0x14u);
    } else if (IsGuestCommitted(victim_va, 0xB0u)) {
        origin.x = black::RF32(guest_base, victim_va + black::kCharPos + 0u);
        origin.y = black::RF32(guest_base, victim_va + black::kCharPos + 4u);
        origin.z = black::RF32(guest_base, victim_va + black::kCharPos + 8u);
    }
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
        return true;
    }

    const uint8_t resolved_atk = ResolvePlayerSlotByActor(guest_base, raw_attacker_va);
    const uint8_t eq_weapon = ReadLocalEquippedCatalogWeapon(guest_base);
    const uint8_t blast_weapon = (eq_weapon == 14u || eq_weapon == 15u) ? eq_weapon : netcore::kNoWeapon;

    uint8_t attacker_slot = resolved_atk;
    if (attacker_slot == netcore::kNoPlayer && victim_slot != local_slot) {
        attacker_slot = local_slot;
    }

    const float dmg = (std::isfinite(raw_damage) && raw_damage > 0.0f) ? std::min(raw_damage, 200.0f) : 0.0f;
    if (dmg >= 1.0f) {
        netcore::BlastEvent ev{};
        ev.attacker = attacker_slot;
        ev.target = victim_slot;
        ev.weapon = (attacker_slot == local_slot) ? blast_weapon : netcore::kNoWeapon;
        ev.flags = netcore::kBlastFlagClaim;
        ev.origin = origin;
        ev.damage = dmg;
        if (g_role == Role::Host && g_host_session) {
            ev.client_tick = g_host_session->current_tick();
            if (g_host_session->local_blast(ev) && attacker_slot == local_slot && victim_slot != local_slot) {
                NotifyHitmarker(victim_slot, false);
            }
        } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
            ev.client_tick = g_client_session->latest_snapshot().tick;
            if (g_client_session->send_blast(ev) && attacker_slot == local_slot && victim_slot != local_slot) {
                NotifyHitmarker(victim_slot, false);
            }
        }
    }
    return true;
}

bool PrepareRemoteActorTransform(uint8_t* guest_base, uint8_t slot, uint32_t& out_actor_va, uint32_t& out_pos_vec_va) {
    out_actor_va = 0;
    out_pos_vec_va = 0;
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return false;
    if (slot >= netcore::kMaxPlayers || !g_has_applied_snapshot[slot]) return false;

    const uint32_t actor_va = g_remote_actor_va[slot];
    if (!IsGuestCommitted(actor_va, 0x3C0)) return false;

    if (g_guest_scratch_va == 0) {
        g_guest_scratch_va = mem::AllocSystem(64, 16);
    }
    if (!IsGuestCommitted(g_guest_scratch_va, 16)) return false;

    const auto& pos = g_last_applied_snapshot[slot].position;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return false;

    black::WF32(guest_base, g_guest_scratch_va + 0, pos.x);
    black::WF32(guest_base, g_guest_scratch_va + 4, pos.y);
    black::WF32(guest_base, g_guest_scratch_va + 8, pos.z);
    black::WF32(guest_base, g_guest_scratch_va + 12, 1.0f);

    out_actor_va = actor_va;
    out_pos_vec_va = g_guest_scratch_va;
    return true;
}

bool OnAnimSelectPre(uint8_t* guest_base, uint32_t anim_va, float& out_saved_speed, uint8_t& out_saved_move, float& out_saved_ctrl_speed) {
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return false;
    if (!IsGuestCommitted(anim_va, 4)) return false;
    const uint32_t actor_va = black::R32(guest_base, anim_va);
    if (!IsGuestCommitted(actor_va, 0x3C0)) return false;

    const uint8_t slot = FindRemoteSlotByActor(actor_va);
    if (slot == netcore::kNoPlayer || !g_has_applied_snapshot[slot]) return false;

    const auto& sp = g_last_applied_snapshot[slot];
    const bool alive = (sp.flags & netcore::kFlagAlive) != 0;
    const float speed = alive ? std::sqrt(sp.velocity.x * sp.velocity.x + sp.velocity.z * sp.velocity.z) : 0.0f;

    out_saved_speed = black::RF32(guest_base, actor_va + black::kCharMeasuredSpeed);
    black::WF32(guest_base, actor_va + black::kCharMeasuredSpeed, speed);

    const uint32_t ctrl = black::R32(guest_base, actor_va + black::kCharControl);
    if (IsGuestCommitted(ctrl, 0x90)) {
        out_saved_ctrl_speed = black::RF32(guest_base, ctrl + black::kControlSpeed);
        out_saved_move = black::R8(guest_base, ctrl + black::kControlLocomotion);
        black::WF32(guest_base, ctrl + black::kControlSpeed, speed);
        black::W8(guest_base, ctrl + black::kControlLocomotion, speed > 0.05f ? 1u : 0u);
    } else {
        out_saved_ctrl_speed = 0.0f;
        out_saved_move = 0u;
    }
    return true;
}

void OnAnimSelectPost(uint8_t* guest_base, uint32_t anim_va, float saved_speed, uint8_t saved_move, float saved_ctrl_speed) {
    if (!guest_base || !IsGuestCommitted(anim_va, 4)) return;
    const uint32_t actor_va = black::R32(guest_base, anim_va);
    if (!IsGuestCommitted(actor_va, 0x3C0)) return;
    black::WF32(guest_base, actor_va + black::kCharMeasuredSpeed, saved_speed);

    const uint32_t ctrl = black::R32(guest_base, actor_va + black::kCharControl);
    if (IsGuestCommitted(ctrl, 0x90)) {
        black::WF32(guest_base, ctrl + black::kControlSpeed, saved_ctrl_speed);
        black::W8(guest_base, ctrl + black::kControlLocomotion, saved_move);
    }
}

bool ShouldProtectActorFromCrush(const uint8_t* guest_base, uint32_t actor_va) {
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return false;
    return ResolvePlayerSlotByActor(guest_base, actor_va) != netcore::kNoPlayer;
}

namespace {

bool IsValidWeaponPtrForSfx(const uint8_t* base, uint32_t weapon_va) {
    if (!IsGuestCommitted(weapon_va, 0x20)) return false;
    const uint32_t handler_va = black::R32(base, weapon_va + 0x04u);
    if (!IsGuestCommitted(handler_va, 0x110)) return false;
    const uint32_t wdef_va = black::R32(base, weapon_va + 0x0Cu);
    if (!IsGuestCommitted(wdef_va, 0x28)) return false;
    const uint32_t sig_va = black::R32(base, handler_va + 0xE8u);
    const uint32_t owner_char_va = black::R32(base, handler_va + 0xF0u);
    if (!IsGuestCommitted(sig_va, 0x14) || !IsGuestCommitted(owner_char_va, 0xB0)) return false;
    if (black::R8(base, handler_va + 0x108u) == 0) {
        const uint32_t sfx_desc_va = black::R32(base, sig_va + 0x10u);
        if (!IsGuestCommitted(sfx_desc_va, 0x10)) return false;
    }
    return true;
}

}  // namespace

bool OnLocalWeaponFirePre(uint8_t* guest_base, uint32_t weapon_va) {
    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return false;
    if (!IsGuestCommitted(weapon_va, 0x20)) {
        static int s_bad_weapon = 0;
        if (s_bad_weapon++ < 5) {
            LOGI("mp_bridge: sub_001DBD50 bad weapon_va=0x%08X", weapon_va);
        }
        return false;
    }

    const uint32_t handler_va = black::R32(guest_base, weapon_va + 0x04u);
    if (!IsGuestCommitted(handler_va, 0xF4)) {
        static int s_bad_handler = 0;
        if (s_bad_handler++ < 5) {
            LOGI("mp_bridge: sub_001DBD50 bad handler_va=0x%08X (weapon=0x%08X)", handler_va, weapon_va);
        }
        return false;
    }

    const uint32_t owner_char_va = black::R32(guest_base, handler_va + 0xF0u);
    const uint32_t local_player = black::LocalPlayerAddr(guest_base);
    if (!owner_char_va || owner_char_va != local_player) {
        static int s_mismatch = 0;
        if (s_mismatch++ < 10) {
            LOGI("mp_bridge: sub_001DBD50 non-local weapon=0x%08X handler=0x%08X owner=0x%08X local=0x%08X",
                 weapon_va, handler_va, owner_char_va, local_player);
        }
        return false;
    }

    static int s_local_hit = 0;
    if (s_local_hit++ < 10) {
        LOGR("mp_bridge: local sub_001DBD50 weapon=0x%08X handler=0x%08X", weapon_va, handler_va);
    }

    g_saw_weapon_fire_hook = true;
    g_in_local_weapon_fire = true;
    g_local_shot_claimed_target = netcore::kNoPlayer;
    g_local_weapon_va = weapon_va;
    return true;
}

void OnLocalWeaponFirePost(uint8_t* guest_base, uint32_t weapon_va) {
    (void)weapon_va;
    g_in_local_weapon_fire = false;
    const uint8_t claimed = g_local_shot_claimed_target;
    g_local_shot_claimed_target = netcore::kNoPlayer;

    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) return;
    const uint32_t tick =
        g_host_session ? g_host_session->current_tick() : static_cast<uint32_t>(g_frame_count);
    const uint32_t prev_tick = g_last_local_fire_tick;
    const uint8_t wpn_id = ReadLocalEquippedCatalogWeapon(guest_base);
    EmitLocalFireEvent(guest_base, tick, wpn_id, claimed);
    static int s_post_log = 0;
    if (s_post_log++ < 10) {
        const unsigned phase = g_host_session ? static_cast<unsigned>(g_host_session->phase()) : 99u;
        const unsigned alive = g_host_session ? ((g_host_session->slots()[0].flags & netcore::kFlagAlive) ? 1u : 0u) : 1u;
        LOGR("mp_bridge: local fire post tick=%u accepted=%d prev_tick=%u after_tick=%u claimed=%u wpn=%u phase=%u alive=%u",
             tick, (g_last_local_fire_tick == tick) ? 1 : 0, prev_tick, g_last_local_fire_tick,
             static_cast<unsigned>(claimed), static_cast<unsigned>(wpn_id), phase, alive);
    }
}

bool PrepareRemoteFireRender(uint8_t* guest_base, uint32_t& out_tracer_mgr_va,
                             uint32_t& out_start_vec_va, uint32_t& out_end_vec_va,
                             uint32_t& out_weapon_va) {
    out_tracer_mgr_va = 0;
    out_start_vec_va = 0;
    out_end_vec_va = 0;
    out_weapon_va = 0;

    if (g_role == Role::OfflineCampaign || !guest_base || !black::IsInLevel(guest_base)) {
        g_pending_remote_fires.clear();
        return false;
    }
    if (g_pending_remote_fires.empty()) return false;

    if (g_guest_scratch_va == 0) {
        g_guest_scratch_va = mem::AllocSystem(64, 16);
    }
    if (!IsGuestCommitted(g_guest_scratch_va, 48)) {
        static bool s_bad_scratch = false;
        if (!s_bad_scratch) {
            s_bad_scratch = true;
            LOGI("mp_bridge: remote fire scratch uncommitted va=0x%08X", g_guest_scratch_va);
        }
        g_pending_remote_fires.clear();
        return false;
    }

    const uint32_t effects_base = black::R32(guest_base, black::kLevel);
    if (!IsGuestCommitted(effects_base, black::kTracerMgrOffset + 4)) {
        static bool s_bad_effects = false;
        if (!s_bad_effects) {
            s_bad_effects = true;
            LOGI("mp_bridge: remote fire effects_base uncommitted va=0x%08X", effects_base);
        }
        g_pending_remote_fires.clear();
        return false;
    }
    const uint32_t tracer_mgr_va = effects_base + black::kTracerMgrOffset;

    while (!g_pending_remote_fires.empty()) {
        const netcore::FireEvent ev = g_pending_remote_fires.front();
        g_pending_remote_fires.erase(g_pending_remote_fires.begin());

        if (ev.shooter >= netcore::kMaxPlayers) continue;
        if (!std::isfinite(ev.origin.x) || !std::isfinite(ev.origin.y) || !std::isfinite(ev.origin.z)) continue;
        if (!std::isfinite(ev.direction.x) || !std::isfinite(ev.direction.y) || !std::isfinite(ev.direction.z)) continue;

        const float len2 = ev.direction.x * ev.direction.x +
                           ev.direction.y * ev.direction.y +
                           ev.direction.z * ev.direction.z;
        if (len2 < 1e-4f) continue;
        const float inv_len = 1.0f / std::sqrt(len2);
        const float dx = ev.direction.x * inv_len;
        const float dy = ev.direction.y * inv_len;
        const float dz = ev.direction.z * inv_len;

        const uint32_t start_va = g_guest_scratch_va + 16;
        const uint32_t end_va = g_guest_scratch_va + 32;
        const float sx = ev.origin.x + dx * 0.6f;
        const float sy = ev.origin.y + 1.4f + dy * 0.6f;
        const float sz = ev.origin.z + dz * 0.6f;

        black::WF32(guest_base, start_va + 0, sx);
        black::WF32(guest_base, start_va + 4, sy);
        black::WF32(guest_base, start_va + 8, sz);
        black::WF32(guest_base, start_va + 12, 1.0f);

        black::WF32(guest_base, end_va + 0, sx + dx * 45.0f);
        black::WF32(guest_base, end_va + 4, sy + dy * 45.0f);
        black::WF32(guest_base, end_va + 8, sz + dz * 45.0f);
        black::WF32(guest_base, end_va + 12, 1.0f);

        uint32_t weapon_va = 0;
        const uint32_t actor_va = g_remote_actor_va[ev.shooter];
        if (IsGuestCommitted(actor_va, 0x3C0)) {
            const uint32_t handler_va = black::R32(guest_base, actor_va + black::kCharWeapon);
            if (IsGuestCommitted(handler_va, 0xF8)) {
                const uint32_t cand_wpn = black::R32(guest_base, handler_va + 0xF4u);
                if (IsValidWeaponPtrForSfx(guest_base, cand_wpn) &&
                    black::R32(guest_base, cand_wpn + 0x04u) == handler_va) {
                    weapon_va = cand_wpn;
                }
            }
        }
        if (weapon_va == 0 && IsValidWeaponPtrForSfx(guest_base, g_local_weapon_va)) {
            const uint32_t h = black::R32(guest_base, g_local_weapon_va + 0x04u);
            if (black::R32(guest_base, h + 0xF0u) == black::LocalPlayerAddr(guest_base)) {
                weapon_va = g_local_weapon_va;
            }
        }

        static uint32_t s_remote_fire_logs = 0;
        if (s_remote_fire_logs < 10) {
            ++s_remote_fire_logs;
            LOGR("mp_bridge: render remote fire shooter=%u start=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) claimed=%u wpn=0x%08X",
                 static_cast<unsigned>(ev.shooter), sx, sy, sz, dx, dy, dz,
                 static_cast<unsigned>(ev.claimed_target), weapon_va);
        }

        out_tracer_mgr_va = tracer_mgr_va;
        out_start_vec_va = start_va;
        out_end_vec_va = end_va;
        out_weapon_va = weapon_va;
        return true;
    }

    return false;
}

Role CurrentRole() {
    return g_role;
}

void SetRole(Role r) {
    std::lock_guard<std::recursive_mutex> lk(g_bridge_mu);
    if (r == Role::OfflineCampaign) {
        StopMultiplayer();
        return;
    }
    g_role = r;
    if (g_role == Role::Host && !g_host_session) {
        g_local_endpoint = std::make_unique<netcore::LoopbackEndpoint>();
        g_host_session = std::make_unique<netcore::HostSession>(
            *g_local_endpoint, netcore::RulesConfig::default_deathmatch(), "Host");
        g_host_session->start_match();
    }
}

uint64_t FrameCount() {
    return g_frame_count;
}

}  // namespace mp
