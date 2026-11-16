#include "app/app.h"

#include <emscripten.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/pc_input.h"
#include "core/diag.h"
#include "core/log.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "mp/mp_bridge.h"
#include "mp/mp_webrtc.h"
#include "netcore/transport.hpp"
#include "web/web_stats.h"

namespace app {

namespace {

Settings g_settings{
    .display_mode = DisplayMode::Borderless,
    .width = 1280,
    .height = 720,
    .render_percent = 100,
    .fps_cap = 60,
    .vsync = true,
};

std::recursive_mutex g_ui_lock;
std::atomic<uint32_t> g_canvas_w{1280};
std::atomic<uint32_t> g_canvas_h{720};
std::atomic<bool> g_has_webgpu{false};
std::string g_exe_dir = "/black_assets";
std::mutex g_status_mu;
std::string g_boot_message = "Aguardando assets em /black_assets";

void SetBootStatus(int32_t state, std::string msg) {
    {
        std::lock_guard<std::mutex> lock(g_status_mu);
        g_boot_message = std::move(msg);
    }
    web_stats::g_boot_state.store(state, std::memory_order_release);
}

bool LoadFileBytes(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto sz = in.tellg();
    if (sz <= 0) return false;
    out.resize(static_cast<size_t>(sz));
    in.seekg(0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), sz));
}

std::string LowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool FindXbeOrIso(const std::filesystem::path& root, std::vector<uint8_t>& out_xbe, std::filesystem::path& out_game_dir,
                  std::string& out_err) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        out_err = "Diretorio de assets ausente: " + root.string();
        return false;
    }

    const auto manifest_path = root / ".vfs_manifest.tsv";
    if (std::filesystem::exists(manifest_path, ec)) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string err;
        if (!disc->Open(manifest_path, err)) {
            out_err = "Falha ao abrir VFS manifest: " + err;
            return false;
        }
        if (!disc->Load("default.xbe", out_xbe)) {
            out_err = "VFS manifest sem default.xbe";
            return false;
        }
        kernel::MountDisc(std::move(disc));
        out_game_dir = root;
        return true;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = LowerStr(entry.path().filename().string());
        if (name == "default.xbe") {
            if (LoadFileBytes(entry.path(), out_xbe)) {
                out_game_dir = root;
                return true;
            }
        }
    }

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = LowerStr(entry.path().filename().string());
        if (name.size() > 4 && name.substr(name.size() - 4) == ".iso") {
            auto disc = std::make_unique<kernel::Disc>();
            std::string err;
            if (!disc->Open(entry.path(), err)) {
                out_err = "Falha ao abrir ISO: " + err;
                return false;
            }
            if (!disc->Load("default.xbe", out_xbe)) {
                out_err = "ISO sem default.xbe";
                return false;
            }
            kernel::MountDisc(std::move(disc));
            out_game_dir = root;
            return true;
        }
    }

    out_err = "Nenhum default.xbe ou .iso encontrado em " + root.string();
    return false;
}

struct BootArgs {
    std::string data_dir;
};

void* BootWorkerMain(void* raw) {
    std::unique_ptr<BootArgs> args(static_cast<BootArgs*>(raw));
    const std::string root = args->data_dir.empty() ? "/black_assets" : args->data_dir;
    g_exe_dir = root;

    std::vector<uint8_t> xbe;
    std::filesystem::path game_dir;
    std::string err;
    if (!FindXbeOrIso(root, xbe, game_dir, err)) {
        LOGE("web_main: %s", err.c_str());
        SetBootStatus(-1, err);
        return nullptr;
    }

    const std::filesystem::path save_dir = std::filesystem::path(root) / "_save";
    std::error_code ec;
    std::filesystem::create_directories(save_dir, ec);

    SetBootStatus(1, "Inicializando kernel Xbox e recompilado XBE...");
    if (!kernel::Boot(std::move(xbe), "default.xbe", game_dir.string(), save_dir.string())) {
        SetBootStatus(-1, "kernel::Boot falhou ao validar ou carregar default.xbe");
        return nullptr;
    }

    SetBootStatus(2, "default.xbe em execucao (WebGPU + AudioWorklet)");
    return nullptr;
}

