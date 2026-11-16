#pragma once
// Override de cpu/x86_ops.h para WebAssembly (wasm32):
//   1. Suprime __declspec(dllimport/dllexport) e pragmas MSVC.
//   2. Redireciona X86_WEAK_FUNC para __attribute__((weak, alias(...))) nativo do LLVM/wasm-ld.
//   3. Dobra todo endereço de 32 bits da memória convidada via wasm_mem::FoldGuestVa (arena de 256 MB).

#include "common/wasm_memory.h"

#ifndef __declspec
#define __declspec(x)
#define BLACK_UNDEF_DECLSPEC 1
#endif

#define x86_rep_movs x86_rep_movs_sdk_unused_
#define x86_rep_stos x86_rep_stos_sdk_unused_

#include_next "cpu/x86_ops.h"

#undef x86_rep_movs
#undef x86_rep_stos
#ifdef BLACK_UNDEF_DECLSPEC
#undef __declspec
#undef BLACK_UNDEF_DECLSPEC
#endif

#undef X86_PTR
#undef X86_MEM8
#undef X86_MEM16
#undef X86_MEM32
#undef X86_MEM64
#undef X86_MEMF32
#undef X86_MEMF64

#define X86_PTR(T, a) ((T*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))
#define X86_MEM8(a) (*(uint8_t*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))
#define X86_MEM16(a) (*(x86_u16u*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))
#define X86_MEM32(a) (*(x86_u32u*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))
#define X86_MEM64(a) (*(x86_u64u*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))
#define X86_MEMF32(a) (*(x86_f32u*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))
#define X86_MEMF64(a) (*(x86_f64u*)(base + ::wasm_mem::FoldGuestVa((uint32_t)(a))))

#undef X86_HOST_API
#undef X86_HOST_EXPORT
#undef X86_WEAK_FUNC
#undef X86_FUNC
#undef X86_EXTERN_FUNC
#undef X86_TRACED_FUNC

#define X86_HOST_API
#define X86_HOST_EXPORT
#define X86_STR_(x) #x
#define X86_STR(x) X86_STR_(x)
#define X86_WEAK_FUNC(name, impl) \
    extern "C" __attribute__((weak, alias(X86_STR(impl)))) void name(X86Context& __restrict ctx, uint8_t* base);
#define X86_FUNC(name) extern "C" void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_EXTERN_FUNC(name) extern "C" void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_TRACED_FUNC(name, impl, va)                                        \
    extern "C" void name##_traced(X86Context& __restrict ctx, uint8_t* base) { \
        uint32_t x86_ret = x86_trace(ctx, va, 0);                              \
        impl(ctx, base);                                                       \
        x86_trace(ctx, va, x86_ret);                                           \
    }                                                                          \
    X86_WEAK_FUNC(name, name##_traced)

inline void x86_rep_movs(X86Context& ctx, uint8_t* base, uint32_t w) {
    uint32_t n = ctx.ecx;
    if (!n) return;
    int32_t d = ctx.df ? -int32_t(w) : int32_t(w);
    if (!ctx.df && w == 1) {
        const uint32_t dst_off = ::wasm_mem::FoldGuestVa(ctx.edi);
        const uint32_t src_off = ::wasm_mem::FoldGuestVa(ctx.esi);
        if (dst_off + n <= ::wasm_mem::kArenaSize && src_off + n <= ::wasm_mem::kArenaSize) {
            std::memmove(base + dst_off, base + src_off, n);
            ctx.esi += n;
            ctx.edi += n;
            ctx.ecx = 0;
            return;
        }
    }
    for (; n; n--) {
        std::memcpy(base + ::wasm_mem::FoldGuestVa(ctx.edi), base + ::wasm_mem::FoldGuestVa(ctx.esi), w);
        ctx.esi += d;
        ctx.edi += d;
    }
    ctx.ecx = 0;
}

inline void x86_rep_stos(X86Context& ctx, uint8_t* base, uint32_t w) {
    uint32_t n = ctx.ecx;
    if (!n) return;
    int32_t d = ctx.df ? -int32_t(w) : int32_t(w);
    if (!ctx.df && w == 1) {
        const uint32_t dst_off = ::wasm_mem::FoldGuestVa(ctx.edi);
        if (dst_off + n <= ::wasm_mem::kArenaSize) {
            std::memset(base + dst_off, uint8_t(ctx.eax), n);
            ctx.edi += n;
            ctx.ecx = 0;
            return;
        }
    }
    uint32_t v = ctx.eax;
    for (; n; n--) {
        std::memcpy(base + ::wasm_mem::FoldGuestVa(ctx.edi), &v, w);
        ctx.edi += d;
    }
    ctx.ecx = 0;
}
