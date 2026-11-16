#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "common/wasm_memory.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace mem {

namespace {

constexpr uint32_t kWasmUserHigh = 0x08000000u;     // 128 MB dobrado
constexpr uint32_t kWasmSystemSize = 0x02000000u;   // 32 MB dobrado
constexpr uint32_t kWasmStackSize = 0x02000000u;    // 32 MB dobrado

struct Reservation {
    uint32_t size = 0;
    uint32_t protect = 0x04;
    std::vector<uint8_t> commit;  // 1 byte por página de 4 KB
};

struct BlockAlloc {
    uint32_t base = 0;
    uint32_t size = 0;
    bool used = false;
};

class BlockAllocator {
public:
    void Init(uint32_t start, uint32_t limit) {
        start_ = start;
        limit_ = limit;
        blocks_.clear();
        blocks_.push_back(BlockAlloc{start, limit - start, false});
    }

    uint32_t Alloc(uint32_t size, uint32_t align, uint32_t lowest_va = 0, uint32_t highest_va = 0xFFFFFFFFu) {
        size = (size + kPage - 1) & ~(kPage - 1);
        if (size == 0) size = kPage;
        align = std::max(align, kPage);
        if ((align & (align - 1)) != 0) {
            uint32_t p = kPage;
            while (p < align && p < 0x10000000u) p <<= 1;
            align = p;
        }

        for (size_t i = 0; i < blocks_.size(); ++i) {
            BlockAlloc b = blocks_[i];
            if (b.used || b.size < size) continue;

            uint32_t cand = (b.base + align - 1) & ~(align - 1);
            if (cand < lowest_va) {
                cand = (lowest_va + align - 1) & ~(align - 1);
            }
            if (cand < b.base) continue;
            const uint64_t end = uint64_t(cand) + size;
            if (end > uint64_t(b.base) + b.size) continue;
            if (end - 1 > highest_va) continue;

            const uint32_t prefix = cand - b.base;
            const uint32_t suffix = (b.base + b.size) - uint32_t(end);
            blocks_.erase(blocks_.begin() + ptrdiff_t(i));
            if (prefix > 0) {
                blocks_.insert(blocks_.begin() + ptrdiff_t(i++), BlockAlloc{b.base, prefix, false});
            }
            blocks_.insert(blocks_.begin() + ptrdiff_t(i), BlockAlloc{cand, size, true});
            used_bytes_ += size;
            if (suffix > 0) {
                blocks_.insert(blocks_.begin() + ptrdiff_t(i + 1), BlockAlloc{uint32_t(end), suffix, false});
            }
            return cand;
        }
        return 0;
    }

    bool Free(uint32_t va, uint32_t* out_size = nullptr) {
        for (size_t i = 0; i < blocks_.size(); ++i) {
            if (!blocks_[i].used || blocks_[i].base != va) continue;
            if (out_size) *out_size = blocks_[i].size;
            used_bytes_ -= blocks_[i].size;
            blocks_[i].used = false;
            if (i + 1 < blocks_.size() && !blocks_[i + 1].used) {
                blocks_[i].size += blocks_[i + 1].size;
                blocks_.erase(blocks_.begin() + ptrdiff_t(i + 1));
            }
            if (i > 0 && !blocks_[i - 1].used) {
                blocks_[i - 1].size += blocks_[i].size;
                blocks_.erase(blocks_.begin() + ptrdiff_t(i));
            }
            return true;
        }
        return false;
    }

    uint32_t SizeOf(uint32_t va) const {
        for (const auto& b : blocks_) {
            if (b.used && va >= b.base && va < b.base + b.size) {
                return b.size - (va - b.base);
            }
        }
        return 0;
    }

