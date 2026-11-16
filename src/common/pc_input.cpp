#include "common/pc_input.h"

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
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "game/black_addrs.h"
#include "kernel/kernel.h"

namespace common {

namespace {

struct ScriptEvent {
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    uint16_t buttons = 0;
    int analog_idx = -1;
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
};

int16_t ClampAxis(int v) {
    return static_cast<int16_t>(std::clamp(v, -32767, 32767));
}

bool ParseToken(const std::string& raw, ScriptEvent& ev) {
    const std::size_t colon = raw.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= raw.size()) return false;

    const std::string timing = raw.substr(0, colon);
    std::string key = raw.substr(colon + 1);
    for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    const std::size_t dash = timing.find('-');
    if (dash == std::string::npos) {
        ev.start_ms = std::strtoull(timing.c_str(), nullptr, 10);
        ev.end_ms = ev.start_ms + 220;
    } else {
        ev.start_ms = std::strtoull(timing.substr(0, dash).c_str(), nullptr, 10);
        ev.end_ms = std::strtoull(timing.substr(dash + 1).c_str(), nullptr, 10);
    }
    if (ev.end_ms <= ev.start_ms) ev.end_ms = ev.start_ms + 150;

    if (key == "UP") ev.buttons = 0x01;
    else if (key == "DOWN") ev.buttons = 0x02;
    else if (key == "LEFT") ev.buttons = 0x04;
    else if (key == "RIGHT") ev.buttons = 0x08;
    else if (key == "START") ev.buttons = 0x10;
    else if (key == "BACK") ev.buttons = 0x20;
    else if (key == "LS") ev.buttons = 0x40;
    else if (key == "RS") ev.buttons = 0x80;
    else if (key == "A") ev.analog_idx = 0;
    else if (key == "B") ev.analog_idx = 1;
    else if (key == "X") ev.analog_idx = 2;
    else if (key == "Y") ev.analog_idx = 3;
    else if (key == "RB" || key == "BLACK") ev.analog_idx = 4;
    else if (key == "LB" || key == "WHITE") ev.analog_idx = 5;
    else if (key == "LT") ev.analog_idx = 6;
    else if (key == "RT") ev.analog_idx = 7;
    else if (key == "LX+") ev.lx = 32000;
    else if (key == "LX-") ev.lx = -32000;
    else if (key == "LY+") ev.ly = 32000;
    else if (key == "LY-") ev.ly = -32000;
    else if (key == "RX+") ev.rx = 24000;
    else if (key == "RX-") ev.rx = -24000;
    else if (key == "RY+") ev.ry = 24000;
    else if (key == "RY-") ev.ry = -24000;
    else return false;

