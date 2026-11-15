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

#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/black_addrs.h"
#include "mp/mp_udp.h"
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

// Dados salvos do jogador local para restauração no respawn sem recarregar a fase (Testes #7 e #8 da Fase 2).
uint32_t g_saved_player_controller = 0;
uint32_t g_saved_player_name[2] = {};
bool g_local_waiting_respawn = false;
netcore::Vec3 g_last_local_pos{};
bool g_has_last_local_pos = false;

std::unique_ptr<netcore::LoopbackEndpoint> g_local_endpoint;
std::unique_ptr<UdpTransport> g_udp_transport;
std::unique_ptr<netcore::HostSession> g_host_session;
std::unique_ptr<netcore::ClientSession> g_client_session;

float clamp_angle_rad(float rad) {
    while (rad > 3.14159265f) rad -= 6.28318530f;
    while (rad < -3.14159265f) rad += 6.28318530f;
    return rad;
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

    netcore::Vec3 vel{};
    if (g_has_last_local_pos) {
        vel.x = (x - g_last_local_pos.x) * 30.0f;
        vel.y = (y - g_last_local_pos.y) * 30.0f;
        vel.z = (z - g_last_local_pos.z) * 30.0f;
    }
    g_last_local_pos = netcore::Vec3{x, y, z};
    g_has_last_local_pos = true;

    const bool alive = (black::R32(base, player + black::kCharState) == 0);
    const bool crouched = (black::R8(base, player + black::kCharCrouch) != 0);

    out = netcore::PlayerState{
        .sequence = 0,
        .client_tick = tick,
        .position = netcore::Vec3{x, y, z},
        .yaw = yaw_rad,
        .pitch = pitch_rad,
        .velocity = vel,
        .anim_state = static_cast<uint8_t>(crouched ? 1 : 0),
        .weapon = 0,
        .flags = static_cast<uint8_t>((alive ? netcore::kFlagAlive : 0) | (crouched ? netcore::kFlagCrouched : 0)),
    };
    return true;
}