    uint32_t Used() const { return used_bytes_; }

private:
    uint32_t start_ = 0;
    uint32_t limit_ = 0;
    uint32_t used_bytes_ = 0;
    std::vector<BlockAlloc> blocks_;
};

uint8_t* g_base = nullptr;
std::recursive_mutex g_lock;
std::map<uint32_t, Reservation> g_reserved;
BlockAllocator g_contig;
BlockAllocator g_system;
BlockAllocator g_stacks;
std::map<uint32_t, uint32_t> g_stack_tops;

// Bitmask de páginas comprometidas sobre a arena dobrada de 256 MB (65536 páginas * 4 KB).
constexpr uint32_t kArenaPages = wasm_mem::kArenaSize / kPage;
std::array<uint64_t, kArenaPages / 64> g_page_commit{};

void SetCommittedPages(uint32_t va, uint32_t size, bool committed) {
    if (!size) return;
    const uint32_t first = va & ~(kPage - 1u);
    const uint64_t last = (uint64_t(va) + size + kPage - 1u) & ~uint64_t(kPage - 1u);
    for (uint64_t cur = first; cur < last; cur += kPage) {
        const uint32_t foff = wasm_mem::FoldGuestVa(static_cast<uint32_t>(cur));
        const uint32_t pidx = foff >> 12;
        if (pidx < kArenaPages) {
            const uint64_t bit = uint64_t(1) << (pidx & 63u);
            if (committed) g_page_commit[pidx >> 6] |= bit;
            else g_page_commit[pidx >> 6] &= ~bit;
        }
    }
}

bool InPhysical(uint32_t va) {
    const uint32_t top = va >> 28;
    return top == 0x8u || top == 0xFu;
}

std::map<uint32_t, Reservation>::iterator FindReservation(uint32_t va) {
    auto it = g_reserved.upper_bound(va);
    if (it == g_reserved.begin()) return g_reserved.end();
    --it;
    if (va >= it->first && va < it->first + it->second.size) return it;
    return g_reserved.end();
}

uint32_t FindFreeUserRange(uint32_t size, bool top_down) {
    if (size > kWasmUserHigh - kUserLow) return 0;
    if (!top_down) {
        uint32_t cur = kUserLow;
        for (const auto& [base, r] : g_reserved) {
            if (cur + size <= base) return cur;
            cur = std::max(cur, base + r.size);
            cur = (cur + 0xFFFFu) & ~0xFFFFu;
        }
        if (cur + size <= kWasmUserHigh) return cur;
        return 0;
    }

    uint32_t hi = kWasmUserHigh;
    for (auto it = g_reserved.rbegin(); it != g_reserved.rend(); ++it) {
        if (hi >= it->first + it->second.size + size) {
            const uint32_t cand = (hi - size) & ~0xFFFFu;
            if (cand >= it->first + it->second.size) return cand;
        }
        hi = std::min(hi, it->first);
    }
    if (hi >= kUserLow + size) {
        const uint32_t cand = (hi - size) & ~0xFFFFu;
        if (cand >= kUserLow) return cand;
    }
    return 0;
}

}  // namespace

bool Init() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_base) return true;

    void* ptr = nullptr;
#if defined(_ISOC11_SOURCE) || defined(__EMSCRIPTEN__)
    ptr = std::aligned_alloc(wasm_mem::kArenaAlign, wasm_mem::kArenaSize);
#endif
    if (!ptr) {
        LOGE("memory_wasm: failed to allocate %u MB guest arena", wasm_mem::kArenaSize >> 20);
        return false;
    }
    g_base = static_cast<uint8_t*>(ptr);
    std::memset(g_base, 0, wasm_mem::kArenaSize);
    g_page_commit.fill(0);

    g_contig.Init(kContiguousBase + kKernelDataSize, kContiguousBase + kPhysicalSize);
    g_system.Init(kSystemBase, kSystemBase + kWasmSystemSize);
    g_stacks.Init(kStackBase, kStackBase + kWasmStackSize);

    SetCommittedPages(kKernelDataBase, kKernelDataSize, true);
    LOGI("memory_wasm: 256 MB folded guest arena ready at %p", static_cast<void*>(g_base));
    return true;
}

uint8_t* Base() { return g_base; }

bool Commit(uint32_t va, uint32_t size) {
    if (!size) return true;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SetCommittedPages(va, size, true);
    return true;
}

void Decommit(uint32_t va, uint32_t size) {
    if (!size) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    SetCommittedPages(va, size, false);
}

bool IsCommitted(uint32_t va) {
    if (!g_base) return false;
    const uint32_t foff = wasm_mem::FoldGuestVa(va);
    const uint32_t pidx = foff >> 12;
    if (pidx >= kArenaPages) return false;
    return (g_page_commit[pidx >> 6] & (uint64_t(1) << (pidx & 63u))) != 0;
}

