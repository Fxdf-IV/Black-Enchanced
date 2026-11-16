#include "kernel/kernel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "xbe/xbe.h"

namespace dispatch {
uint32_t EntryPoint();
uint32_t KernelThunkAddress(uint32_t ordinal);
void Init();
}  // namespace dispatch

namespace xnet {
void CloseSockets();
}

namespace kernel {

uint8_t* g_base = nullptr;
const xbe::Image* g_image = nullptr;
std::string g_launch_file;

namespace {

xbe::Image g_xbe;
uint32_t g_saved_data = 0;
bool g_kept_marked = false;
std::string g_kept_picture;

std::string PersistFile() {
    if (g_launch_file.empty()) return {};
    return g_launch_file + ".persist";
}

void LoadPersistedDisplay() {
    const std::string p = PersistFile();
    if (p.empty()) return;
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    uint32_t hdr[4] = {};
    in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!in || hdr[3] == 0) return;
    const uint32_t va = mem::AllocContiguous(hdr[3], 0, 0xFFFFFFFFu, 0x1000u);
    if (!va) return;
    in.read(static_cast<char*>(HostPtr(va)), hdr[3]);
    g_saved_data = va;
    (void)hdr[0];
    (void)hdr[1];
    (void)hdr[2];
}

}  // namespace

void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes) {
    const std::string p = PersistFile();
    if (p.empty() || !pixels || !bytes) return;
    std::ofstream out(p, std::ios::binary);
    if (!out) return;
    const uint32_t hdr[4] = {common, format, size, bytes};
    out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    out.write(static_cast<const char*>(pixels), bytes);
}

uint32_t SavedDataAddress() { return g_saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_saved_data = surface; }
void MarkPersistedDisplay() { g_kept_marked = true; }
bool KeptDisplayMarked() { return g_kept_marked; }
void SetShownPicture(std::string path) { g_kept_picture = std::move(path); }
const std::string& KeptPicture() { return g_kept_picture; }
std::wstring HandoverEvent(unsigned long) { return {}; }

bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::string& game_dir,
          const std::string& data_dir) {
    if (!mem::Init()) return false;
    g_base = mem::Base();

    std::string err;
    if (!g_xbe.Parse(std::move(xbe), err)) {
        LOGE("boot_wasm: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    const xbe::Image& img = g_xbe;
    if (img.entry != dispatch::EntryPoint()) {
        LOGE("boot_wasm: %s entry %08X != recompiled entry %08X", xbe_name.c_str(), img.entry,
             dispatch::EntryPoint());
        return false;
    }
    LOGI("boot_wasm: title %08X, entry %08X, %zu sections", img.title_id, img.entry, img.sections.size());

    mem::ReserveImage(img.base, img.image_size);
    std::memcpy(HostPtr(img.base), img.file.data(), std::min<size_t>(img.header_size, img.file.size()));

    const uint32_t sec_hdr = img.U32(0x120);
    for (size_t i = 0; i < img.sections.size(); ++i) {
        const auto& s = img.sections[i];
        if (!(s.flags & xbe::kPreload)) continue;
        std::memcpy(HostPtr(s.va), img.file.data() + s.raw, std::min(s.rsize, s.vsize));
        W32(sec_hdr + uint32_t(i) * 56u + 0x18u, 1u);
    }

    if (app::kGame.loaded) app::kGame.loaded();

    InitKernelData();
    dispatch::Init();

    uint32_t missing = 0;
    for (uint32_t slot = img.kernel_thunk;; slot += 4u) {
        const uint32_t v = R32(slot);
        if (!v) break;
        const uint32_t ord = v & 0x7FFFFFFFu;
        const uint32_t data = DataAddress(ord);
        W32(slot, data ? data : dispatch::KernelThunkAddress(ord));
        if (!HasExport(ord)) {
            LOGW("boot_wasm: missing kernel import %u (%s)", ord, ExportName(ord));
            ++missing;
        }
    }
    LOGI("boot_wasm: kernel imports resolved (%u missing)", missing);

    MountDefaults(game_dir, data_dir);

    g_launch_file = data_dir + "/launch.bin";
    std::ifstream lf(g_launch_file, std::ios::binary);
    if (lf) {
        const uint32_t page = mem::AllocContiguous(0x1000u, 0, 0xFFFFFFFFu, 0x1000u);
        lf.read(static_cast<char*>(HostPtr(page)), 0x1000);
        lf.close();
        std::remove(g_launch_file.c_str());
        W32(g_launch_data_page_va, page);
        LoadPersistedDisplay();
    }
    std::remove(PersistFile().c_str());

    uint32_t tls = 0;
    if (img.tls_addr) {
        tls = ((img.tls.data_end - img.tls.data_start + img.tls.zero_fill + 15u) & ~15u) + 4u;
    }
    ThreadRef t = CreateThread(std::max<uint32_t>(img.stack_commit, 0x10000u), tls, 0, img.entry, 0, false);
    StartThread(t);
    return true;
}

const char* ImageExe(std::string xbe_path) {
    for (char& c : xbe_path) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& [name, exe] : app::kGame.images) {
        if (xbe_path.find(name) != std::string::npos) return exe;
    }
    return nullptr;
}

void Shutdown(int code) {
    LOGI("boot_wasm: Shutdown(%d)", code);
    xnet::CloseSockets();
    logx::Flush();
    std::exit(code);
}

void Relaunch(const char* exe) {
    LOGW("boot_wasm: Relaunch(%s) ignored in browser", exe ? exe : "");
    xnet::CloseSockets();
    logx::Flush();
    std::exit(0);
}

}  // namespace kernel