    return true;
}

const std::vector<ScriptEvent>& GetPadScript() {
    static std::vector<ScriptEvent> events = [] {
        std::vector<ScriptEvent> parsed;
        const char* env = std::getenv("BLACK_PAD_SCRIPT");
        if (!env || !*env) return parsed;
        std::string s(env);
        std::size_t pos = 0;
        while (pos < s.size()) {
            const std::size_t comma = s.find(',', pos);
            const std::string item = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
            ScriptEvent ev;
            if (ParseToken(item, ev)) parsed.push_back(ev);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (!parsed.empty()) {
            LOGI("pc_input: loaded %zu scripted pad event(s)", parsed.size());
        }
        return parsed;
    }();
    return events;
}

#ifdef _WIN32
bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
#endif

struct WebInputSnapshot {
    std::mutex mu;
    uint32_t key_mask = 0;
    uint8_t mouse_buttons = 0;
    float mouse_dx = 0.0f;
    float mouse_dy = 0.0f;
    float direct_dx = 0.0f;
    float direct_dy = 0.0f;
    bool gp_connected = false;
    uint16_t gp_buttons = 0;
    uint8_t gp_analog[8] = {};
    int16_t gp_lx = 0;
    int16_t gp_ly = 0;
    int16_t gp_rx = 0;
    int16_t gp_ry = 0;
    EnhancedConfig enhanced{};
};

WebInputSnapshot& WebInput() {
    static WebInputSnapshot s;
    return s;
}

}  // namespace

void SetEnhancedConfig(const EnhancedConfig& cfg) {
    auto& w = WebInput();
    std::lock_guard<std::mutex> lock(w.mu);
    w.enhanced = cfg;
    w.enhanced.mouse_sens = std::clamp(w.enhanced.mouse_sens, 0.01f, 0.6f);
    w.enhanced.ads_sens_mult = std::clamp(w.enhanced.ads_sens_mult, 0.1f, 2.0f);
    w.enhanced.fov_deg = std::clamp(w.enhanced.fov_deg, 55.0f, 115.0f);
    if (!w.enhanced.mouse_direct) {
        w.direct_dx = 0.0f;
        w.direct_dy = 0.0f;
    }
}

EnhancedConfig GetEnhancedConfig() {
    auto& w = WebInput();
    std::lock_guard<std::mutex> lock(w.mu);
    return w.enhanced;
}

bool ConsumeDirectMouseLook(float& out_dx, float& out_dy) {
    auto& w = WebInput();
    std::lock_guard<std::mutex> lock(w.mu);
    if (!w.enhanced.mouse_direct) {
        w.direct_dx = 0.0f;
        w.direct_dy = 0.0f;
        out_dx = 0.0f;
        out_dy = 0.0f;
        return false;
    }
    out_dx = w.direct_dx;
    out_dy = w.direct_dy;
    w.direct_dx = 0.0f;
    w.direct_dy = 0.0f;
    return (out_dx != 0.0f || out_dy != 0.0f);
}

void ClearDirectMouseLook() {
    auto& w = WebInput();
    std::lock_guard<std::mutex> lock(w.mu);
    w.direct_dx = 0.0f;
    w.direct_dy = 0.0f;
}

void SetWebKeyboardMouse(uint32_t key_mask, uint8_t mouse_buttons, float mouse_dx, float mouse_dy) {
    auto& w = WebInput();
    std::lock_guard<std::mutex> lock(w.mu);
    w.key_mask = key_mask;
    w.mouse_buttons = mouse_buttons;
    if (w.enhanced.mouse_direct) {
        w.direct_dx += mouse_dx;
        w.direct_dy += mouse_dy;
    } else {
        w.mouse_dx += mouse_dx;
        w.mouse_dy += mouse_dy;
    }
}

void SetWebGamepad(bool connected, uint16_t buttons, const uint8_t analog[8], int16_t lx, int16_t ly, int16_t rx,
                   int16_t ry) {
    auto& w = WebInput();
    std::lock_guard<std::mutex> lock(w.mu);
    w.gp_connected = connected;
    w.gp_buttons = buttons;
    if (analog) {
        std::copy_n(analog, 8, w.gp_analog);
    } else {
        std::fill_n(w.gp_analog, 8, uint8_t(0));
    }
    w.gp_lx = lx;
    w.gp_ly = ly;
    w.gp_rx = rx;
    w.gp_ry = ry;
}

void PollPcPad(int port, input::Pad& pad) {
    if (port != 0) return;

#ifdef _WIN32
    static std::mutex s_lock;
    std::lock_guard<std::mutex> lock(s_lock);

    static const auto s_t0 = std::chrono::steady_clock::now();
    const uint64_t elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count());

    // Xbox reconhece controle conectado na porta 0 para uso de teclado/mouse no PC.
    pad.connected = true;

