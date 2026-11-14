#pragma once
// Implementação da interface gpu::Backend (sdk/runtime/gpu/backend.h) voltada para WebGPU.
// Registra chamadas de desenho e apresentação em CommandRecorder para consumo na thread principal.

#include <cstdint>
#include <memory>

#include "common/gpu_command_stream.h"
#include "gpu/backend.h"

namespace gpu_webgpu {

class WebGpuStreamBackend final : public gpu::Backend {
public:
    explicit WebGpuStreamBackend(gpu_stream::CommandRecorder& recorder) : recorder_(recorder) {}

    bool Init(uint32_t width, uint32_t height) override {
        width_ = width;
        height_ = height;
        recorder_.record_init(width, height);
        return true;
    }

    void Clear(const gpu::State& /*s*/, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        const std::size_t rect_bytes = rects ? static_cast<std::size_t>(count) * 16u : 0u;
        recorder_.record_clear(flags, color, z, stencil, rects, rect_bytes);
    }

    void Draw(const gpu::State& /*s*/, uint32_t prim, uint32_t start, uint32_t count,
              const uint8_t* indices) override {
        const std::size_t idx_bytes = indices ? static_cast<std::size_t>(count) * 2u : 0u;
        recorder_.record_draw(prim, start, count, indices, idx_bytes);
    }

    void Copy(const gpu::Surface& /*src*/, const gpu::Surface& /*dst*/, uint32_t /*count*/, uint32_t /*src_rects_va*/,
              uint32_t /*dst_points_va*/, float /*src_kx*/, float /*src_ky*/) override {
        // Transferências de superfície ficam encapsuladas até a vinculação completa de WGSL textures
    }

    void Present(const gpu::Surface& front) override { recorder_.record_present(front.data); }

    void Sync(uint32_t /*data_va*/, uint32_t /*size*/) override {}
    void SyncLate(uint32_t /*data_va*/) override {}
    void Flush() override {}

    void SetGamma(const uint8_t* ramp) override { recorder_.record_gamma(ramp); }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    gpu_stream::CommandRecorder& recorder_;
    uint32_t width_ = 640;
    uint32_t height_ = 480;
};

}  // namespace gpu_webgpu