void MainLoopTick() {
    // Kept intentionally lightweight so Emscripten pthreads can proxy MAIN_THREAD_EM_ASM smoothly.
}

}  // namespace

Settings& Get() { return g_settings; }
void Load() {}
void Save() {}
void LogSettings() {}
void LogSystem() {}
void ApplyDisplay() {}

Mode Desktop() {
    return {g_canvas_w.load(std::memory_order_relaxed), g_canvas_h.load(std::memory_order_relaxed)};
}
Mode Output() { return Desktop(); }

float RenderScale(uint32_t guest_height) {
    const uint32_t out_h = g_canvas_h.load(std::memory_order_relaxed);
    const uint32_t pct = std::clamp<uint32_t>(g_settings.render_percent, 25u, 200u);
    const float target_h = float(out_h) * (float(pct) / 100.0f);
    const float gh = float(guest_height ? guest_height : 480u);
    return std::max(1.0f, target_h / gh);
}

bool Widescreen() {
    const uint32_t w = g_canvas_w.load(std::memory_order_relaxed);
    const uint32_t h = std::max(1u, g_canvas_h.load(std::memory_order_relaxed));
    return uint64_t(w) * 2ull >= uint64_t(h) * 3ull;
}

std::filesystem::path ExeDir() { return std::filesystem::path(g_exe_dir); }

bool FindGame(GameFiles&) { return false; }
std::string Sha1(const std::vector<uint8_t>&) { return {}; }
bool PrepareGameCode(const GameFiles&) { return true; }
int Instance() { return 0; }
void* Window() { return nullptr; }
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}
std::recursive_mutex& UiLock() { return g_ui_lock; }

void Pace() {
    static auto prev = std::chrono::steady_clock::now();
    const uint32_t cap = g_settings.fps_cap;
    if (cap > 1) {
        const auto frame_dur = std::chrono::microseconds(1000000ull / cap);
        const auto now = std::chrono::steady_clock::now();
        if (now < prev + frame_dur) {
            std::this_thread::sleep_until(prev + frame_dur);
        }
        prev = std::chrono::steady_clock::now();
    } else {
        std::this_thread::yield();
    }
}

}  // namespace app

int main() {
    logx::Init("/tmp", app::kGame.log ? app::kGame.log : "black_web");
    if (app::kGame.init) app::kGame.init();
    LOGI("Black Enhanced WebAssembly runtime initialized");
    emscripten_set_main_loop(app::MainLoopTick, 0, false);
    return 0;
}

extern "C" {

void BlackWebNotifyCanvasReady(uint32_t width, uint32_t height, uint32_t has_webgpu) {
    if (width >= 160 && height >= 120) {
        app::g_canvas_w.store(width, std::memory_order_relaxed);
        app::g_canvas_h.store(height, std::memory_order_relaxed);
        app::g_settings.width = width;
        app::g_settings.height = height;
    }
    app::g_has_webgpu.store(has_webgpu != 0, std::memory_order_release);
}

int32_t BlackWebRequestBoot(const char* data_dir) {
    const int32_t cur = web_stats::g_boot_state.load(std::memory_order_acquire);
    if (cur == 1 || cur == 2) return 0;
    if (!app::g_has_webgpu.load(std::memory_order_acquire)) {
        app::SetBootStatus(-1, "WebGPU indisponivel no navegador. Use Chrome/Edge com WebGPU ativo.");
        return -1;
    }

    web_stats::g_boot_state.store(1, std::memory_order_release);
    app::SetBootStatus(1, "Montando assets no thread do jogo...");

    auto* args = new app::BootArgs{data_dir ? std::string(data_dir) : std::string("/black_assets")};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4u << 20);
    pthread_t tid{};
    const int rc = pthread_create(&tid, &attr, &app::BootWorkerMain, args);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete args;
        app::SetBootStatus(-1, "Falha ao criar thread de boot WASM");
        return -1;
    }
    return 1;
}

