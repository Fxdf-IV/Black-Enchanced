// Portable pthreaded render backend queue for WebAssembly, matching sdk/runtime/gpu/render_thread.cpp.
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/simple_dirty.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#if __has_include("image_symbols.h")
#include "image_symbols.h"
#endif

namespace gpu {

namespace {

constexpr uint32_t kBatchCommands = 64;
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };
enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0, g = 0;
    uint32_t data = 0, size = 0;
    int32_t vs = -1, ps = -1;
    Surface s1, s2;
};

struct Batch {
    uint64_t id = 0;
    std::vector<Cmd> cmds;
    std::vector<uint8_t> bytes;
    std::vector<std::shared_ptr<VertexShader>> vs;
    std::vector<std::shared_ptr<PixelShader>> ps;
};

struct Range {
    size_t off, len;
};

size_t At(const State& s, const void* p) {
    return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s));
}

std::vector<Range> StateRanges() {
    static const State s;
    return {
        {At(s, &s.rs), At(s, &s.textures) - At(s, &s.rs)},
        {At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)},
        {At(s, &s.vsc), At(s, &s.rt) - At(s, &s.vsc)},
        {At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)},
        {At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[256]), sizeof(s.transforms[0])},
        {At(s, &s.material), sizeof(State) - At(s, &s.material)},
    };
}

bool Same(const uint8_t* a, const uint8_t* b, size_t n) { return n == 0 || std::memcmp(a, b, n) == 0; }

