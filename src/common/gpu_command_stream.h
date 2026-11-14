#pragma once
// Prototipo (Fase 6 - Trilha de Pesquisa) de fila de comandos entre a thread do jogo
// (sdk/runtime/gpu/backend.h) e a thread principal no navegador (WebGPU).
//
// ATENCAO SOBRE READ-BACK E SINCRONIZACAO EM WEBGPU:
// Este gravador cobre comandos de desenho e estende os opcodes de sincronizacao/consulta
// (Sync, RecordLate, ClearReport, GetReport, Fence). Porem, no WebGPU, leituras GPU -> CPU
// (buffer.mapAsync) e espera por conclusao de fila (onSubmittedWorkDone) sao ASSINCRONAS por
// definicao. Portanto, Sync, SyncLate/DeliverLate, GetReport e WaitFence nao podem apenas ser
// gravados nesta fila esperando leitura sincrona imediata na main thread: a implementacao real
// da Fase 6 precisara de bufferizacao adiada e sincronizacao entre worker/main thread.

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
    Sync = 8,
    RecordLate = 9,
    ClearReport = 10,
    GetReport = 11,
    Fence = 12,
};

struct Command {
    OpCode op = OpCode::Flush;
    uint32_t a0 = 0;  // Init: width | Clear: flags | Draw: prim | Present: front_va | Sync/RecordLate/GetReport: data_va | Fence: fence_id
    uint32_t a1 = 0;  // Init: height | Clear: color | Draw: start | Sync: size
    uint32_t a2 = 0;  // Clear: stencil | Draw: count
    float f0 = 0.0f;  // Clear: z | Copy: src_kx
    float f1 = 0.0f;  // Copy: src_ky
    std::vector<uint8_t> inline_bytes;  // indices 16-bit, retangulos de Clear ou rampa de gama (768 B)
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

    // Read-back sincrono/adiado, testes de visibilidade e cerca da interface gpu::Backend.
    void record_sync(uint32_t data_va, uint32_t size) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::Sync;
        cmd.a0 = data_va;
        cmd.a1 = size;
        staging_.push_back(std::move(cmd));
    }

    void record_late(uint32_t data_va) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::RecordLate;
        cmd.a0 = data_va;
        staging_.push_back(std::move(cmd));
    }

    void record_clear_report() {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::ClearReport;
        staging_.push_back(std::move(cmd));
    }

    void record_get_report(uint32_t report_va) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::GetReport;
        cmd.a0 = report_va;
        staging_.push_back(std::move(cmd));
    }

    void record_fence(uint32_t fence_id) {
        std::lock_guard<std::mutex> lock(mu_);
        Command cmd{};
        cmd.op = OpCode::Fence;
        cmd.a0 = fence_id;
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
