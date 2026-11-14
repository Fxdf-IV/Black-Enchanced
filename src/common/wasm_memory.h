#pragma once
// Arena de memória convidada para WebAssembly (wasm32) — dobra o espaço de 4 GB do Xbox
// em uma arena contígua de 256 MB mantendo ponteiros de 32 bits intactos no código recompilado.
//
// Mapeamento de nibbles (va >> 28):
//   0x0....... -> 0x00000000 + (va & 0x07FFFFFF)  (128 MB: User / XBE / VirtualAlloc)
//   0x8....... -> 0x08000000 + (va & 0x03FFFFFF)  ( 64 MB: RAM Física Contígua)
//   0xF....... -> 0x08000000 + (va & 0x03FFFFFF)  ( 64 MB: Write-Combine espelhada em RAM Física)
//   0xB....... -> 0x0C000000 + (va & 0x01FFFFFF)  ( 32 MB: Pools de sistema / KPCR / ETHREAD)
//   0xD....... -> 0x0E000000 + (va & 0x01FFFFFF)  ( 32 MB: Pilhas das pthreads convidadas)

#include <array>
#include <cstdint>

namespace wasm_mem {

inline constexpr uint32_t kArenaAlign = 65536u;
inline constexpr uint32_t kArenaSize = 256u << 20;  // 256 MB

inline constexpr std::array<uint32_t, 16> kRegionBase = {
    0x00000000u,  // 0x0: User / XBE
    0x00000000u,  // 0x1
    0x00000000u,  // 0x2
    0x00000000u,  // 0x3
    0x00000000u,  // 0x4
    0x00000000u,  // 0x5
    0x00000000u,  // 0x6
    0x00000000u,  // 0x7
    0x08000000u,  // 0x8: Physical contiguous (64 MB)
    0x00000000u,  // 0x9
    0x00000000u,  // 0xA
    0x0C000000u,  // 0xB: System pools (32 MB)
    0x00000000u,  // 0xC
    0x0E000000u,  // 0xD: Thread stacks (32 MB)
    0x00000000u,  // 0xE
    0x08000000u,  // 0xF: Write-Combine (mesma RAM física de 0x8...)
};

inline constexpr std::array<uint32_t, 16> kRegionMask = {
    0x07FFFFFFu,  // 0x0
    0x07FFFFFFu,  // 0x1
    0x07FFFFFFu,  // 0x2
    0x07FFFFFFu,  // 0x3
    0x07FFFFFFu,  // 0x4
    0x07FFFFFFu,  // 0x5
    0x07FFFFFFu,  // 0x6
    0x07FFFFFFu,  // 0x7
    0x03FFFFFFu,  // 0x8
    0x07FFFFFFu,  // 0x9
    0x07FFFFFFu,  // 0xA
    0x01FFFFFFu,  // 0xB
    0x07FFFFFFu,  // 0xC
    0x01FFFFFFu,  // 0xD
    0x07FFFFFFu,  // 0xE
    0x03FFFFFFu,  // 0xF
};

inline constexpr uint32_t FoldGuestVa(uint32_t va) noexcept {
    const uint32_t nibble = va >> 28;
    return kRegionBase[nibble] | (va & kRegionMask[nibble]);
}

inline constexpr bool IsPhysicalMirror(uint32_t va) noexcept {
    const uint32_t nibble = va >> 28;
    return nibble == 0x8u || nibble == 0xFu;
}

}  // namespace wasm_mem