using Clock = std::chrono::steady_clock;
uint64_t Ns(Clock::time_point t) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
}

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner) : inner_(std::move(inner)), ranges_(StateRanges()) {
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        cur_ = NewBatch();

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 4u << 20);
        pthread_t tid{};
        const int rc = pthread_create(&tid, &attr, &ThreadEntry, this);
        pthread_attr_destroy(&attr);
        if (rc != 0) {
            LOGE("render_thread_wasm: pthread_create failed (%d)", rc);
        }
    }

    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(m_);
        if (!done_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; })) {
            (void)inner_.release();
        }
    }

    bool Init(uint32_t width, uint32_t height) override { return inner_->Init(width, height); }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        auto t = Clock::now();
        SendState(s);
        Cmd c{Op::Draw};
        c.a = prim;
        c.b = start;
        c.c = count;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        cur_->cmds.push_back(c);
        for (uint32_t tex : s.textures) Use(tex);
        for (size_t i = 0; i < 4; i++) {
            uint32_t pal = s.palettes[i];
            if (!pal) pal = GuestStagePalette(i);
            Use(pal);
        }
        for (const auto& st : s.streams) Use(st.vb);
        Use(s.rt.header);
        Use(s.zs.header);
        Written(s.rt);
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        g_perf_capture_ns += Ns(t);
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        auto t = Clock::now();
        SendState(s);
        Cmd c{Op::Clear};
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        g_perf_capture_ns += Ns(t);
    }

    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points,
              float src_kx, float src_ky) override {
        auto t = Clock::now();
        Cmd c{Op::Copy};
        c.a = count;
        c.f = src_kx;
        c.g = src_ky;
        c.b = count && src_rects ? Scratch(kernel::HostPtr(src_rects), count * 16) : 0;
        c.c = count && dst_points ? Scratch(kernel::HostPtr(dst_points), count * 8) : 0;
        c.s1 = src;
        c.s1.header = Header(src.header);
        c.s2 = dst;
        c.s2.header = Header(dst.header);
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
        g_perf_capture_ns += Ns(t);
    }

    void Present(const Surface& front) override {
        auto t = Clock::now();
        Cmd c{Op::Present};
        c.s1 = front;
        c.s1.header = Header(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        uint64_t id = Submit();
        g_perf_capture_ns += Ns(t);
        Wait(last_present_, WaitFor::Frame, nullptr);
        if (presents_) inner_->WaitGpuFrame(presents_ - 1);
        presents_++;
        last_present_ = id;
    }

    void Sync(uint32_t data, uint32_t) override { ReadBack(data, false); }

    void SyncLate(uint32_t data) override {
        if (!inner_->DeliverLate(data)) {
            if (diag::Perf())
                LOG_FIRST(16, "render thread: no finished copy of %08X for a NOOVERWRITE lock, reading it back now",
                          data);
            written_.insert(data);
            ReadBack(data, true);
        }
        Cmd c{Op::RecordLate};
        c.a = data;
        cur_->cmds.push_back(c);
    }

    void Flush() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Wait(Submit(), WaitFor::Flush, "flush");
    }

    void SetGamma(const uint8_t* ramp) override {
        Cmd c{Op::Gamma};
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }

    void GetReport(uint32_t report) override {
        Cmd c{Op::GetReport};
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() override { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) override { return fence > done_.load(); }
    void WaitFence(uint32_t fence) override { Wait(fence, WaitFor::Fence, "fence"); }
    bool Busy(uint32_t resource) override {
        auto it = last_use_.find(resource);
        return it != last_use_.end() && it->second > done_.load();
    }
    void WaitResource(uint32_t resource) override {
        auto it = last_use_.find(resource);
        if (it != last_use_.end()) Wait(it->second, WaitFor::Resource, "resource");
    }

private:
    static void* ThreadEntry(void* arg) {
        static_cast<Threaded*>(arg)->Run();
        return nullptr;
    }

    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.resize(at + n);
        std::memcpy(cur_->bytes.data() + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data, bool late) {
        if (!written_.erase(data)) return;
        (late ? g_perf_sync_late : g_perf_sync_block)++;
        if (diag::Perf() && logged_.insert(uint64_t(data) << 1 | (late ? 1 : 0)).second)
            LOGI("render thread: read-back of %08X for %s", data,
                 late ? "a NOOVERWRITE lock without a finished copy" : "a lock / BlockOnResource");
        Cmd c{Op::Sync};
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::ReadBack, "read-back");
    }

    uint32_t Scratch(const void* p, uint32_t n) {
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        std::memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    uint32_t Header(uint32_t header) {
        if (!header || !ring_) return header;
        const uint32_t type = kernel::R32(header) & 0x70000;
        uint8_t now[kHeaderBytes] = {};
        std::memcpy(now, kernel::HostPtr(header), type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes);
        Recent<HeaderCopy>& r = recent_headers_[(header >> 3) & 63];
        HeaderCopy& c = r.key == header ? *r.value : headers_[header];
        r = {header, &c};
        if (c.made && !std::memcmp(c.bytes, now, kHeaderBytes) && ring_pos_ - c.made < kHeaderRing / 2) return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        std::memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        if (Same(sent, static_cast<const uint8_t*>(value), n)) return;
        std::memcpy(sent, value, n);
        uint32_t head[2] = {uint32_t(At(sent_, field)), uint32_t(n)};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + n);
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, value, n);
    }

    void DeltaRange(const Range& r, const uint8_t* now) {
        for (size_t off = r.off, end = r.off + r.len; off < end; off += 64)
            Delta(reinterpret_cast<const uint8_t*>(&sent_) + off, now + off, std::min<size_t>(64, end - off));
    }

    void SendSimple(const State& s) {
        static_assert(sizeof(State::simple) == sizeof(g_simple_dirty) * 8 * kSimplePiece * sizeof(uint32_t));
        if (!g_simple_tracked) {
            DeltaRange({At(s, &s.simple), sizeof(s.simple)}, reinterpret_cast<const uint8_t*>(&s));
            return;
        }
        for (size_t w = 0; w < g_simple_dirty.size(); w++) {
            for (uint64_t bits = g_simple_dirty[w]; bits; bits &= bits - 1) {
                size_t first = (w * 64 + size_t(std::countr_zero(bits))) * kSimplePiece;
                Delta(&sent_.simple[first], &s.simple[first], kSimplePiece * sizeof(uint32_t));
            }
            g_simple_dirty[w] = 0;
        }
    }

    static uint32_t GuestStagePalette(size_t stage) {
#if defined(HAS_D3D_pDevice) && HAS_D3D_pDevice
#define RT_HEX_PASTE_(x) 0x##x##u
#define RT_HEX_PASTE(x) RT_HEX_PASTE_(x)
        const uint32_t dev_va = kernel::R32(RT_HEX_PASTE(SYM_D3D_pDevice));
#undef RT_HEX_PASTE
#undef RT_HEX_PASTE_
        if (dev_va) return kernel::R32(dev_va + XDK_CDevice_Palettes + uint32_t(stage) * 4u);
#endif
        (void)stage;
        return 0u;
    }

    void SendState(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        Cmd c{Op::State};
        size_t first = cur_->bytes.size();
        SendSimple(s);
        for (const Range& r : ranges_) DeltaRange(r, now);
        std::array<uint32_t, 4> textures;
        for (size_t i = 0; i < 4; i++) textures[i] = Header(s.textures[i]);
        Delta(&sent_.textures, &textures, sizeof(textures));
        std::array<uint32_t, 4> palettes;
        for (size_t i = 0; i < 4; i++) {
            uint32_t pal = s.palettes[i];
            if (!pal) pal = GuestStagePalette(i);
            palettes[i] = Header(pal);
        }
        Delta(&sent_.palettes, &palettes, sizeof(palettes));
        auto streams = s.streams;
        for (auto& st : streams) st.vb = Header(st.vb);
        Delta(&sent_.streams, &streams, sizeof(streams));
        Surface rt = s.rt, zs = s.zs;
        rt.header = Header(rt.header);
        zs.header = Header(zs.header);
        Delta(&sent_.rt, &rt, sizeof(rt));
        Delta(&sent_.zs, &zs, sizeof(zs));
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            c.vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            c.ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        if (cur_->bytes.size() == first && c.vs < 0 && c.ps < 0) return;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    void Use(uint32_t header) {
        if (!header) return;
        Recent<uint64_t>& r = recent_uses_[(header >> 3) & 63];
        if (r.key != header) r = {header, &last_use_[header]};
        *r.value = cur_->id;
    }

    void Written(const Surface& s) {
        if (s.header) written_.insert(mem::VirtualFromPhysical(s.data));
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id, WaitFor kind, const char* why) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        auto t = Clock::now();
        {
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return done_.load() >= id; });
        }
        uint64_t ns = Ns(t);
        g_perf_wait_ns += ns;
        g_perf_wait_kind_ns[size_t(kind)] += ns;
        if (why) {
            g_perf_waits++;
            if (diag::Perf()) LOG_FIRST(32, "render thread: the title waited %.2f ms for a %s", ns / 1e6, why);
        }
    }

    void Run() {
        for (;;) {
            Batch* b;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            for (const Cmd& c : b->cmds) Execute(*b, c);
            uint64_t id = b->id;
            b->cmds.clear();
            b->bytes.clear();
            b->vs.clear();
            b->ps.clear();
            {
                std::lock_guard lock(m_);
                free_.push_back(b);
                done_.store(id);
            }
            done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        switch (c.op) {
            case Op::State: {
                uint8_t* s = reinterpret_cast<uint8_t*>(&state_);
                for (const uint8_t* p = data; p < data + c.size;) {
                    uint32_t head[2];
                    std::memcpy(head, p, 8);
                    std::memcpy(s + head[0], p + 8, head[1]);
                    p += 8 + head[1];
                }
                if (c.vs >= 0) state_.vs = b.vs[size_t(c.vs)];
                if (c.ps >= 0) state_.ps = b.ps[size_t(c.ps)];
                break;
            }
            case Op::Draw: inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr); break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: inner_->Copy(c.s1, c.s2, c.a, c.b, c.c, c.f, c.g); break;
            case Op::Present: inner_->Present(c.s1); break;
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
        }
    }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;

    struct HeaderCopy {
        uint32_t at = 0;
        uint64_t made = 0;
        uint8_t bytes[kHeaderBytes];
    };
    std::unordered_map<uint32_t, HeaderCopy> headers_;
    template <typename T>
    struct Recent {
        uint32_t key = 0;
        T* value = nullptr;
    };
    std::array<Recent<HeaderCopy>, 64> recent_headers_{};
    std::array<Recent<uint64_t>, 64> recent_uses_{};
    uint32_t ring_ = 0;
    uint64_t ring_pos_ = 0;
    Batch* cur_ = nullptr;
    State sent_;
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;
    std::unordered_set<uint64_t> logged_;
    uint64_t next_id_ = 0, last_present_ = 0;
    uint64_t presents_ = 0;

    State state_;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_{0};
    bool stop_ = false, exited_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) {
    return std::make_unique<Threaded>(std::move(inner));
}

}  // namespace gpu
