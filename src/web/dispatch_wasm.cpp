#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <unordered_set>
#include <vector>

namespace {

std::vector<X86Func*> g_table;
std::vector<bool> g_continuation;
uint32_t g_lo = 0, g_hi = 0;

struct Export {
    const char* name = nullptr;
    kernel::KernelFn fn = nullptr;
    uint32_t data = 0;
};
Export g_exports[400];

constexpr uint32_t kKernelThunkBase = 0xFE000000u;

}  // namespace

namespace dispatch {

bool Load(const std::filesystem::path&) {
    g_lo = X86CodeBase;
    g_hi = X86CodeEnd;
    return true;
}

uint32_t EntryPoint() { return X86EntryPoint; }
bool InCode(uint32_t va) { return va >= g_lo && va < g_hi; }

void Init() {
    g_lo = X86CodeBase;
    g_hi = X86CodeEnd;
    g_table.assign(g_hi - g_lo, nullptr);
    size_t n = 0;
    for (const X86FuncMapping* m = X86FuncMappings; m->host; ++m, ++n) {
        if (m->guest >= g_lo && m->guest < g_hi) {
            g_table[m->guest - g_lo] = m->host;
        }
    }
    g_continuation.assign(g_hi - g_lo, false);
    for (const uint32_t* c = X86Continuations; *c; ++c) {
        if (*c >= g_lo && *c < g_hi) {
            g_continuation[*c - g_lo] = true;
        }
    }
    LOGI("dispatch_wasm: %zu functions over %08X..%08X", n, g_lo, g_hi);
}

X86Func* Find(uint32_t guest) {
    if (guest >= g_lo && guest < g_hi) return g_table[guest - g_lo];
    return nullptr;
}

constexpr uint32_t kHostThunkBase = 0xFD000000u;
static std::vector<X86Func*> g_host_thunks;
static std::mutex g_host_lock;

uint32_t HostThunk(X86Func* fn) {
    std::lock_guard<std::mutex> lock(g_host_lock);
    for (size_t i = 0; i < g_host_thunks.size(); ++i) {
        if (g_host_thunks[i] == fn) return kHostThunkBase + uint32_t(i) * 4u;
    }
    g_host_thunks.push_back(fn);
    return kHostThunkBase + uint32_t(g_host_thunks.size() - 1) * 4u;
}

uint32_t KernelThunkAddress(uint32_t ordinal) { return kKernelThunkBase + ordinal * 4u; }

}  // namespace dispatch

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (target >= g_lo && target < g_hi && g_continuation[target - g_lo]) {
        throw X86Resume{target, ctx.esp};
    }
    if (X86Func* f = dispatch::Find(target)) {
        f(ctx, base);
        return;
    }
    if (target >= kKernelThunkBase && target < kKernelThunkBase + 400u * 4u) {
        x86_kernel(ctx, base, (target - kKernelThunkBase) / 4u);
        return;
    }
    if (target >= dispatch::kHostThunkBase) {
        const size_t i = (target - dispatch::kHostThunkBase) / 4u;
        X86Func* f = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch::g_host_lock);
            if (i < dispatch::g_host_thunks.size()) f = dispatch::g_host_thunks[i];
        }
        if (f) {
            f(ctx, base);
            return;
        }
    }
    const uint32_t ret = kernel::R32(ctx.esp);
    LOGE("indirect call to %08X has no recompiled function (ret %08X, esp %08X)", target, ret, ctx.esp);
    x86_trap(ctx, ret, "indirect call to unknown code");
}

void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r) {
    ctx.esp = r.esp;
    dispatch::Find(r.target)(ctx, base);
}

void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    if (ordinal < 400 && g_exports[ordinal].fn) {
        g_exports[ordinal].fn(ctx, base);
        return;
    }
    LOGE("kernel export %u (%s) is not implemented, ret %08X", ordinal, kernel::ExportName(ordinal),
         kernel::R32(ctx.esp));
    x86_trap(ctx, kernel::R32(ctx.esp), "unimplemented kernel export");
}

namespace kernel {

void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn) {
    if (ordinal >= 400) return;
    g_exports[ordinal].name = name;
    g_exports[ordinal].fn = fn;
}