int32_t BlackWebSetRole(uint32_t role, const char* player_name, uint32_t host_peer) {
    mp::StopMultiplayer();
    const std::string name = (player_name && player_name[0]) ? std::string(player_name) : std::string("Player");
    if (role == 1) {
        return mp::StartHostWebRtc(name, true, false, 8000u) ? 1 : 0;
    }
    if (role == 2) {
        return mp::StartClientWebRtc(name, host_peer ? host_peer : 1u, true, false, 8000u) ? 1 : 0;
    }
    mp::SetRole(mp::Role::OfflineCampaign);
    return 1;
}

int32_t BlackWebSetRoleEx(uint32_t role, const char* player_name, uint32_t host_peer, uint32_t cs_loadout,
                          uint32_t fixed_budget, uint32_t budget_cap) {
    mp::StopMultiplayer();
    const std::string name = (player_name && player_name[0]) ? std::string(player_name) : std::string("Player");
    const uint16_t cap = static_cast<uint16_t>(std::clamp<uint32_t>(budget_cap, 450u, 30000u));
    if (role == 1) {
        return mp::StartHostWebRtc(name, cs_loadout != 0, fixed_budget != 0, cap) ? 1 : 0;
    }
    if (role == 2) {
        return mp::StartClientWebRtc(name, host_peer ? host_peer : 1u, cs_loadout != 0, fixed_budget != 0, cap) ? 1 : 0;
    }
    mp::SetRole(mp::Role::OfflineCampaign);
    return 1;
}

void BlackWebSetEnhancedConfig(uint32_t mouse_direct, float mouse_sens, float ads_mult, uint32_t invert_y,
                               float fov_deg, uint32_t hitmarker) {
    common::EnhancedConfig cfg{};
    cfg.mouse_direct = (mouse_direct != 0);
    cfg.mouse_sens = mouse_sens;
    cfg.ads_sens_mult = ads_mult;
    cfg.invert_y = (invert_y != 0);
    cfg.fov_deg = fov_deg;
    cfg.hitmarker = (hitmarker != 0);
    common::SetEnhancedConfig(cfg);
}

int32_t BlackWebBuyLoadout(uint32_t primary_weapon, uint32_t secondary_weapon, uint32_t tactical_flags) {
    return mp::BuyLoadout(static_cast<uint8_t>(primary_weapon & 0xFFu),
                          static_cast<uint8_t>(secondary_weapon & 0xFFu),
                          static_cast<uint8_t>(tactical_flags & 0xFFu))
               ? 1
               : 0;
}

int32_t BlackWebEquipSlot(uint32_t slot_idx) {
    return mp::RequestEquipSlot(static_cast<uint8_t>(slot_idx & 0xFFu)) ? 1 : 0;
}

int32_t BlackWebGetWeaponsJson(char* out_buf, uint32_t max_len) {
    if (!out_buf || max_len == 0) return 0;
    const std::string json = mp::GetWeaponCatalogJson(kernel::g_base);
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(json.size()), max_len - 1u);
    if (n > 0) std::memcpy(out_buf, json.data(), n);
    out_buf[n] = '\0';
    return static_cast<int32_t>(n);
}

static std::atomic<uint32_t> g_key_mask{0};
static std::atomic<uint8_t> g_mouse_buttons{0};

void BlackWebPushKeyState(uint32_t key_mask) {
    g_key_mask.store(key_mask, std::memory_order_relaxed);
    common::SetWebKeyboardMouse(key_mask, g_mouse_buttons.load(std::memory_order_relaxed), 0.0f, 0.0f);
}

void BlackWebPushMouseState(uint32_t buttons, float dx, float dy) {
    const uint8_t b = static_cast<uint8_t>(buttons & 0xFFu);
    g_mouse_buttons.store(b, std::memory_order_relaxed);
    common::SetWebKeyboardMouse(g_key_mask.load(std::memory_order_relaxed), b, dx, dy);
}