// Aplica o SnapshotPlayer recebido da rede ao ator convidado correspondente (Testes #1, #2 e #3 da Fase 2).
void ApplySnapshotToActor(uint8_t* base, uint32_t actor_va, const netcore::SnapshotPlayer& sp) {
    if (!base || !actor_va) return;

    // Marca como cinemático (+0x3A7 = 1) para a física local não sobrescrever a posição replicada
    black::W8(base, actor_va + black::kCharKinematic, 1);

    // Posição dos pés (+0xA0)
    black::WF32(base, actor_va + black::kCharPos + 0, sp.position.x);
    black::WF32(base, actor_va + black::kCharPos + 4, sp.position.y);
    black::WF32(base, actor_va + black::kCharPos + 8, sp.position.z);

    // Vida (+0x2F0) e estado de vida (+0x384)
    black::WF32(base, actor_va + black::kCharHealth, static_cast<float>(sp.health));
    const bool alive = (sp.flags & netcore::kFlagAlive) != 0;
    black::W32(base, actor_va + black::kCharState, alive ? 0u : 2u);

    const bool crouched = (sp.flags & netcore::kFlagCrouched) != 0;
    black::W8(base, actor_va + black::kCharCrouch, crouched ? 1u : 0u);

    // Controlador de IA (+0x324): alimenta ângulos em graus e vetores de locomoção
    const uint32_t ctrl = black::R32(base, actor_va + black::kCharControl);
    if (ctrl) {
        const float yaw_deg = sp.yaw * kRadToDeg;
        const float pitch_deg = sp.pitch * kRadToDeg;
        black::WF32(base, ctrl + black::kControlAngles + 0, yaw_deg);    // yaw
        black::WF32(base, ctrl + black::kControlAngles + 4, yaw_deg);    // torso yaw
        black::WF32(base, ctrl + black::kControlAngles + 8, yaw_deg);    // aim yaw
        black::WF32(base, ctrl + black::kControlAngles + 12, pitch_deg); // pitch

        const float speed =
            std::sqrt(sp.velocity.x * sp.velocity.x + sp.velocity.z * sp.velocity.z);
        black::WF32(base, ctrl + black::kControlSpeed, speed);
        black::W8(base, ctrl + black::kControlCrouch, crouched ? 1u : 0u);

        if (speed > 0.05f) {
            const float inv = 1.0f / speed;
            const float dx = sp.velocity.x * inv;
            const float dz = sp.velocity.z * inv;
            black::WF32(base, ctrl + black::kControlMove + 0, dx);
            black::WF32(base, ctrl + black::kControlMove + 4, 0.0f);
            black::WF32(base, ctrl + black::kControlMove + 8, dz);
            black::WF32(base, ctrl + black::kControlMoveDir + 0, dx);
            black::WF32(base, ctrl + black::kControlMoveDir + 4, 0.0f);
            black::WF32(base, ctrl + black::kControlMoveDir + 8, dz);
            black::WF32(base, ctrl + black::kControlRate, 1.0f);
        } else {
            black::WF32(base, ctrl + black::kControlRate, 0.0f);
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
    black::WF32(base, player + black::kCharHealth, 100.0f);

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
    g_local_waiting_respawn = false;
    LOGI("mp_bridge: local player respawned at (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
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
    uint16_t port = kDefaultUdpPort;
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

    LOGI("mp_bridge: initialized (role=%u, protocol_v=%u)", static_cast<unsigned>(g_role),
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
}

bool StartClientUdp(const std::string& remote_host, uint16_t port, const std::string& player_name) {
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
}

void StopMultiplayer() {
    g_role = Role::OfflineCampaign;
    g_client_session.reset();
    g_host_session.reset();
    g_udp_transport.reset();
    g_local_endpoint.reset();
    g_remote_actor_va.fill(0);
    g_saved_player_controller = 0;
    g_saved_player_name[0] = 0;
    g_saved_player_name[1] = 0;
    g_local_waiting_respawn = false;
    g_has_last_local_pos = false;
}

bool IsClientJoined() {
    return g_client_session && g_client_session->is_joined();
}

void OnImageLoaded(uint8_t* guest_base) {
    g_guest_base = guest_base;
    LOGI("mp_bridge: guest image mapped at %p", static_cast<void*>(guest_base));
}

void OnPreFrame(uint8_t* guest_base) {
    g_guest_base = guest_base;
    if (g_role == Role::OfflineCampaign) return;

    if (g_role == Role::Host && g_host_session) {
        g_host_session->tick();
        const auto& slots = g_host_session->slots();
        // Respawn do host local (slot 0) caso tenha acabado de reviver no HostSession
        if (g_local_waiting_respawn && (slots[0].flags & netcore::kFlagAlive) != 0) {
            RestoreAndTeleportLocalPlayer(guest_base, slots[0].position, slots[0].yaw);
        }
        // Aplica estado replicado aos atores remotos (slots 1..3)
        for (std::size_t i = 1; i < netcore::kMaxPlayers; ++i) {
            if (!slots[i].occupied || g_remote_actor_va[i] == 0) continue;
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
    } else if (g_role == Role::Client && g_client_session) {
        g_client_session->poll();
        for (const auto& resp : g_client_session->recent_respawns()) {
            if (resp.player == g_client_session->local_player_id()) {
                RestoreAndTeleportLocalPlayer(guest_base, resp.position, resp.yaw);
            }
        }
        g_client_session->clear_events();

        const auto& snap = g_client_session->latest_snapshot();
        for (const auto& sp : snap.players) {
            if (sp.player_id == g_client_session->local_player_id()) continue;
            if (sp.player_id < netcore::kMaxPlayers && g_remote_actor_va[sp.player_id] != 0) {
                ApplySnapshotToActor(guest_base, g_remote_actor_va[sp.player_id], sp);
            }
        }
    }

    PreventSamePointCollisionCrash(guest_base);
}

void OnPostFrame(uint8_t* guest_base) {
    ++g_frame_count;
    if (g_frame_count == 1) {
        LOGI("mp_bridge: first guest frame hook fired (sub_0020F6A0)");
    }
    if (g_role == Role::OfflineCampaign) return;

    netcore::PlayerState local_state{};
    const uint32_t tick = g_host_session ? g_host_session->current_tick() : static_cast<uint32_t>(g_frame_count);
    if (!ReadLocalPlayerFromGuest(guest_base, tick, local_state)) return;

    if (g_role == Role::Host && g_host_session) {
        g_host_session->set_local_player_state(local_state);
    } else if (g_role == Role::Client && g_client_session && g_client_session->is_joined()) {
        g_client_session->send_local_state(local_state);
    }
}

void BindRemoteActor(uint8_t player_id, uint32_t actor_va) {
    if (player_id < netcore::kMaxPlayers) {
        g_remote_actor_va[player_id] = actor_va;
        LOGI("mp_bridge: bound player_id=%u to guest actor 0x%08X", static_cast<unsigned>(player_id), actor_va);
    }
}

void UnbindRemoteActor(uint8_t player_id) {
    if (player_id < netcore::kMaxPlayers) {
        g_remote_actor_va[player_id] = 0;
    }
}

bool IsRemoteActor(uint32_t actor_va) {
    if (actor_va == 0) return false;
    for (uint32_t va : g_remote_actor_va) {
        if (va == actor_va) return true;
    }
    return false;
}

bool ShouldSkipAiThink(uint32_t controller_va, const uint8_t* guest_base) {
    if (g_role == Role::OfflineCampaign || !guest_base || !controller_va) return false;
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

Role CurrentRole() {
    return g_role;
}

void SetRole(Role r) {
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