void ReserveImage(uint32_t base, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t first = base & ~(kPage - 1u);
    const uint32_t aligned = ((base + size + kPage - 1u) & ~(kPage - 1u)) - first;
    Reservation r;
    r.size = aligned;
    r.protect = 0x40;
    r.commit.assign(aligned / kPage, 1);
    g_reserved[first] = std::move(r);
    SetCommittedPages(first, aligned, true);
    if (g_base) {
        std::memset(g_base + wasm_mem::FoldGuestVa(first), 0, aligned);
    }
}

uint32_t AllocateVirtual(uint32_t& base, uint32_t& size, uint32_t type, uint32_t protect) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!size) return kernel::kInvalidParameter;
    if (!(type & (kMemReserve | kMemCommit))) return kernel::kInvalidParameter;

    if (base == 0) {
        const uint32_t aligned = (size + kPage - 1u) & ~(kPage - 1u);
        const uint32_t va = FindFreeUserRange(aligned, (type & kMemTopDown) != 0);
        if (!va) return kernel::kNoMemory;
        Reservation r;
        r.size = aligned;
        r.protect = protect;
        r.commit.assign(aligned / kPage, (type & kMemCommit) ? 1 : 0);
        g_reserved[va] = std::move(r);
        if (type & kMemCommit) {
            SetCommittedPages(va, aligned, true);
            if (!(type & kMemNoZero)) {
                std::memset(g_base + wasm_mem::FoldGuestVa(va), 0, aligned);
            }
        }
        base = va;
        size = aligned;
        return kernel::kSuccess;
    }

    const uint32_t first = base & ~(kPage - 1u);
    const uint32_t end = (base + size + kPage - 1u) & ~(kPage - 1u);
    const uint32_t aligned = end - first;
    auto it = FindReservation(first);
    if (it == g_reserved.end()) {
        if (!(type & kMemReserve)) return kernel::kInvalidParameter;
        if (first < kUserLow || end > kWasmUserHigh) return kernel::kInvalidParameter;
        Reservation r;
        r.size = aligned;
        r.protect = protect;
        r.commit.assign(aligned / kPage, (type & kMemCommit) ? 1 : 0);
        g_reserved[first] = std::move(r);
        if (type & kMemCommit) {
            SetCommittedPages(first, aligned, true);
            if (!(type & kMemNoZero)) {
                std::memset(g_base + wasm_mem::FoldGuestVa(first), 0, aligned);
            }
        }
        base = first;
        size = aligned;
        return kernel::kSuccess;
    }

    if (end > it->first + it->second.size) return kernel::kInvalidParameter;
    if (type & kMemCommit) {
        const uint32_t p0 = (first - it->first) / kPage;
        const uint32_t pn = aligned / kPage;
        for (uint32_t i = 0; i < pn; ++i) {
            if (!it->second.commit[p0 + i]) {
                it->second.commit[p0 + i] = 1;
                const uint32_t pva = first + i * kPage;
                SetCommittedPages(pva, kPage, true);
                if (!(type & kMemNoZero)) {
                    std::memset(g_base + wasm_mem::FoldGuestVa(pva), 0, kPage);
                }
            }
        }
    }
    base = first;
    size = aligned;
    return kernel::kSuccess;
}

uint32_t FreeVirtual(uint32_t& base, uint32_t& size, uint32_t type) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = FindReservation(base);
    if (it == g_reserved.end()) return kernel::kInvalidParameter;

    if (type & kMemRelease) {
        const uint32_t rbase = it->first;
        const uint32_t rsize = it->second.size;
        SetCommittedPages(rbase, rsize, false);
        g_reserved.erase(it);
        base = rbase;
        size = rsize;
        return kernel::kSuccess;
    }
    if (type & kMemDecommit) {
        const uint32_t first = base & ~(kPage - 1u);
        const uint32_t aligned = size ? (((base + size + kPage - 1u) & ~(kPage - 1u)) - first)
                                      : (it->first + it->second.size - first);
        const uint32_t p0 = (first - it->first) / kPage;
        const uint32_t pn = std::min<uint32_t>(aligned / kPage, uint32_t(it->second.commit.size()) - p0);
        for (uint32_t i = 0; i < pn; ++i) {
            it->second.commit[p0 + i] = 0;
        }
        SetCommittedPages(first, pn * kPage, false);
        base = first;
        size = pn * kPage;
        return kernel::kSuccess;
    }
    return kernel::kInvalidParameter;
}