void BlackWebPushGamepadState(uint32_t connected, uint32_t buttons, uint32_t analog_low, uint32_t analog_high,
                              int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint8_t analog[8]{};
    analog[0] = uint8_t(analog_low & 0xFFu);
    analog[1] = uint8_t((analog_low >> 8) & 0xFFu);
    analog[2] = uint8_t((analog_low >> 16) & 0xFFu);
    analog[3] = uint8_t((analog_low >> 24) & 0xFFu);
    analog[4] = uint8_t(analog_high & 0xFFu);
    analog[5] = uint8_t((analog_high >> 8) & 0xFFu);
    analog[6] = uint8_t((analog_high >> 16) & 0xFFu);
    analog[7] = uint8_t((analog_high >> 24) & 0xFFu);
    common::SetWebGamepad(connected != 0, static_cast<uint16_t>(buttons & 0xFFFFu), analog, static_cast<int16_t>(lx),
                          static_cast<int16_t>(ly), static_cast<int16_t>(rx), static_cast<int16_t>(ry));
}

int32_t BlackWebPushIncomingNet(uint32_t event_type, uint32_t peer_id, uint32_t channel, const uint8_t* data,
                                uint32_t len) {
    auto* tr = mp::ActiveWebRtcTransport();
    if (!tr) return 0;
    const auto peer = static_cast<netcore::PeerId>(peer_id);
    if (event_type == 0) {
        tr->push_incoming_connected(peer);
        return 1;
    }
    if (event_type == 1) {
        tr->push_incoming_disconnected(peer);
        return 1;
    }
    if (event_type == 2 && data && len > 0) {
        const auto ch = channel == 1u ? netcore::Channel::Unreliable : netcore::Channel::Reliable;
        return tr->push_incoming_packet(peer, ch, std::span<const uint8_t>(data, len)) ? 1 : 0;
    }
    return 0;
}

int32_t BlackWebPopOutgoingNet(uint32_t* out_peer, uint32_t* out_channel, uint8_t* out_buf, uint32_t max_len,
                               int32_t* out_kind) {
    auto* tr = mp::ActiveWebRtcTransport();
    if (!tr) return -1;

    netcore::PeerId disc_peer = 0;
    if (tr->pop_disconnect(disc_peer)) {
        if (out_peer) *out_peer = disc_peer;
        if (out_channel) *out_channel = 0;
        if (out_kind) *out_kind = 1;
        return 0;
    }

    mp::WebRtcOutgoingPacket pkt;
    if (!tr->pop_outgoing(pkt)) return -1;
    if (out_peer) *out_peer = pkt.peer;
    if (out_channel) *out_channel = (pkt.channel == netcore::Channel::Unreliable) ? 1u : 0u;
    if (out_kind) *out_kind = 0;
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(pkt.payload.size()), max_len);
    if (out_buf && n > 0) std::memcpy(out_buf, pkt.payload.data(), n);
    return static_cast<int32_t>(n);
}