static uint32_t g_kdata_next = mem::kKernelDataBase + 0x1000u;

uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (ordinal >= 400) return 0;
    if (g_exports[ordinal].data) return g_exports[ordinal].data;
    const uint32_t a = (g_kdata_next + 15u) & ~15u;
    g_kdata_next = a + ((size + 15u) & ~15u);
    g_exports[ordinal].name = name;
    g_exports[ordinal].data = a;
    return a;
}

uint32_t DataAddress(uint32_t ordinal) { return ordinal < 400 ? g_exports[ordinal].data : 0; }
const char* ExportName(uint32_t ordinal) {
    return ordinal < 400 && g_exports[ordinal].name ? g_exports[ordinal].name : OrdinalName(ordinal);
}
bool HasExport(uint32_t ordinal) { return ordinal < 400 && (g_exports[ordinal].fn || g_exports[ordinal].data); }

uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count) {
    struct {
        uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    } saved{ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    const uint32_t esp = ctx.esp;
    for (const uint32_t* p = args + count; p != args;) {
        ctx.esp -= 4;
        W32(ctx.esp, *--p);
    }
    ctx.esp -= 4;
    W32(ctx.esp, 0xFFFFFFF0u);
    x86_icall(ctx, g_base, fn);
    const uint32_t result = ctx.eax;
    ctx.eax = saved.eax;
    ctx.ecx = saved.ecx;
    ctx.edx = saved.edx;
    ctx.ebx = saved.ebx;
    ctx.esi = saved.esi;
    ctx.edi = saved.edi;
    ctx.ebp = saved.ebp;
    ctx.esp = esp;
    return result;
}

void LogGuestStack(X86Context& ctx, int words) {
    LOGE("  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", ctx.eax, ctx.ecx, ctx.edx,
         ctx.ebx, ctx.esp, ctx.ebp, ctx.esi, ctx.edi);
    for (int i = 0; i < words; ++i) {
        const uint32_t a = ctx.esp + uint32_t(i) * 4u;
        if (!mem::IsCommitted(a)) break;
        const uint32_t v = R32(a);
        if (dispatch::InCode(v)) LOGE("  [esp+%03X] %08X", i * 4, v);
    }
}

}  // namespace kernel

void x86_trap(X86Context& ctx, uint32_t va, const char* what) {
    LOGE("guest trap at %08X: %s", va, what);
    kernel::LogGuestStack(ctx, 64);
    logx::Flush();
    std::abort();
}

uint32_t x86_trace(X86Context& ctx, uint32_t va, uint32_t ret) {
    using kernel::R32;
    if (!ret) {
        ret = R32(ctx.esp);
        LOGI("trace %08X from %08X (%08X, %08X, %08X, %08X) ecx %08X", va, ret, R32(ctx.esp + 4), R32(ctx.esp + 8),
             R32(ctx.esp + 12), R32(ctx.esp + 16), ctx.ecx);
    } else {
        LOGI("trace %08X returned %08X to %08X", va, ctx.eax, ret);
    }
    return ret;
}

void x86_priv(X86Context& ctx, uint32_t va, const char* what) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(va).second) LOGW("privileged instruction %s at %08X ignored", what, va);
    (void)ctx;
}

uint64_t x86_rdtsc() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - start).count();
    const uint64_t sec = uint64_t(ns) / 1000000000ull;
    const uint64_t rem = uint64_t(ns) % 1000000000ull;
    return sec * 733333333ull + rem * 733333333ull / 1000000000ull;
}

void x86_cpuid(X86Context& ctx) {
    switch (ctx.eax) {
        case 0:
            ctx.eax = 2;
            ctx.ebx = 0x756E6547;
            ctx.edx = 0x49656E69;
            ctx.ecx = 0x6C65746E;
            break;
        case 1:
            ctx.eax = 0x0000068A;
            ctx.ebx = 0;
            ctx.ecx = 0;
            ctx.edx = 0x0383F9FF;
            break;
        default:
            ctx.eax = ctx.ebx = ctx.ecx = ctx.edx = 0;
            break;
    }
}
