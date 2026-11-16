#pragma once
// Override de kernel/kernel.h para WebAssembly (wasm32):
// Redireciona HostPtr/G/R*/W* e Args::operator[] através de wasm_mem::FoldGuestVa
// e mapeia _fseeki64 para fseeko (usado por sdk/runtime/kernel/files.cpp).

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/wasm_memory.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

#ifndef _fseeki64
#define _fseeki64 fseeko
#endif

#define G G_sdk_unused_
#define R32 R32_sdk_unused_
#define W32 W32_sdk_unused_
#define R16 R16_sdk_unused_
#define W16 W16_sdk_unused_
#define R64 R64_sdk_unused_
#define W64 W64_sdk_unused_
#define HostPtr HostPtr_sdk_unused_
#define Args Args_sdk_unused_

#include_next "kernel/kernel.h"

#undef G
#undef R32
#undef W32
#undef R16
#undef W16
#undef R64
#undef W64
#undef HostPtr
#undef Args

namespace kernel {

template <typename T>
inline T& G(uint32_t va) {
    return *reinterpret_cast<T*>(g_base + ::wasm_mem::FoldGuestVa(va));
}
inline uint32_t R32(uint32_t va) {
    uint32_t v;
    std::memcpy(&v, g_base + ::wasm_mem::FoldGuestVa(va), 4);
    return v;
}
inline void W32(uint32_t va, uint32_t v) {
    std::memcpy(g_base + ::wasm_mem::FoldGuestVa(va), &v, 4);
}
inline uint16_t R16(uint32_t va) {
    uint16_t v;
    std::memcpy(&v, g_base + ::wasm_mem::FoldGuestVa(va), 2);
    return v;
}
inline void W16(uint32_t va, uint16_t v) {
    std::memcpy(g_base + ::wasm_mem::FoldGuestVa(va), &v, 2);
}
inline uint64_t R64(uint32_t va) {
    uint64_t v;
    std::memcpy(&v, g_base + ::wasm_mem::FoldGuestVa(va), 8);
    return v;
}
inline void W64(uint32_t va, uint64_t v) {
    std::memcpy(g_base + ::wasm_mem::FoldGuestVa(va), &v, 8);
}
inline void* HostPtr(uint32_t va) {
    return g_base + ::wasm_mem::FoldGuestVa(va);
}

struct Args {
    X86Context& ctx;
    uint32_t operator[](int i) const { return R32(ctx.esp + 4 + 4 * uint32_t(i)); }
};

}  // namespace kernel