    HWND hwnd = static_cast<HWND>(app::Window());
    if (hwnd && GetForegroundWindow() == hwnd) {
        if (KeyDown(VK_UP)) pad.buttons |= 0x01;
        if (KeyDown(VK_DOWN)) pad.buttons |= 0x02;
        if (KeyDown(VK_LEFT)) pad.buttons |= 0x04;
        if (KeyDown(VK_RIGHT)) pad.buttons |= 0x08;
        if (KeyDown(VK_RETURN)) pad.buttons |= 0x10;   // Start
        if (KeyDown(VK_TAB)) pad.buttons |= 0x20;      // Back
        if (KeyDown(VK_LSHIFT)) pad.buttons |= 0x40;   // Left Thumb
        if (KeyDown(VK_CONTROL)) pad.buttons |= 0x80;  // Right Thumb

        if (KeyDown(VK_SPACE)) pad.analog[0] = 0xFF;               // A
        if (KeyDown('C') || KeyDown(VK_BACK)) pad.analog[1] = 0xFF; // B
        if (KeyDown('R')) pad.analog[2] = 0xFF;                    // X (Reload)
        if (KeyDown('E') || KeyDown('1')) pad.analog[3] = 0xFF;    // Y (Weapon swap)
        if (KeyDown('G')) pad.analog[4] = 0xFF;                    // Black (Grenade / Fire mode)
        if (KeyDown('Q')) pad.analog[5] = 0xFF;                    // White (Fire mode / Action)
        if (KeyDown('V') || KeyDown(VK_RBUTTON)) pad.analog[6] = 0xFF; // LT (Aim / Zoom)
        if (KeyDown('F') || KeyDown(VK_LBUTTON)) pad.analog[7] = 0xFF; // RT (Fire)

        int lx = pad.lx;
        int ly = pad.ly;
        int rx = pad.rx;
        int ry = pad.ry;

        if (KeyDown('A')) lx -= 32000;
        if (KeyDown('D')) lx += 32000;
        if (KeyDown('W')) ly += 32000;
        if (KeyDown('S')) ly -= 32000;
        if (!black::IsInLevel(kernel::g_base)) {
            if (KeyDown('W')) pad.buttons |= 0x01;
            if (KeyDown('S')) pad.buttons |= 0x02;
            if (KeyDown('A')) pad.buttons |= 0x04;
            if (KeyDown('D')) pad.buttons |= 0x08;
        }

        if (KeyDown('J')) rx -= 24000;
        if (KeyDown('L')) rx += 24000;
        if (KeyDown('I')) ry += 24000;
        if (KeyDown('K')) ry -= 24000;

        // Mira com mouse opt-in (toggle F2 quando a janela estiver em foco).
        static bool mouse_look = false;
        static bool f2_prev = false;
        const bool f2_now = KeyDown(VK_F2);
        if (f2_now && !f2_prev) mouse_look = !mouse_look;
        f2_prev = f2_now;

        if (mouse_look) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                POINT center{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
                POINT screen_center = center;
                ClientToScreen(hwnd, &screen_center);
                POINT cur{};
                if (GetCursorPos(&cur)) {
                    const int dx = cur.x - screen_center.x;
                    const int dy = cur.y - screen_center.y;
                    if (GetEnhancedConfig().mouse_direct) {
                        auto& w = WebInput();
                        std::lock_guard<std::mutex> lk(w.mu);
                        w.direct_dx += static_cast<float>(dx);
                        w.direct_dy += static_cast<float>(dy);
                    } else {
                        rx += std::clamp(dx * 900, -32767, 32767);
                        ry += std::clamp(-dy * 900, -32767, 32767);
                    }
                    SetCursorPos(screen_center.x, screen_center.y);
                }
            }
        }

        pad.lx = ClampAxis(lx);
        pad.ly = ClampAxis(ly);
        pad.rx = ClampAxis(rx);
        pad.ry = ClampAxis(ry);
    }

    for (const auto& ev : GetPadScript()) {
        if (elapsed_ms >= ev.start_ms && elapsed_ms <= ev.end_ms) {
            pad.buttons |= ev.buttons;
            if (ev.analog_idx >= 0 && ev.analog_idx < 8) {
                pad.analog[ev.analog_idx] = 0xFF;
            }
            pad.lx = ClampAxis(int(pad.lx) + ev.lx);
            pad.ly = ClampAxis(int(pad.ly) + ev.ly);
            pad.rx = ClampAxis(int(pad.rx) + ev.rx);
            pad.ry = ClampAxis(int(pad.ry) + ev.ry);
        }
    }