int32_t BlackWebGetStatusJson(char* out_buf, uint32_t max_len) {
    if (!out_buf || max_len == 0) return 0;
    const mp::BridgeTelemetry tel = mp::GetTelemetry(kernel::g_base);
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(app::g_status_mu);
        msg = app::g_boot_message;
    }
    auto escape_json = [](const std::string& src) {
        std::string escaped;
        escaped.reserve(src.size());
        for (char c : src) {
            if (c == '"' || c == '\\') escaped.push_back('\\');
            if (static_cast<unsigned char>(c) >= 0x20) escaped.push_back(c);
        }
        return escaped;
    };
    const std::string escaped = escape_json(msg);

    std::string json;
    json.reserve(1024);
    char hdr[768];
    const int hdr_len = std::snprintf(
        hdr, sizeof(hdr),
        "{\"bootState\":%d,\"bootMessage\":\"%s\",\"hasWebGpu\":%s,\"gpuReady\":%s,\"canvasW\":%u,\"canvasH\":%u,"
        "\"presents\":%llu,\"draws\":%llu,\"clears\":%llu,\"copies\":%llu,\"shaders\":%llu,\"role\":%u,"
        "\"localSlot\":%u,\"tick\":%u,\"peers\":%u,\"clientJoined\":%s,\"inLevel\":%s,\"guestReady\":%s,"
        "\"frames\":%llu,\"audioFrames\":%llu,"
        "\"primaryWeapon\":%u,\"secondaryWeapon\":%u,\"activeWeapon\":%u,\"activeSlot\":%u,\"tacticalFlags\":%u,"
        "\"lastLoadoutResult\":%u,\"credits\":%u,\"maxCredits\":%u,\"kitCost\":%u,\"buyTicksLeft\":%u,"
        "\"fixedBudget\":%s,\"enforceLoadout\":%s,\"hp\":%u,\"kills\":%u,\"deaths\":%u,"
        "\"hitmarkerSeq\":%llu,\"engineWeaponCount\":%u,\"activeEngineWeaponId\":%u,\"players\":[",
        web_stats::g_boot_state.load(std::memory_order_relaxed), escaped.c_str(),
        app::g_has_webgpu.load(std::memory_order_relaxed) ? "true" : "false",
        web_stats::g_gpu_ready.load(std::memory_order_relaxed) ? "true" : "false",
        app::g_canvas_w.load(std::memory_order_relaxed), app::g_canvas_h.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(web_stats::g_presents.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(web_stats::g_draws.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(web_stats::g_clears.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(web_stats::g_copies.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(web_stats::g_shaders_compiled.load(std::memory_order_relaxed)),
        static_cast<unsigned>(tel.role), static_cast<unsigned>(tel.local_slot), static_cast<unsigned>(tel.tick),
        static_cast<unsigned>(tel.connected_peers), tel.client_joined ? "true" : "false",
        tel.in_level ? "true" : "false", kernel::g_base ? "true" : "false",
        static_cast<unsigned long long>(mp::FrameCount()),
        static_cast<unsigned long long>(web_stats::g_audio_frames_pushed.load(std::memory_order_relaxed)),
        static_cast<unsigned>(tel.primary_weapon), static_cast<unsigned>(tel.secondary_weapon),
        static_cast<unsigned>(tel.active_weapon), static_cast<unsigned>(tel.active_slot),
        static_cast<unsigned>(tel.tactical_flags), static_cast<unsigned>(tel.last_loadout_result),
        static_cast<unsigned>(tel.credits), static_cast<unsigned>(tel.max_credits),
        static_cast<unsigned>(tel.kit_cost), static_cast<unsigned>(tel.buy_ticks_left),
        tel.fixed_budget_mode ? "true" : "false", tel.enforce_loadout ? "true" : "false",
        static_cast<unsigned>(tel.local_health), static_cast<int>(tel.local_kills),
        static_cast<int>(tel.local_deaths), static_cast<unsigned long long>(tel.hitmarker_seq),
        static_cast<unsigned>(tel.engine_weapon_count), static_cast<unsigned>(tel.active_engine_weapon_id));
    if (hdr_len > 0) json.append(hdr, static_cast<std::size_t>(std::min<int>(hdr_len, sizeof(hdr) - 1)));

    bool first_p = true;
    for (const auto& p : tel.players) {
        if (!p.occupied) continue;
        if (!first_p) json.push_back(',');
        first_p = false;
        const std::string pname = escape_json(p.name.empty() ? ("Operador " + std::to_string(p.player_id + 1)) : p.name);
        char prow[256];
        const int plen = std::snprintf(
            prow, sizeof(prow),
            "{\"id\":%u,\"name\":\"%s\",\"alive\":%s,\"hp\":%u,\"kills\":%u,\"deaths\":%u,\"weapon\":%u,\"primary\":%u,\"secondary\":%u,\"tactical\":%u}",
            static_cast<unsigned>(p.player_id), pname.c_str(), p.alive ? "true" : "false",
            static_cast<unsigned>(p.health), static_cast<int>(p.kills), static_cast<int>(p.deaths),
            static_cast<unsigned>(p.weapon), static_cast<unsigned>(p.primary_weapon),
            static_cast<unsigned>(p.secondary_weapon), static_cast<unsigned>(p.tactical_flags));
        if (plen > 0) json.append(prow, static_cast<std::size_t>(std::min<int>(plen, sizeof(prow) - 1)));
    }
    json += "]}";

    const uint32_t out_n = std::min<uint32_t>(static_cast<uint32_t>(json.size()), max_len - 1u);
    if (out_n > 0) std::memcpy(out_buf, json.data(), out_n);
    out_buf[out_n] = '\0';
    return static_cast<int32_t>(out_n);
}

}  // extern "C"
