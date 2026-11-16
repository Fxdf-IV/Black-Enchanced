#include "app/app.h"
#include "common/pc_input.h"
#include "kernel/kernel.h"
#include "mp/mp_bridge.h"

namespace {

void OnGameInit() {
    mp::InitBridge();
}

void OnImageLoaded() {
    mp::OnImageLoaded(kernel::g_base);
}

}  // namespace

const app::GameInfo app::kGame = {
    .title = L"Black Enhanced Online",
    .short_title = L"BLACK",
    .name = L"BlackEnhancedOnline",
    .log = "black_enhanced",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Coloque sua cópia de BLACK (Xbox, NTSC-U) na pasta assets: Black.iso ou default.xbe com as pastas do disco ao lado.",
    .images = {{"default.xbe", "BlackEnhancedOnline"}},
    .options = {},
    .load = nullptr,
    .save = nullptr,
    .init = &OnGameInit,
    .loaded = &OnImageLoaded,
    .window = nullptr,
    .message = nullptr,
    .ui = nullptr,
    .pad = &common::PollPcPad,
    .rumble = nullptr,
    .releases = {},
};
