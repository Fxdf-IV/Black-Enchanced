#include "core/input.h"

#include "app/app.h"
#include "core/log.h"

namespace input {

void Init() {
    LOGI("input_wasm: browser keyboard/mouse/Gamepad API bridge ready");
}

uint16_t HeldOnAnyController() {
    const Pad p = Poll(0);
    uint16_t held = 0;
    if (p.analog[0] > 0x40) held |= 0x1000u;  // A
    if (p.analog[1] > 0x40) held |= 0x2000u;  // B
    if (p.analog[2] > 0x40) held |= 0x4000u;  // X
    if (p.analog[3] > 0x40) held |= 0x8000u;  // Y
    return held;
}

Pad Poll(int port) {
    Pad p{};
    if (app::kGame.pad) {
        app::kGame.pad(port, p);
    }
    return p;
}

void Rumble(int, uint16_t, uint16_t) {
    // Opcional no navegador via Gamepad.vibrationActuator.
}

}  // namespace input
