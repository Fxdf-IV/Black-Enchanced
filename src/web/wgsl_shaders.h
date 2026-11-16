#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/shaders.h"
#include "gpu/state.h"

namespace gpu_webgpu {

// Extended 4-bit-per-register decode value for B8G8R8A8_UNORM D3DCOLOR bound as WebGPU unorm8x4.
inline constexpr uint8_t kWebDecodeBgra = 7;

std::string VertexProgramWgsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode, bool sprite,
                              bool quad);

std::string FixedVertexWgsl(uint16_t regs, bool pretransformed, uint64_t decode, const gpu::FixedKey& f, bool quad);

std::string PixelWgsl(const gpu::PixelKey& k);

// Fullscreen textured quad with optional 256x1 gamma LUT.
std::string PresentBlitWgsl();

// Sub-channel or partial-rect clear shader.
std::string ClearQuadWgsl();

// Fullscreen depth copy (r32float -> frag_depth) for CPU-written depth textures.
std::string DepthUploadWgsl();

}  // namespace gpu_webgpu