bool QueryVirtual(uint32_t addr, RegionInfo& out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = FindReservation(addr);
    if (it == g_reserved.end()) return false;
    out.base = addr & ~(kPage - 1u);
    out.alloc_base = it->first;
    out.alloc_protect = it->second.protect;
    const uint32_t p0 = (out.base - it->first) / kPage;
    const uint8_t c = it->second.commit[p0];
    uint32_t n = 1;
    while (p0 + n < it->second.commit.size() && it->second.commit[p0 + n] == c) ++n;
    out.size = n * kPage;
    out.state = c ? kMemCommit : kMemReserve;
    out.protect = c ? it->second.protect : 0x01;
    out.type = 0x20000;  // MEM_PRIVATE
    return true;
}

uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t aligned = (size + kPage - 1u) & ~(kPage - 1u);
    const uint32_t lo_va = kContiguousBase + std::max(lowest & ~(kPage - 1u), kKernelDataSize);
    const uint32_t hi_va =
        highest < kPhysicalSize ? (kContiguousBase + highest) : (kContiguousBase + kPhysicalSize - 1u);
    const uint32_t va = g_contig.Alloc(aligned, align, lo_va, hi_va);
    if (!va) return 0;
    SetCommittedPages(va, aligned, true);
    std::memset(g_base + wasm_mem::FoldGuestVa(va), 0, aligned);
    return va;
}

void FreeContiguous(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t size = 0;
    if (g_contig.Free(va, &size)) {
        SetCommittedPages(va, size, false);
    }
}

uint32_t AllocSystem(uint32_t size, uint32_t align) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t aligned = (size + kPage - 1u) & ~(kPage - 1u);
    const uint32_t va = g_system.Alloc(aligned, align);
    if (!va) return 0;
    SetCommittedPages(va, aligned, true);
    std::memset(g_base + wasm_mem::FoldGuestVa(va), 0, aligned);
    return va;
}

void FreeSystem(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t size = 0;
    if (g_system.Free(va, &size)) {
        SetCommittedPages(va, size, false);
    }
}

uint32_t AllocStack(uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t aligned = ((size + kPage - 1u) & ~(kPage - 1u)) + kPage;  // +1 página de guarda
    const uint32_t base = g_stacks.Alloc(aligned, kPage);
    if (!base) return 0;
    SetCommittedPages(base + kPage, aligned - kPage, true);
    std::memset(g_base + wasm_mem::FoldGuestVa(base + kPage), 0, aligned - kPage);
    const uint32_t top = base + aligned;
    g_stack_tops[top] = base;
    return top;
}

void FreeStack(uint32_t top) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_stack_tops.find(top);
    if (it == g_stack_tops.end()) return;
    uint32_t size = 0;
    if (g_stacks.Free(it->second, &size)) {
        SetCommittedPages(it->second, size, false);
    }
    g_stack_tops.erase(it);
}

uint32_t AllocationSize(uint32_t va) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (const uint32_t s = g_contig.SizeOf(va)) return s;
    if (const uint32_t s = g_system.SizeOf(va)) return s;
    auto it = FindReservation(va);
    if (it != g_reserved.end()) return it->second.size - (va - it->first);
    return 0;
}

uint32_t PhysicalFromVirtual(uint32_t va) {
    if (InPhysical(va)) return va & (kPhysicalSize - 1u);
    return va;
}

uint32_t VirtualFromPhysical(uint32_t pa) {
    if (pa < kPhysicalSize) return kContiguousBase | pa;
    return pa;
}

Stats Query() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Stats s{};
    s.total_pages = kPhysicalSize / kPage;
    uint64_t vc = 0;
    for (const auto& [a, r] : g_reserved) {
        (void)a;
        for (uint8_t c : r.commit) vc += c;
    }
    s.virtual_committed = uint32_t(vc);
    s.contiguous_used = uint32_t(g_contig.Used() / kPage);
    s.system_used = uint32_t(g_system.Used() / kPage);
    const uint64_t used = vc + s.contiguous_used + s.system_used;
    s.available_pages = used >= s.total_pages ? 0 : uint32_t(s.total_pages - used);
    return s;
}

}  // namespace mem
