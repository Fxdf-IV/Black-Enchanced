#pragma once
// Fila de comandos de renderização entre a thread do jogo (sdk/runtime/gpu/backend.h)
// e a thread principal da página no navegador (requestAnimationFrame -> WebGPU).
//
// Por que existe: no navegador, o contexto WebGPU do canvas é apresentado no loop de animação
// da thread principal, enquanto o motor de BLACK desenha a partir da sua própria thread.
// Segue o padrão validado pelo cricket07-browser: a thread do jogo grava comandos de alto nível
// (Init, Clear, Draw, Copy, Present, SetGamma) num quadro, e a thread da página consome o quadro fechado.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace gpu_stream {

enum class OpCode : uint8_t {
    Init = 1,
    Clear = 2,
    Draw = 3,
    Copy = 4,
    Present = 5,
    SetGamma = 6,
    Flush = 7,
};

struct Command {
    OpCode op = OpCode::Flush;
    uint32_t a0 = 0;  // Init: width | Clear: flags | Draw: prim | Copy: count | Present: front_va
    uint32_t a1 = 0;  // Init: height | Clear: color | Draw: start | Copy: src_va
    uint32_t a2 = 0;  // Clear: stencil | Draw: count | Copy: dst_va
    float f0 = 0.0f;  // Clear: z | Copy: src_kx
    float f1 = 0.0f;  // Copy: src_ky
    std::vector<uint8_t> inline_bytes;  // índices 16-bit, retângulos de Clear ou rampa de gama (768 B)
};

class CommandRecorder {
public:
    void record_init(uint32_t width, uint32_t height) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::Init;
        cmd.a0 = width;
        cmd.a1 = height;
        staging_.push_back(std::move(cmd));
    }

    void record_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint8_t* rects,
                      std::size_t rect_bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::Clear;
        cmd.a0 = flags;
        cmd.a1 = color;
        cmd.a2 = stencil;
        cmd.f0 = z;
        if (rects && rect_bytes > 0) {
            cmd.inline_bytes.assign(rects, rects + rect_bytes);
        }
        staging_.push_back(std::move(cmd));
    }

    void record_draw(uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices, std::size_t index_bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::Draw;
        cmd.a0 = prim;
        cmd.a1 = start;
        cmd.a2 = count;
        if (indices && index_bytes > 0) {
            cmd.inline_bytes.assign(indices, indices + index_bytes);
        }
        staging_.push_back(std::move(cmd));
    }

    void record_present(uint32_t front_surface_va) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::Present;
        cmd.a0 = front_surface_va;
        staging_.push_back(std::move(cmd));
        completed_frames_.push_back(std::move(staging_));
        staging_.clear();
        ++submitted_frame_id_;
    }

    void record_gamma(const uint8_t ramp[768]) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::SetGamma;
        if (ramp) {
            cmd.inline_bytes.assign(ramp, ramp + 768);
        }
        staging_.push_back(std::move(cmd));
    }

    // Chamado pela main thread do navegador (requestAnimationFrame) para retirar o próximo quadro pronto.
    bool pop_completed_frame(std::vector<Command>& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (completed_frames_.empty()) return false;
        out = std::move(completed_frames_.front());
        completed_frames_.erase(completed_frames_.begin());
        return true;
    }

    uint64_t submitted_frames() const {
        std::lock_guard<std::mutex> lock(mu_);
        return submitted_frame_id_;
    }

private:
    mutable std::mutex mu_;
    std::vector<Command> staging_;
    std::vector<std::vector<Command>> completed_frames_;
    uint64_t submitted_frame_id_ = 0;
};

}  // namespace gpu_stream