#elif defined(__EMSCRIPTEN__)
    pad.connected = true;
    auto& w = WebInput();
    uint32_t km = 0;
    uint8_t mb = 0;
    float mdx = 0.0f;
    float mdy = 0.0f;
    bool gp_conn = false;
    uint16_t gp_btns = 0;
    uint8_t gp_an[8] = {};
    int16_t gp_lx = 0, gp_ly = 0, gp_rx = 0, gp_ry = 0;
    {
        std::lock_guard<std::mutex> lock(w.mu);
        km = w.key_mask;
        mb = w.mouse_buttons;
        mdx = w.mouse_dx;
        mdy = w.mouse_dy;
        w.mouse_dx = 0.0f;
        w.mouse_dy = 0.0f;
        gp_conn = w.gp_connected;
        gp_btns = w.gp_buttons;
        std::copy_n(w.gp_analog, 8, gp_an);
        gp_lx = w.gp_lx;
        gp_ly = w.gp_ly;
        gp_rx = w.gp_rx;
        gp_ry = w.gp_ry;
    }

    if (gp_conn) {
        pad.buttons |= gp_btns;
        for (int i = 0; i < 8; ++i) {
            pad.analog[i] = std::max(pad.analog[i], gp_an[i]);
        }
    }
    if (km & kWebKeyUp) pad.buttons |= 0x01;
    if (km & kWebKeyDown) pad.buttons |= 0x02;
    if (km & kWebKeyLeft) pad.buttons |= 0x04;
    if (km & kWebKeyRight) pad.buttons |= 0x08;
    if (km & kWebKeyStart) pad.buttons |= 0x10;
    if (km & kWebKeyBack) pad.buttons |= 0x20;
    if (km & kWebKeyLShift) pad.buttons |= 0x40;
    if (km & (kWebKeyCtrl | kWebKeyMelee)) pad.buttons |= 0x80;

    if (km & kWebKeySpace) pad.analog[0] = 0xFF;               // A
    if (km & kWebKeyC) pad.analog[1] = 0xFF;                   // B
    if (km & kWebKeyR) pad.analog[2] = 0xFF;                   // X
    if (km & (kWebKeyE | kWebKeyY)) pad.analog[3] = 0xFF;      // Y
    if (km & kWebKeyG) pad.analog[4] = 0xFF;                   // Black
    if (km & kWebKeyQ) pad.analog[5] = 0xFF;                   // White
    if ((km & kWebKeyV) || (mb & 2u)) pad.analog[6] = 0xFF;    // LT
    if ((km & kWebKeyF) || (mb & 1u)) pad.analog[7] = 0xFF;    // RT

    int lx = gp_conn ? gp_lx : pad.lx;
    int ly = gp_conn ? gp_ly : pad.ly;
    int rx = gp_conn ? gp_rx : pad.rx;
    int ry = gp_conn ? gp_ry : pad.ry;

    if (km & kWebKeyA) lx -= 32000;
    if (km & kWebKeyD) lx += 32000;
    if (km & kWebKeyW) ly += 32000;
    if (km & kWebKeyS) ly -= 32000;
    if (!black::IsInLevel(kernel::g_base)) {
        if (km & kWebKeyW) pad.buttons |= 0x01;
        if (km & kWebKeyS) pad.buttons |= 0x02;
        if (km & kWebKeyA) pad.buttons |= 0x04;
        if (km & kWebKeyD) pad.buttons |= 0x08;
    }

    rx += static_cast<int>(std::clamp(mdx * 750.0f, -32767.0f, 32767.0f));
    ry += static_cast<int>(std::clamp(-mdy * 750.0f, -32767.0f, 32767.0f));

    pad.lx = ClampAxis(lx);
    pad.ly = ClampAxis(ly);
    pad.rx = ClampAxis(rx);
    pad.ry = ClampAxis(ry);
#else
    (void)pad;
#endif
}

}  // namespace common
