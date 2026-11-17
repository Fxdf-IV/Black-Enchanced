#include "gpu/backend.h"

#include <emscripten.h>
#include <emscripten/threading_primitives.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "dxgiformat.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "web/web_stats.h"
#include "web/wgsl_shaders.h"
#if __has_include("image_symbols.h")
#include "image_symbols.h"
#endif

namespace gpu {

namespace {

static_assert(sizeof(DrawConstants) == 528 * sizeof(float), "DrawConstants WGSL layout mismatch");

// Command opcodes mirrored in web/webgpu_renderer.js
enum : uint32_t {
    kOpCreateShader = 1,
    kOpCreateSampler = 2,
    kOpCreateTexture = 3,
    kOpUploadTexture = 4,
    kOpUploadGamma = 5,
    kOpDestroyTexture = 6,
    kOpCreatePipeline = 7,
    kOpClear = 8,
    kOpDraw = 9,
    kOpCopyTex = 10,
    kOpPresent = 11,
    kOpUploadDepth = 12,
};

// WebGPU texture formats encoded for OP_CREATE_TEXTURE
enum : uint32_t {
    kWgpuTexBgra8 = 0,
    kWgpuTexBc1 = 1,
    kWgpuTexBc2 = 2,
    kWgpuTexBc3 = 3,
    kWgpuTexR32f = 4,
    kWgpuTexD24S8 = 5,
};

// Texture view kinds mirrored in OP_CREATE_PIPELINE / OP_DRAW
enum : uint32_t {
    kView2D = 0,
    kView3D = 1,
    kViewCube = 2,
    kViewDepth = 3,
};

// D3D constants matching sdk/runtime/gpu/d3d12.cpp
constexpr uint32_t kFillWireframe = 0x1B01;
constexpr uint32_t kCullCw = 0x900;
constexpr uint32_t kZUseW = 2;
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr float kAlphaTestAlways = 7.0f;
constexpr uint32_t kBlendDestColor = 0x306;
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;
constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;
constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536.0f;
constexpr float kGlowBoxTexels = 3.0f;
constexpr uint32_t kQuadVertices = 6;
constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;
constexpr uint64_t kSpriteSalt = 0x3C3C0000;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void ColorArgb(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint32_t MapBlendFactor(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return 0;       // zero
        case 1: return 1;       // one
        case 0x300: return alpha ? 4 : 2;  // src-alpha / src
        case 0x301: return alpha ? 5 : 3;  // one-minus-src-alpha / one-minus-src
        case 0x302: return 4;   // src-alpha
        case 0x303: return 5;   // one-minus-src-alpha
        case 0x304: return 6;   // dst-alpha
        case 0x305: return 7;   // one-minus-dst-alpha
        case kBlendDestColor: return alpha ? 6 : 8;  // dst-alpha / dst
        case 0x307: return alpha ? 7 : 9;            // one-minus-dst-alpha / one-minus-dst
        case 0x308: return 10;  // src-alpha-saturated
        case 0x8001:
        case 0x8003: return 11; // constant
        case 0x8002:
        case 0x8004: return 12; // one-minus-constant
        default: return 1;
    }
}

uint32_t MapBlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return 1;  // subtract
        case 0x800B: return 2;  // reverse-subtract
        case 0x8007: return 3;  // min
        case 0x8008: return 4;  // max
        default: return 0;      // add
    }
}

uint32_t MapCmp0(uint32_t v) { return (v >= 0x200 && v <= 0x207) ? (v - 0x200) : 7u; }

uint32_t MapReversedCmp0(uint32_t v) {
    static constexpr uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return mirror[MapCmp0(v)];
}

uint32_t MapStencilOp(uint32_t v) {
    switch (v) {
        case 0: return 1;       // zero
        case 0x1E01: return 2;  // replace
        case 0x1E02: return 3;  // increment-clamp
        case 0x1E03: return 4;  // decrement-clamp
        case 0x150A: return 5;  // invert
        case 0x8507: return 6;  // increment-wrap
        case 0x8508: return 7;  // decrement-wrap
        default: return 0;      // keep
    }
}

uint32_t MapAddressMode(uint32_t v) {
    switch (v) {
        case 2: return 1;  // mirror-repeat
        case 3:
        case 4:
        case 5: return 2;  // clamp-to-edge (WebGPU has no border color)
        default: return 0; // repeat
    }
}

uint32_t GuestPitch(uint32_t width) { return (width * 4u + 63u) & ~63u; }
uint8_t ShadowDepthKind(uint32_t format) { return (format == kFmtD16 || format == kFmtLinD16) ? 2 : 1; }

uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) {
    return uint64_t(data) | (uint64_t(depth ? 1u : 0u) << 32) | (uint64_t(gw & 0xFFFu) << 40) |
           (uint64_t(gh & 0xFFFu) << 52);
}

bool SameTexDesc(const TextureDesc& a, const TextureDesc& b) {
    return a.format == b.format && a.width == b.width && a.height == b.height && a.depth == b.depth &&
           a.levels == b.levels && a.pitch == b.pitch && a.cube == b.cube && a.linear == b.linear;
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    if (d.format != 0x0B) return;
    uint32_t header = s.palettes[size_t(stage)];
#if defined(HAS_D3D_pDevice) && HAS_D3D_pDevice
    if (!header) {
#define BLACK_HEX_PASTE_(x) 0x##x##u
#define BLACK_HEX_PASTE(x) BLACK_HEX_PASTE_(x)
        const uint32_t dev_va = kernel::R32(BLACK_HEX_PASTE(SYM_D3D_pDevice));
#undef BLACK_HEX_PASTE
#undef BLACK_HEX_PASTE_
        if (dev_va) header = kernel::R32(dev_va + XDK_CDevice_Palettes + uint32_t(stage) * 4u);
    }
#endif
    if (!header) return;
    const uint32_t raw_data = kernel::R32(header + 4u);
    if (!raw_data) return;
    palette.va = mem::VirtualFromPhysical(raw_data);
    const uint32_t size_code = std::min<uint32_t>(kernel::R32(header) >> 30, 4u);
    const size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    if (const void* ptr = kernel::HostPtr(palette.va)) {
        std::memcpy(palette.colours, ptr, bytes);
    }
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

uint64_t MemSig(uint32_t data, uint32_t total) {
    constexpr uint32_t kSigWholeMax = 65536;
    constexpr uint32_t kSigSamples = 64;
    constexpr uint32_t kSigSampleBytes = 64;
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (!p || !total) return sig;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0.0f ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 1; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!((a.regs >> r) & 1u) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

// Software DXT -> BGRA8 fallback for WebGPU adapters without texture-compression-bc or NPOT block sizes.
void DecodeRgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint32_t r5 = (c >> 11) & 31u;
    uint32_t g6 = (c >> 5) & 63u;
    uint32_t b5 = c & 31u;
    r = uint8_t((r5 << 3) | (r5 >> 2));
    g = uint8_t((g6 << 2) | (g6 >> 4));
    b = uint8_t((b5 << 3) | (b5 >> 2));
}

void DecodeBcBlockColor(const uint8_t* blk, bool four_alpha, uint8_t out_bgra[64]) {
    uint16_t c0 = uint16_t(blk[0]) | (uint16_t(blk[1]) << 8);
    uint16_t c1 = uint16_t(blk[2]) | (uint16_t(blk[3]) << 8);
    uint32_t bits = uint32_t(blk[4]) | (uint32_t(blk[5]) << 8) | (uint32_t(blk[6]) << 16) | (uint32_t(blk[7]) << 24);
    uint8_t pal[4][4]{};
    DecodeRgb565(c0, pal[0][2], pal[0][1], pal[0][0]);
    pal[0][3] = 255;
    DecodeRgb565(c1, pal[1][2], pal[1][1], pal[1][0]);
    pal[1][3] = 255;
    if (c0 > c1 || four_alpha) {
        for (int ch = 0; ch < 3; ch++) {
            pal[2][ch] = uint8_t((2 * pal[0][ch] + pal[1][ch]) / 3);
            pal[3][ch] = uint8_t((pal[0][ch] + 2 * pal[1][ch]) / 3);
        }
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ch++) pal[2][ch] = uint8_t((pal[0][ch] + pal[1][ch]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t code = (bits >> (i * 2)) & 3u;
        out_bgra[i * 4 + 0] = pal[code][0];
        out_bgra[i * 4 + 1] = pal[code][1];
        out_bgra[i * 4 + 2] = pal[code][2];
        out_bgra[i * 4 + 3] = pal[code][3];
    }
}

void DecodeBcToBgra(Host h, uint32_t w, uint32_t h_px, uint32_t depth, const std::vector<uint8_t>& src,
                    uint32_t src_pitch, std::vector<uint8_t>& dst, uint32_t& out_pitch) {
    const uint32_t block_bytes = (h == Host::BC1) ? 8u : 16u;
    const uint32_t bw = std::max(1u, (w + 3u) / 4u);
    const uint32_t bh = std::max(1u, (h_px + 3u) / 4u);
    out_pitch = w * 4u;
    dst.assign(size_t(out_pitch) * h_px * std::max(1u, depth), 0);
    const uint32_t slice_src_rows = bh;

    for (uint32_t z = 0; z < std::max(1u, depth); z++) {
        for (uint32_t by = 0; by < bh; by++) {
            const uint8_t* row = src.data() + size_t(z * slice_src_rows + by) * src_pitch;
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* blk = row + bx * block_bytes;
                uint8_t rgba[64]{};
                if (h == Host::BC1) {
                    DecodeBcBlockColor(blk, false, rgba);
                } else if (h == Host::BC2) {
                    DecodeBcBlockColor(blk + 8, true, rgba);
                    for (int py = 0; py < 4; py++) {
                        uint16_t arow = uint16_t(blk[py * 2]) | (uint16_t(blk[py * 2 + 1]) << 8);
                        for (int px = 0; px < 4; px++) {
                            uint8_t a4 = uint8_t((arow >> (px * 4)) & 0xFu);
                            rgba[(py * 4 + px) * 4 + 3] = uint8_t((a4 << 4) | a4);
                        }
                    }
                } else {
                    DecodeBcBlockColor(blk + 8, true, rgba);
                    uint8_t a0 = blk[0], a1 = blk[1];
                    uint64_t abits = 0;
                    for (int k = 0; k < 6; k++) abits |= uint64_t(blk[2 + k]) << (8 * k);
                    uint8_t apal[8]{a0, a1};
                    if (a0 > a1) {
                        for (int k = 1; k <= 6; k++) apal[1 + k] = uint8_t(((7 - k) * a0 + k * a1) / 7);
                    } else {
                        for (int k = 1; k <= 4; k++) apal[1 + k] = uint8_t(((5 - k) * a0 + k * a1) / 5);
                        apal[6] = 0;
                        apal[7] = 255;
                    }
                    for (int i = 0; i < 16; i++) {
                        rgba[i * 4 + 3] = apal[(abits >> (i * 3)) & 7u];
                    }
                }
                for (uint32_t py = 0; py < 4; py++) {
                    uint32_t y = by * 4 + py;
                    if (y >= h_px) break;
                    uint8_t* dst_row = dst.data() + (size_t(z) * h_px + y) * out_pitch;
                    for (uint32_t px = 0; px < 4; px++) {
                        uint32_t x = bx * 4 + px;
                        if (x >= w) break;
                        std::memcpy(dst_row + x * 4, &rgba[(py * 4 + px) * 4], 4);
                    }
                }
            }
        }
    }
}

uint32_t WgslVertexFmtCode(uint8_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32_FLOAT: return 0;
        case DXGI_FORMAT_R32G32_FLOAT: return 1;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 2;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 3;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
        case DXGI_FORMAT_R16G16_SNORM: return 5;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return 6;
        case DXGI_FORMAT_R8G8_UNORM: return 7;
        case DXGI_FORMAT_R16G16_SINT: return 8;
        case DXGI_FORMAT_R16G16B16A16_SINT: return 9;
        case DXGI_FORMAT_R32_UINT: return 10;
        default: return UINT32_MAX;
    }
}

bool ExpandUnsupportedVertexFormat(const Assembled& a, std::vector<float>& out_verts) {
    uint32_t nregs = 0;
    for (int r = 0; r < 16; r++) nregs += ((a.regs >> r) & 1u);
    if (!nregs || !a.vertex_count) return false;
    out_verts.assign(size_t(a.vertex_count) * nregs * 4u, 0.0f);

    uint32_t slot = 0;
    for (int r = 0; r < 16; r++) {
        if (!((a.regs >> r) & 1u)) continue;
        const VertexElement& e = a.elements[size_t(r)];
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src) {
            slot++;
            continue;
        }
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            const size_t off = size_t(v) * vb.stride + e.offset;
            float dst[4] = {0, 0, 0, 1};
            if (off < vb.bytes) {
                const uint8_t* src = vb.src + off;
                const size_t avail = vb.bytes - off;
                auto read_i16 = [&](size_t idx) -> int16_t {
                    int16_t x = 0;
                    if (idx * 2 + 2 <= avail) std::memcpy(&x, src + idx * 2, 2);
                    return x;
                };
                auto read_f32 = [&](size_t idx) -> float {
                    float x = 0;
                    if (idx * 4 + 4 <= avail) std::memcpy(&x, src + idx * 4, 4);
                    return x;
                };
                switch (e.format) {
                    case DXGI_FORMAT_R32_FLOAT: dst[0] = read_f32(0); break;
                    case DXGI_FORMAT_R32G32_FLOAT:
                        dst[0] = read_f32(0);
                        dst[1] = read_f32(1);
                        break;
                    case DXGI_FORMAT_R32G32B32_FLOAT: {
                        dst[0] = read_f32(0);
                        dst[1] = read_f32(1);
                        uint32_t dec = uint32_t(a.decode >> (r * 4)) & 15u;
                        if (dec == kDecodeFloat2H) {
                            dst[2] = 0.0f;
                            dst[3] = read_f32(2);
                        } else {
                            dst[2] = read_f32(2);
                        }
                        break;
                    }
                    case DXGI_FORMAT_R32G32B32A32_FLOAT:
                        for (int k = 0; k < 4; k++) dst[k] = read_f32(size_t(k));
                        break;
                    case DXGI_FORMAT_B8G8R8A8_UNORM:
                        if (avail >= 4) {
                            dst[0] = src[2] / 255.0f;
                            dst[1] = src[1] / 255.0f;
                            dst[2] = src[0] / 255.0f;
                            dst[3] = src[3] / 255.0f;
                        }
                        break;
                    case DXGI_FORMAT_R8G8B8A8_UNORM:
                        if (avail >= 4) {
                            for (int k = 0; k < 4; k++) dst[k] = src[k] / 255.0f;
                        }
                        break;
                    case DXGI_FORMAT_R8G8_UNORM:
                        if (avail >= 2) {
                            dst[0] = src[0] / 255.0f;
                            dst[1] = src[1] / 255.0f;
                        }
                        break;
                    case DXGI_FORMAT_R8_UNORM:
                        if (avail >= 1) dst[0] = src[0] / 255.0f;
                        break;
                    case DXGI_FORMAT_R16_SNORM:
                        dst[0] = std::max(-1.0f, read_i16(0) / 32767.0f);
                        break;
                    case DXGI_FORMAT_R16G16_SNORM:
                        dst[0] = std::max(-1.0f, read_i16(0) / 32767.0f);
                        dst[1] = std::max(-1.0f, read_i16(1) / 32767.0f);
                        break;
                    case DXGI_FORMAT_R16G16B16A16_SNORM: {
                        for (int k = 0; k < 3; k++) dst[k] = std::max(-1.0f, read_i16(size_t(k)) / 32767.0f);
                        uint32_t dec = uint32_t(a.decode >> (r * 4)) & 15u;
                        dst[3] = (dec == kDecodeW1) ? 1.0f : std::max(-1.0f, read_i16(3) / 32767.0f);
                        break;
                    }
                    case DXGI_FORMAT_R16_SINT: dst[0] = float(read_i16(0)); break;
                    case DXGI_FORMAT_R16G16_SINT:
                        dst[0] = float(read_i16(0));
                        dst[1] = float(read_i16(1));
                        break;
                    case DXGI_FORMAT_R16G16B16A16_SINT: {
                        for (int k = 0; k < 3; k++) dst[k] = float(read_i16(size_t(k)));
                        uint32_t dec = uint32_t(a.decode >> (r * 4)) & 15u;
                        dst[3] = (dec == kDecodeIntW1) ? 1.0f : float(read_i16(3));
                        break;
                    }
                    case DXGI_FORMAT_R32_UINT:
                        if (avail >= 4) {
                            uint32_t u = 0;
                            std::memcpy(&u, src, 4);
                            int32_t sx = int32_t(u << 21) >> 21;
                            int32_t sy = int32_t(u << 10) >> 21;
                            int32_t sz = int32_t(u) >> 22;
                            dst[0] = std::max(-1.0f, sx / 1023.0f);
                            dst[1] = std::max(-1.0f, sy / 1023.0f);
                            dst[2] = std::max(-1.0f, sz / 511.0f);
                            dst[3] = 1.0f;
                        }
                        break;
                    default: break;
                }
            }
            std::memcpy(&out_verts[(size_t(v) * nregs + slot) * 4u], dst, sizeof(dst));
        }
        slot++;
    }
    return true;
}

class BatchWriter {
public:
    void Clear() { buf_.clear(); }
    bool Empty() const { return buf_.empty(); }
    const std::vector<uint8_t>& Data() const { return buf_; }

    size_t Begin(uint32_t op) {
        size_t pos = buf_.size();
        U32(op);
        U32(0);
        return pos;
    }

    void End(size_t header_pos) {
        while (buf_.size() & 3u) buf_.push_back(0);
        uint32_t payload = uint32_t(buf_.size() - header_pos - 8u);
        std::memcpy(buf_.data() + header_pos + 4u, &payload, 4);
    }

    void U32(uint32_t v) {
        size_t p = buf_.size();
        buf_.resize(p + 4);
        std::memcpy(buf_.data() + p, &v, 4);
    }
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void F32(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        U32(u);
    }
    void Bytes(const void* data, size_t n) {
        size_t p = buf_.size();
        size_t aligned = (n + 3u) & ~size_t(3u);
        buf_.resize(p + aligned);
        if (n > 0 && data) std::memcpy(buf_.data() + p, data, n);
        if (aligned > n) std::memset(buf_.data() + p + n, 0, aligned - n);
    }

private:
    std::vector<uint8_t> buf_;
};

struct DrawShaders {
    bool prog = false;
    bool quad = false;
    bool fixed_transform = false;
    bool sprite = false;
    FixedKey fk{};
    uint64_t vkey = 0;
    uint64_t pkey = 0;
    uint64_t decode_wgpu = 0;
};

struct WebPsoKey {
    uint32_t vs_id = 0;
    uint32_t fs_id = 0;
    uint16_t regs = 0;
    uint8_t topo = 0;
    uint8_t strip_idx = 0;  // 0 none, 1 u16, 2 u32
    uint8_t cull = 0;
    uint8_t ccw = 0;
    uint8_t has_rt = 0;
    uint8_t has_ds = 0;
    uint8_t blend_en = 0;
    uint8_t src_rgb = 1, dst_rgb = 0, op_rgb = 0;
    uint8_t src_a = 1, dst_a = 0, op_a = 0;
    uint8_t wmask = 0xF;
    uint8_t z_en = 0, z_write = 0, z_func = 7;
    uint8_t sten_en = 0, sten_rmask = 0xFF, sten_wmask = 0xFF;
    uint8_t sten_fail = 0, sten_zfail = 0, sten_pass = 0, sten_func = 7;
    int32_t z_bias = 0;
    float z_slope = 0.0f;
    uint8_t tex_kinds[4] = {};
    uint32_t vb_count = 0;
    uint32_t strides[16] = {};
    uint32_t attr_count[16] = {};
    uint32_t attrs[16] = {};  // format | (buffer << 8) | (offset << 16)
};

class WebGpuBackend final : public Backend {
public:
    WebGpuBackend() { std::memset(ramp_, 0, sizeof(ramp_)); }

    bool Init(uint32_t width, uint32_t height) override {
        g_reversed_depth = true;
        guest_w_ = width ? width : 640u;
        guest_h_ = height ? height : 480u;
        scale_ = app::RenderScale(guest_h_);

        init_flag_ = 0;
        caps_ = 0;
        MAIN_THREAD_EM_ASM(
            {
                const flagPtr = $0 >>> 0;
                const capsPtr = $1 >>> 0;
                const getI32 = () => {
                    if (typeof growMemViews === 'function') growMemViews();
                    return HEAP32;
                };
                if (typeof window === 'undefined' || !window.__blackWebGpu) {
                    const i32 = getI32();
                    Atomics.store(i32, flagPtr >> 2, 2);
                    Atomics.notify(i32, flagPtr >> 2, 1);
                    return;
                }
                Promise.resolve(window.__blackWebGpu.ensureDevice())
                    .then((info) => {
                        const i32 = getI32();
                        const ok = info && info.ready ? 1 : 2;
                        i32[capsPtr >> 2] = info && info.bc ? 1 : 0;
                        Atomics.store(i32, flagPtr >> 2, ok);
                        Atomics.notify(i32, flagPtr >> 2, 1);
                    })
                    .catch(() => {
                        const i32 = getI32();
                        Atomics.store(i32, flagPtr >> 2, 2);
                        Atomics.notify(i32, flagPtr >> 2, 1);
                    });
            },
            &init_flag_, &caps_);

        while (__atomic_load_n(&init_flag_, __ATOMIC_ACQUIRE) == 0) {
            emscripten_futex_wait(&init_flag_, 0, 50.0);
        }
        const bool ok = (__atomic_load_n(&init_flag_, __ATOMIC_ACQUIRE) == 1);
        has_bc_ = (caps_ & 1u) != 0;
        web_stats::g_gpu_ready.store(ok, std::memory_order_release);
        LOGI("gpu_webgpu: init %s (%ux%u scale %.2f, BC=%d)", ok ? "OK" : "FAILED", guest_w_, guest_h_, scale_,
             has_bc_ ? 1 : 0);
        return ok;
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
        Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
        if (!rt && !ds) return;

        Target* ref = rt ? rt : ds;
        scratch_rects_.clear();
        if (count && rects) {
            scratch_rects_.reserve(size_t(count) * 4u);
            const float sc = float(ref->w) / float(ref->gw ? ref->gw : 1u);
            for (uint32_t i = 0; i < count; i++) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * 16u, sizeof(r));
                scratch_rects_.push_back(std::clamp<int32_t>(int32_t(r[0] * sc), 0, int32_t(ref->w)));
                scratch_rects_.push_back(std::clamp<int32_t>(int32_t(r[1] * sc), 0, int32_t(ref->h)));
                scratch_rects_.push_back(std::clamp<int32_t>(int32_t(r[2] * sc), 0, int32_t(ref->w)));
                scratch_rects_.push_back(std::clamp<int32_t>(int32_t(r[3] * sc), 0, int32_t(ref->h)));
            }
        } else {
            const float sc = float(ref->w) / float(ref->gw ? ref->gw : 1u);
            const auto& v = s.viewport;
            scratch_rects_.push_back(std::clamp<int32_t>(int32_t(v.x * sc), 0, int32_t(ref->w)));
            scratch_rects_.push_back(std::clamp<int32_t>(int32_t(v.y * sc), 0, int32_t(ref->h)));
            scratch_rects_.push_back(std::clamp<int32_t>(int32_t((v.x + v.w) * sc), 0, int32_t(ref->w)));
            scratch_rects_.push_back(std::clamp<int32_t>(int32_t((v.y + v.h) * sc), 0, int32_t(ref->h)));
        }

        if (rt && !rt->depth && LatchCpuWrite(*rt)) UploadColorTarget(*rt, s.rt);

        float col[4];
        ColorArgb(color, col);
        uint32_t mask = (flags & kClearTarget) ? ((flags >> 4) & 0xFu) : 0u;
        uint32_t clear_flags = (mask ? 1u : 0u) | ((flags & kClearZ) && ds ? 2u : 0u) |
                               ((flags & kClearStencil) && ds ? 4u : 0u);
        const float z_rev = 1.0f - std::clamp(z, 0.0f, 1.0f);

        const size_t pos = writer_.Begin(kOpClear);
        writer_.U32(rt ? rt->id : 0u);
        writer_.U32(ds ? ds->id : 0u);
        writer_.U32(clear_flags);
        writer_.U32(mask);
        for (int i = 0; i < 4; i++) writer_.F32(col[i]);
        writer_.F32(z_rev);
        writer_.U32(stencil & 0xFFu);
        writer_.U32(uint32_t(scratch_rects_.size() / 4u));
        for (int32_t v : scratch_rects_) writer_.I32(v);
        writer_.End(pos);

        if (rt) rt->drawn = true;
        if (ds) ds->drawn = true;
        web_stats::g_clears.fetch_add(1, std::memory_order_relaxed);
    }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        Target* rt = EnsureTarget(s.rt, false);
        Target* ds = EnsureTarget(s.zs, true);
        if (!rt && !ds) return;
        if (rt && LatchCpuWrite(*rt)) UploadColorTarget(*rt, s.rt);

        if (!Assemble(s, prim, start, count, indices, asm_)) return;

        // Check viewport/scissor first so zero-area scissors skip cleanly.
        const uint32_t tw = rt ? rt->w : ds->w;
        const uint32_t th = rt ? rt->h : ds->h;
        uint32_t gw = 640, gh = 480;
        SurfaceSize(rt ? s.rt : s.zs, gw, gh);
        if (!gw) gw = 1;
        if (!gh) gh = 1;
        const float sc = float(tw) / float(gw);
        int32_t x0 = int32_t(s.viewport.x * sc);
        int32_t y0 = int32_t(s.viewport.y * sc);
        int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc);
        int32_t y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
        if (s.scissor_count) {
            x0 = std::max(x0, int32_t(s.scissors[0] * sc));
            y0 = std::max(y0, int32_t(s.scissors[1] * sc));
            x1 = std::min(x1, int32_t(s.scissors[2] * sc));
            y1 = std::min(y1, int32_t(s.scissors[3] * sc));
        }
        x0 = std::clamp<int32_t>(x0, 0, int32_t(tw));
        y0 = std::clamp<int32_t>(y0, 0, int32_t(th));
        x1 = std::clamp<int32_t>(x1, 0, int32_t(tw));
        y1 = std::clamp<int32_t>(y1, 0, int32_t(th));
        if (x1 <= x0 || y1 <= y0) return;

        // Normalize geometry to WebGPU alignment & format rules before shader key generation.
        scratch_verts_.clear();
        bool using_converted = !asm_.vertices.empty();
        if (!using_converted) {
            bool need_expand = false;
            for (int r = 0; r < 16; r++) {
                if (!((asm_.regs >> r) & 1u)) continue;
                const VertexElement& e = asm_.elements[size_t(r)];
                const uint32_t wf = WgslVertexFmtCode(e.format);
                const VertexBuffer& vb = asm_.buffers[e.buffer];
                if (wf == UINT32_MAX || (e.offset & 3u) || vb.stride == 0 || (vb.stride & 3u)) {
                    need_expand = true;
                    break;
                }
            }
            if (need_expand && ExpandUnsupportedVertexFormat(asm_, scratch_verts_)) {
                using_converted = true;
            }
        }

        // Compute WebGPU vertex buffer bindings & decode word.
        uint64_t decode_wgpu = using_converted ? 0ull : asm_.decode;
        uint32_t num_regs = 0;
        for (int r = 0; r < 16; r++) num_regs += ((asm_.regs >> r) & 1u);

        struct VbSlice {
            const uint8_t* ptr = nullptr;
            uint32_t bytes = 0;
            uint32_t stride = 0;
        };
        std::array<VbSlice, 16> vbs{};
        uint32_t vb_count = 0;
        std::array<VertexElement, 16> wgpu_elems{};

        if (using_converted) {
            const float* src_f = !asm_.vertices.empty() ? asm_.vertices.data() : scratch_verts_.data();
            const uint32_t float_count =
                !asm_.vertices.empty() ? uint32_t(asm_.vertices.size()) : uint32_t(scratch_verts_.size());
            if (asm_.regs && num_regs) {
                vb_count = 1;
                vbs[0] = {reinterpret_cast<const uint8_t*>(src_f), float_count * 4u, num_regs * 16u};
                uint16_t slot = 0;
                for (int r = 0; r < 16; r++) {
                    if (!((asm_.regs >> r) & 1u)) continue;
                    wgpu_elems[size_t(r)] = {DXGI_FORMAT_R32G32B32A32_FLOAT, 0, uint16_t(slot * 16u)};
                    slot++;
                }
            }
        } else {
            vb_count = asm_.buffer_count;
            for (uint32_t b = 0; b < vb_count; b++) {
                vbs[b] = {asm_.buffers[b].src, asm_.buffers[b].bytes, asm_.buffers[b].stride};
            }
            wgpu_elems = asm_.elements;
            for (int r = 0; r < 16; r++) {
                if (!((asm_.regs >> r) & 1u)) continue;
                if (wgpu_elems[size_t(r)].format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    decode_wgpu = (decode_wgpu & ~(0xFull << (r * 4))) | (uint64_t(gpu_webgpu::kWebDecodeBgra) << (r * 4));
                }
            }
        }

        // Bind stage textures
        TexInfo tex_info[4]{};
        uint32_t tex_ids[4]{};
        bool stage_rt[4]{};
        float stage_wide[4]{};
        uint32_t smp_ids[4]{};
        bool clamp_uv = AnyStageWraps(s) && UnitTexcoords(asm_);
        bool fixed_vs = !asm_.pretransformed && !((s.vs_handle & 1u) && s.vs && !s.vs->program.empty());
        for (int i = 0; i < 4; i++) {
            tex_ids[i] = BindTexture(s, i, rt, ds, tex_info[i], stage_rt[i], stage_wide[i]);
            smp_ids[i] = EnsureSampler(s, i, tex_info[i], clamp_uv, fixed_vs);
        }

        // Classify & compile/lookup shaders
        DrawShaders sh;
        sh.prog = (s.vs_handle & 1u) && s.vs && !s.vs->program.empty();
        if (sh.prog && !s.vs->hash) {
            s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4u) | 1ull;
        }
        const bool points = (asm_.topology == Topology::Points);
        sh.quad = (count <= kQuadVertices) && !s.rs[kRsZEnable] && !points;
        sh.fixed_transform = !sh.prog && !asm_.pretransformed;
        sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
        sh.decode_wgpu = decode_wgpu;
        if (!sh.prog) sh.fk = MakeFixedKey(s);

        uint64_t seed = sh.prog ? (s.vs->hash ^ asm_.regs) : Hash(&s.vs_handle, 4, asm_.regs | kDeclarationSalt);
        sh.vkey = Hash(&sh.decode_wgpu, 8, seed);
        if (!sh.prog && asm_.pretransformed) sh.vkey ^= kPretransformedSalt;
        if (sh.quad) sh.vkey ^= kQuadSalt;
        if (sh.sprite) sh.vkey ^= kSpriteSalt;
        if (!sh.prog) sh.vkey = Hash(&sh.fk, sizeof(sh.fk), sh.vkey);

        uint32_t vs_id = EnsureVertexShader(s, sh);
        PixelKey pk = MakePixelKey(s, tex_info);
        if (sh.quad) pk.quad = 1;
        sh.pkey = Hash(&pk, sizeof(pk));
        uint32_t fs_id = EnsurePixelShader(pk, sh.pkey);

        // Build WebGPU PSO
        uint32_t tex_kinds[4]{};
        for (int i = 0; i < 4; i++) {
            const TexInfo& ti = pk.tex[i];
            tex_kinds[i] = ti.shadow ? kViewDepth : (ti.kind == kTex3D ? kView3D : (ti.kind == kTexCube ? kViewCube : kView2D));
        }

        const uint32_t idx_fmt = (asm_.index_count > 0) ? (asm_.index16 ? 1u : 2u) : 0u;
        const uint32_t pso_id = EnsurePipeline(s, asm_, vs_id, fs_id, rt != nullptr, ds != nullptr, ds, tex_kinds,
                                               vb_count, vbs.data(), wgpu_elems, idx_fmt);

        // Fill DrawConstants
        DrawConstants dc{};
        dc.surf[0] = float(gw);
        dc.surf[1] = float(gh);
        dc.surf[2] = 1.0f / (s.zscale != 0.0f ? s.zscale : 16777215.0f);
        if (points) dc.surf[3] = float(tw) / float(gw);
        ColorArgb(s.rs[kRsFogColor], dc.fog_color);
        dc.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFFu);
        dc.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(MapCmp0(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
        dc.misc[2] = float(MapCmp0(s.rs[kRsShadowFunc]));
        dc.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;

        if (s.rs[kRsFogEnable]) {
            if (sh.prog && s.rs[kRsFogTableMode] == kFogTableNone) {
                dc.fog[0] = 1.0f;
                dc.fog[1] = 0.0f;
                dc.fog[2] = 0.0f;
                dc.fog[3] = kFogTableLinear;
            } else {
                dc.fog[0] = Bits(s.rs[kRsFogStart]);
                dc.fog[1] = Bits(s.rs[kRsFogEnd]);
                dc.fog[2] = Bits(s.rs[kRsFogDensity]);
                dc.fog[3] = float(s.rs[kRsFogTableMode]);
            }
        }
        for (int i = 0; i < 8; i++) {
            ColorArgb(SimpleState(s, kMPsConstant0 + uint32_t(i) * 4u), dc.pc[i * 2]);
            ColorArgb(SimpleState(s, kMPsConstant1 + uint32_t(i) * 4u), dc.pc[i * 2 + 1]);
        }
        ColorArgb(SimpleState(s, kMPsFinalConstant0), dc.pc[16]);
        ColorArgb(SimpleState(s, kMPsFinalConstant1), dc.pc[17]);
        ColorArgb(s.rs[kRsTextureFactor], dc.tfactor);

        const float rt_shift = (!sh.prog || ReadsViewportOffset(s.vs->program)) ? (-0.5f * float(tw) / float(gw)) : 0.0f;
        for (int i = 0; i < 4; i++) {
            TextureDesc td;
            if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], td)) {
                dc.texsize[i][0] = float(td.width);
                dc.texsize[i][1] = float(td.height);
                if (stage_rt[i] && stage_wide[i] > 1.01f) {
                    float n = std::ceil(kGlowBoxTexels * stage_wide[i] * 0.5f);
                    dc.spread[i][0] = kGlowBoxTexels / (n * float(td.width));
                    dc.spread[i][1] = kGlowBoxTexels / (n * float(td.height));
                    dc.spread[i][2] = n;
                }
            }
            if (stage_rt[i]) dc.rtshift[i] = rt_shift;
            const auto& t = s.tss[size_t(i)];
            dc.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
            dc.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
            dc.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
            dc.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
            dc.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
            dc.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        }

        // Depth parameters (Reversed Depth)
        {
            bool offset = (asm_.topology == Topology::Triangles) && SimpleState(s, kMSolidOffsetEnable);
            float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0.0f;
            float zs = s.zscale != 0.0f ? s.zscale : 16777215.0f;
            if (s.rs[kRsZEnable] != kZUseW) {
                dc.zparams[2] = units / zs;
            } else {
                float near_w = 0.0f, far_w = zs;
                if (!sh.prog) {
                    const float* m = s.transforms[kTsProjection].data();
                    if (m[10] != m[11] && m[10] != 0.0f) {
                        near_w = m[15] - m[14] / m[10] * m[11];
                        far_w = (m[15] - m[14]) / (m[10] - m[11]) * m[11] + m[15];
                    }
                } else if (SimpleState(s, kMClipMax)) {
                    near_w = Bits(SimpleState(s, kMClipMin));
                }
                if (near_w <= 0.0f || near_w >= far_w) near_w = far_w * kFallbackNear;
                dc.zparams[0] = near_w / (far_w - near_w);
                dc.zparams[1] = far_w;
                dc.zparams[2] = sh.prog ? units : units * far_w / zs;
            }
        }

        if (sh.fixed_transform) {
            float wv[16], wvp[16], full[16];
            MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
            MatMul(wv, s.transforms[kTsProjection].data(), wvp);
            const auto& v = s.viewport;
            float sx = float(v.w) / float(gw);
            float sy = float(v.h) / float(gh);
            float ox = (2.0f * v.x + v.w) / float(gw) - 1.0f;
            float oy = 1.0f - (2.0f * v.y + v.h) / float(gh);
            float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
            MatMul(wvp, vpm, full);
            Transpose(full, dc.wvp);
            Transpose(wv, dc.wv);
            float n[16] = {};
            InverseTranspose3x3(wv, n);
            Transpose(n, dc.wvn);
            for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), dc.tm[t]);

            std::memcpy(dc.mat, s.material.data(), sizeof(float) * 16);
            dc.mat[4][0] = s.material[16];
            ColorArgb(s.rs[kRsAmbient], dc.amb);
            const float* vm = s.transforms[kTsView].data();
            for (int i = 0; i < 8; i++) {
                if (!((sh.fk.types >> (i * 2)) & 3u)) continue;
                float lf[26];
                std::memcpy(lf, s.lights[size_t(i)].data(), sizeof(lf));
                for (int j = 0; j < 4; j++) {
                    dc.lt[i][0][j] = lf[1 + j];
                    dc.lt[i][1][j] = lf[5 + j];
                    dc.lt[i][2][j] = lf[9 + j];
                }
                for (int j = 0; j < 3; j++) {
                    dc.lt[i][3][j] = lf[13] * vm[j] + lf[14] * vm[4 + j] + lf[15] * vm[8 + j] + vm[12 + j];
                    dc.lt[i][4][j] = lf[16] * vm[j] + lf[17] * vm[4 + j] + lf[18] * vm[8 + j];
                }
                dc.lt[i][3][3] = lf[19];
                dc.lt[i][4][3] = lf[20];
                dc.lt[i][5][0] = lf[21];
                dc.lt[i][5][1] = lf[22];
                dc.lt[i][5][2] = lf[23];
                dc.lt[i][6][0] = std::cos(lf[24] * 0.5f);
                dc.lt[i][6][1] = std::cos(lf[25] * 0.5f);
            }
        }

        // Emit OP_DRAW
        const bool vsc_dirty = !has_batch_vsc_ || std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc)) != 0;
        const bool dc_dirty = !has_batch_dc_ || std::memcmp(&last_dc_, &dc, sizeof(dc)) != 0;
        if (vsc_dirty) {
            std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
            has_batch_vsc_ = true;
        }
        if (dc_dirty) {
            std::memcpy(&last_dc_, &dc, sizeof(dc));
            has_batch_dc_ = true;
        }

        float bf[4]{};
        ColorArgb(SimpleState(s, kMBlendColor), bf);
        const uint32_t sten_ref = SimpleState(s, kMStencilRef) & 0xFFu;
        const uint32_t idx_bytes =
            (asm_.index_count > 0) ? (asm_.index_count * (asm_.index16 ? 2u : 4u)) : 0u;

        const size_t pos = writer_.Begin(kOpDraw);
        writer_.U32(pso_id);
        writer_.U32(rt ? rt->id : 0u);
        writer_.U32(ds ? ds->id : 0u);
        for (int i = 0; i < 4; i++) writer_.U32(tex_ids[i]);
        for (int i = 0; i < 4; i++) writer_.U32(smp_ids[i]);
        for (int i = 0; i < 4; i++) writer_.U32(tex_kinds[i]);
        writer_.U32(uint32_t(x0));
        writer_.U32(uint32_t(y0));
        writer_.U32(uint32_t(x1 - x0));
        writer_.U32(uint32_t(y1 - y0));
        for (int i = 0; i < 4; i++) writer_.F32(bf[i]);
        writer_.U32(sten_ref);
        writer_.U32(idx_fmt);
        writer_.U32(asm_.index_count ? asm_.index_count : asm_.draw_count);
        writer_.I32(asm_.index_count ? asm_.base_vertex : 0);
        writer_.U32(idx_bytes);
        writer_.U32(vb_count);
        writer_.U32(vsc_dirty ? 1u : 0u);
        writer_.U32(dc_dirty ? 1u : 0u);
        if (vsc_dirty) writer_.Bytes(s.vsc.data(), sizeof(s.vsc));
        if (dc_dirty) writer_.Bytes(&dc, sizeof(dc));
        if (idx_bytes > 0) writer_.Bytes(asm_.index_src, idx_bytes);
        for (uint32_t b = 0; b < vb_count; b++) {
            writer_.U32(vbs[b].bytes);
            writer_.Bytes(vbs[b].ptr, vbs[b].bytes);
        }
        writer_.End(pos);

        if (rt) rt->drawn = true;
        if (ds) ds->drawn = true;
        web_stats::g_draws.fetch_add(1, std::memory_order_relaxed);
    }

    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va, uint32_t dst_points_va,
              float src_kx, float src_ky) override {
        Target* src_t = FindTarget(src, false);
        if (!src_t || !src_t->drawn) {
            if (CopySurfaceMemory(src, dst, count, src_rects_va, dst_points_va)) {
                if (Target* dst_t = FindTarget(dst, false)) UploadColorTarget(*dst_t, dst);
            }
            return;
        }
        Target* dst_t = EnsureTarget(dst, false);
        if (!dst_t || src_t == dst_t) return;

        const bool use_blit = (src_t->w != dst_t->w || src_t->h != dst_t->h || src_kx != 1.0f || src_ky != 1.0f);
        const uint32_t n = count ? count : 1u;
        const uint8_t* rects = count && src_rects_va ? static_cast<const uint8_t*>(kernel::HostPtr(src_rects_va)) : nullptr;
        const uint8_t* pts = count && dst_points_va ? static_cast<const uint8_t*>(kernel::HostPtr(dst_points_va)) : nullptr;

        const size_t pos = writer_.Begin(kOpCopyTex);
        writer_.U32(src_t->id);
        writer_.U32(dst_t->id);
        writer_.U32(use_blit ? 1u : 0u);
        writer_.U32(n);
        const float skx = src_t->gw ? float(src_t->w) / (float(src_t->gw) * src_kx) : 1.0f;
        const float sky = src_t->gh ? float(src_t->h) / (float(src_t->gh) * src_ky) : 1.0f;
        const float dkx = dst_t->gw ? float(dst_t->w) / float(dst_t->gw) : 1.0f;
        const float dky = dst_t->gh ? float(dst_t->h) / float(dst_t->gh) : 1.0f;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t sx = 0, sy = 0, sw = src_t->w, sh = src_t->h, dx = 0, dy = 0, dw = dst_t->w, dh = dst_t->h;
            if (rects) {
                int32_t r[4];
                std::memcpy(r, rects + size_t(i) * 16u, 16);
                sx = uint32_t(std::clamp<int32_t>(int32_t(r[0] * skx), 0, int32_t(src_t->w)));
                sy = uint32_t(std::clamp<int32_t>(int32_t(r[1] * sky), 0, int32_t(src_t->h)));
                uint32_t sx1 = uint32_t(std::clamp<int32_t>(int32_t(r[2] * skx), 0, int32_t(src_t->w)));
                uint32_t sy1 = uint32_t(std::clamp<int32_t>(int32_t(r[3] * sky), 0, int32_t(src_t->h)));
                sw = sx1 > sx ? sx1 - sx : 0u;
                sh = sy1 > sy ? sy1 - sy : 0u;
                dw = uint32_t(std::lround(float(r[2] - r[0]) * dkx));
                dh = uint32_t(std::lround(float(r[3] - r[1]) * dky));
            }
            if (pts) {
                int32_t pt[2];
                std::memcpy(pt, pts + size_t(i) * 8u, 8);
                dx = uint32_t(std::clamp<int32_t>(int32_t(pt[0] * dkx), 0, int32_t(dst_t->w)));
                dy = uint32_t(std::clamp<int32_t>(int32_t(pt[1] * dky), 0, int32_t(dst_t->h)));
            }
            writer_.U32(sx);
            writer_.U32(sy);
            writer_.U32(sw);
            writer_.U32(sh);
            writer_.U32(dx);
            writer_.U32(dy);
            writer_.U32(dw);
            writer_.U32(dh);
        }
        writer_.End(pos);
        dst_t->drawn = true;
        web_stats::g_copies.fetch_add(1, std::memory_order_relaxed);
    }

    void Present(const Surface& front) override {
        Target* t = EnsureTarget(front, false);
        if (t && LatchCpuWrite(*t)) UploadColorTarget(*t, front);
        if (ramp_dirty_) {
            const size_t pos = writer_.Begin(kOpUploadGamma);
            std::vector<uint8_t> lut(256u * 4u, 255u);
            for (uint32_t i = 0; i < 256u; i++) {
                lut[i * 4u + 0u] = ramp_[i];
                lut[i * 4u + 1u] = ramp_[256u + i];
                lut[i * 4u + 2u] = ramp_[512u + i];
                lut[i * 4u + 3u] = 255u;
            }
            writer_.Bytes(lut.data(), lut.size());
            writer_.End(pos);
            ramp_dirty_ = false;
        }

        const auto mode = app::Output();
        const size_t pos = writer_.Begin(kOpPresent);
        writer_.U32(t ? t->id : 0u);
        writer_.U32(app::Widescreen() ? 1u : 0u);
        writer_.U32(gamma_ ? 1u : 0u);
        writer_.U32(mode.width);
        writer_.U32(mode.height);
        writer_.End(pos);

        FlushBatch(false);
        presents_++;
        web_stats::g_presents.store(presents_, std::memory_order_relaxed);

        // Rebuild scaled render targets if browser window resized or render_percent changed
        float want = app::RenderScale(guest_h_);
        if (std::fabs(want - scale_) > 1e-3f) {
            for (const auto& [k, tg] : targets_) {
                DestroyTex(tg.id);
                if (tg.feedback_id) DestroyTex(tg.feedback_id);
            }
            targets_.clear();
            scale_ = want;
        }
        if ((presents_ % 60u) == 0u) {
            ReleaseOldTextures();
        }
    }

    void Sync(uint32_t data_va, uint32_t) override {
        Target* match = nullptr;
        for (auto& [k, t] : targets_) {
            if (!t.depth && t.data == data_va && t.drawn) {
                match = &t;
                break;
            }
        }
        if (!match) return;

        FlushBatch(false);
        const uint32_t w = match->gw, h = match->gh;
        const uint32_t pitch = GuestPitch(w);
        if (!w || !h) return;

        std::vector<uint8_t> staging(size_t(w) * h * 4u, 0u);
        sync_flag_ = 0;
        MAIN_THREAD_EM_ASM(
            {
                const texId = $0 >>> 0;
                const w = $1 >>> 0;
                const h = $2 >>> 0;
                const outPtr = $3 >>> 0;
                const flagPtr = $4 >>> 0;
                const getI32 = () => {
                    if (typeof growMemViews === 'function') growMemViews();
                    return HEAP32;
                };
                if (!window.__blackWebGpu) {
                    const i32 = getI32();
                    Atomics.store(i32, flagPtr >> 2, 2);
                    Atomics.notify(i32, flagPtr >> 2, 1);
                    return;
                }
                Promise.resolve(window.__blackWebGpu.readbackTexture(texId, w, h, outPtr))
                    .then(() => {
                        const i32 = getI32();
                        Atomics.store(i32, flagPtr >> 2, 1);
                        Atomics.notify(i32, flagPtr >> 2, 1);
                    })
                    .catch(() => {
                        const i32 = getI32();
                        Atomics.store(i32, flagPtr >> 2, 2);
                        Atomics.notify(i32, flagPtr >> 2, 1);
                    });
            },
            match->id, w, h, staging.data(), &sync_flag_);

        while (__atomic_load_n(&sync_flag_, __ATOMIC_ACQUIRE) == 0) {
            emscripten_futex_wait(&sync_flag_, 0, 50.0);
        }

        if (__atomic_load_n(&sync_flag_, __ATOMIC_ACQUIRE) == 1) {
            uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data_va));
            for (uint32_t y = 0; y < h; y++) {
                std::memcpy(dst + size_t(y) * pitch, staging.data() + size_t(y) * w * 4u, size_t(w) * 4u);
            }
            match->read.swap(staging);
            match->sig = MemSig(data_va, pitch * h);
            match->epoch = DirtyEpoch(data_va);
        }
        match->drawn = false;
        web_stats::g_last_sync_va.store(data_va, std::memory_order_relaxed);
    }

    void SyncLate(uint32_t data_va) override { Sync(data_va, 0); }

    void Flush() override { FlushBatch(true); }

    void SetGamma(const uint8_t* ramp) override {
        if (!ramp) return;
        std::memcpy(ramp_, ramp, 768);
        ramp_dirty_ = true;
        bool identity = true;
        for (int i = 0; i < 256 && identity; i++) {
            if (ramp_[i] != i || ramp_[256 + i] != i || ramp_[512 + i] != i) identity = false;
        }
        gamma_ = !identity;
    }

private:
    struct Target {
        uint32_t id = 0;
        uint32_t feedback_id = 0;
        uint32_t data = 0;
        uint32_t gw = 0, gh = 0;
        uint32_t w = 0, h = 0;
        bool depth = false;
        bool drawn = false;
        uint64_t sig = 0;
        uint64_t epoch = 0;
        uint64_t last_frame = 0;
        std::vector<uint8_t> read;
    };

    struct Tex {
        uint32_t id = 0;
        TextureDesc d{};
        uint64_t sig = 0;
        uint64_t epoch = 0;
        uint64_t pal_hash = 0;
        uint64_t last_frame = 0;
    };

    uint32_t AllocId() { return next_id_++; }

    void DestroyTex(uint32_t id) {
        if (!id) return;
        const size_t pos = writer_.Begin(kOpDestroyTexture);
        writer_.U32(id);
        writer_.End(pos);
    }

    // Varre periodicamente texturas e render targets antigos entre transições de fase/vídeo XMV
    // evitando acúmulo de VRAM no WebGPU (espelha D3D12Backend::ReleaseOldTextures).
    void ReleaseOldTextures() {
        size_t swept = 0;
        if (textures_.size() > 768u) {
            for (auto it = textures_.begin(); it != textures_.end();) {
                const uint64_t lf = it->second.last_frame;
                if (presents_ > lf && (presents_ - lf) > 600u) {
                    DestroyTex(it->second.id);
                    it = textures_.erase(it);
                    ++swept;
                } else {
                    ++it;
                }
            }
        }
        if (targets_.size() > 32u) {
            for (auto it = targets_.begin(); it != targets_.end();) {
                const uint64_t lf = it->second.last_frame;
                if (presents_ > lf && (presents_ - lf) > 1200u) {
                    DestroyTex(it->second.id);
                    if (it->second.feedback_id) DestroyTex(it->second.feedback_id);
                    it = targets_.erase(it);
                    ++swept;
                } else {
                    ++it;
                }
            }
        }
        if (swept >= 32u) {
            LOGR("gpu_webgpu: ReleaseOldTextures liberou %zu recursos antigos (restantes tex=%zu rt=%zu)",
                 swept, textures_.size(), targets_.size());
        }
    }

    void FlushBatch(bool wait_gpu) {
        if (!writer_.Empty()) {
            const auto& buf = writer_.Data();
            MAIN_THREAD_EM_ASM(
                {
                    if (window.__blackWebGpu) {
                        window.__blackWebGpu.submitBatch($0 >>> 0, $1 >>> 0);
                    }
                },
                buf.data(), buf.size());
            writer_.Clear();
        }
        has_batch_vsc_ = false;
        has_batch_dc_ = false;

        if (wait_gpu) {
            flush_flag_ = 0;
            MAIN_THREAD_EM_ASM(
                {
                    const flagPtr = $0 >>> 0;
                    const getI32 = () => {
                        if (typeof growMemViews === 'function') growMemViews();
                        return HEAP32;
                    };
                    if (!window.__blackWebGpu) {
                        const i32 = getI32();
                        Atomics.store(i32, flagPtr >> 2, 1);
                        Atomics.notify(i32, flagPtr >> 2, 1);
                        return;
                    }
                    Promise.resolve(window.__blackWebGpu.waitIdle())
                        .then(() => {
                            const i32 = getI32();
                            Atomics.store(i32, flagPtr >> 2, 1);
                            Atomics.notify(i32, flagPtr >> 2, 1);
                        })
                        .catch(() => {
                            const i32 = getI32();
                            Atomics.store(i32, flagPtr >> 2, 1);
                            Atomics.notify(i32, flagPtr >> 2, 1);
                        });
                },
                &flush_flag_);
            while (__atomic_load_n(&flush_flag_, __ATOMIC_ACQUIRE) == 0) {
                emscripten_futex_wait(&flush_flag_, 0, 50.0);
            }
        }
    }

    Target* FindTarget(const Surface& s, bool depth) {
        if (!s.header || !s.data) return nullptr;
        uint32_t gw = 0, gh = 0;
        SurfaceSize(s, gw, gh);
        if (!gw || !gh) return nullptr;
        const uint32_t data_va = mem::VirtualFromPhysical(s.data);
        auto it = targets_.find(SizedTargetKey(data_va, depth, gw, gh));
        if (it == targets_.end()) return nullptr;
        it->second.last_frame = presents_ + 1u;
        return &it->second;
    }

    Target* EnsureTarget(const Surface& s, bool depth) {
        if (!s.header || !s.data) return nullptr;
        uint32_t gw = 0, gh = 0;
        SurfaceSize(s, gw, gh);
        if (!gw || !gh) return nullptr;
        const uint32_t data_va = mem::VirtualFromPhysical(s.data);
        const uint64_t key = SizedTargetKey(data_va, depth, gw, gh);
        auto it = targets_.find(key);
        if (it != targets_.end()) {
            it->second.last_frame = presents_ + 1u;
            return &it->second;
        }

        uint32_t w = std::max(1u, uint32_t(std::lround(float(gw) * scale_)));
        uint32_t h = std::max(1u, uint32_t(std::lround(float(gh) * scale_)));
        Target t;
        t.id = AllocId();
        t.data = data_va;
        t.gw = gw;
        t.gh = gh;
        t.w = w;
        t.h = h;
        t.depth = depth;
        t.last_frame = presents_ + 1u;

        const size_t pos = writer_.Begin(kOpCreateTexture);
        writer_.U32(t.id);
        writer_.U32(depth ? kWgpuTexD24S8 : kWgpuTexBgra8);
        writer_.U32(0u);  // 2D
        writer_.U32(w);
        writer_.U32(h);
        writer_.U32(1u);
        writer_.U32(1u);
        writer_.U32(depth ? 2u : 1u);
        writer_.End(pos);

        if (!depth) {
            const uint32_t total = GuestPitch(gw) * gh;
            t.sig = MemSig(data_va, total);
            t.epoch = DirtyEpoch(data_va);
            UploadColorTarget(t, s);
        }
        auto [ins, ok] = targets_.emplace(key, std::move(t));
        return &ins->second;
    }

    bool LatchCpuWrite(Target& t) {
        if (t.depth) return false;
        const uint64_t ep = DirtyEpoch(t.data);
        const uint32_t pitch = GuestPitch(t.gw);
        const uint32_t total = pitch * t.gh;
        const uint64_t sig = MemSig(t.data, total);
        if (!t.drawn && ep == t.epoch && sig == t.sig) return false;
        if (t.drawn && ep == t.epoch && sig == t.sig) return false;
        t.epoch = ep;
        t.sig = sig;
        return true;
    }

    void UploadColorTarget(Target& t, const Surface& s) {
        TextureDesc d;
        std::vector<uint8_t> px;
        uint32_t pitch = 0;
        if (s.header && DescribeSurface(s, d)) {
            DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(t.data)), nullptr, px, pitch);
        }
        if (px.empty()) {
            pitch = t.gw * 4u;
            px.resize(size_t(pitch) * t.gh, 0);
            const uint8_t* src = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
            const uint32_t gp = GuestPitch(t.gw);
            for (uint32_t y = 0; y < t.gh; y++) {
                std::memcpy(px.data() + size_t(y) * pitch, src + size_t(y) * gp, pitch);
            }
        }
        // Upload tight guest-sized texture; if target is upscaled, keep a scratch upload texture & blit or upload directly when 1:1
        if (t.w == t.gw && t.h == t.gh) {
            const size_t pos = writer_.Begin(kOpUploadTexture);
            writer_.U32(t.id);
            writer_.U32(0u);
            writer_.U32(0u);
            writer_.U32(t.w);
            writer_.U32(t.h);
            writer_.U32(1u);
            writer_.U32(pitch);
            writer_.U32(t.h);
            writer_.U32(uint32_t(px.size()));
            writer_.Bytes(px.data(), px.size());
            writer_.End(pos);
        } else {
            uint32_t tmp_id = AllocId();
            {
                const size_t pos = writer_.Begin(kOpCreateTexture);
                writer_.U32(tmp_id);
                writer_.U32(kWgpuTexBgra8);
                writer_.U32(0u);
                writer_.U32(t.gw);
                writer_.U32(t.gh);
                writer_.U32(1u);
                writer_.U32(1u);
                writer_.U32(0u);
                writer_.End(pos);
            }
            {
                const size_t pos = writer_.Begin(kOpUploadTexture);
                writer_.U32(tmp_id);
                writer_.U32(0u);
                writer_.U32(0u);
                writer_.U32(t.gw);
                writer_.U32(t.gh);
                writer_.U32(1u);
                writer_.U32(pitch);
                writer_.U32(t.gh);
                writer_.U32(uint32_t(px.size()));
                writer_.Bytes(px.data(), px.size());
                writer_.End(pos);
            }
            {
                const size_t pos = writer_.Begin(kOpCopyTex);
                writer_.U32(tmp_id);
                writer_.U32(t.id);
                writer_.U32(1u);
                writer_.U32(1u);
                writer_.U32(0u);
                writer_.U32(0u);
                writer_.U32(t.gw);
                writer_.U32(t.gh);
                writer_.U32(0u);
                writer_.U32(0u);
                writer_.U32(t.w);
                writer_.U32(t.h);
                writer_.End(pos);
            }
            DestroyTex(tmp_id);
        }
    }

    uint32_t BindTexture(const State& s, int stage, Target* draw_rt, Target* draw_ds, TexInfo& info, bool& is_rt,
                         float& wide) {
        TextureDesc d;
        if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return 0u;
        const FormatInfo& fi = Format(d.format);
        info.kind = d.cube ? kTexCube : (d.depth > 1 ? kTex3D : kTex2D);
        info.rect = d.linear ? 1u : 0u;
        info.shadow = fi.depth ? ShadowDepthKind(d.format) : 0u;

        // Check if this guest surface corresponds to an active GPU render/depth target.
        auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
        if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
            Target& tg = tit->second;
            tg.last_frame = presents_ + 1u;
            if (&tg == draw_ds) return 0u;
            is_rt = true;
            info.flags |= kTexTarget;
            info.kind = kTex2D;
            if (!tg.depth && LatchCpuWrite(tg)) {
                Surface su{};
                su.header = s.textures[size_t(stage)];
                UploadColorTarget(tg, su);
            }
            if (&tg == draw_rt) {
                if (tg.depth) return 0u;
                if (!tg.feedback_id) {
                    tg.feedback_id = AllocId();
                    const size_t pos = writer_.Begin(kOpCreateTexture);
                    writer_.U32(tg.feedback_id);
                    writer_.U32(kWgpuTexBgra8);
                    writer_.U32(0u);  // 2D
                    writer_.U32(tg.w);
                    writer_.U32(tg.h);
                    writer_.U32(1u);
                    writer_.U32(1u);
                    writer_.U32(1u);
                    writer_.End(pos);
                }
                const size_t pos = writer_.Begin(kOpCopyTex);
                writer_.U32(tg.id);
                writer_.U32(tg.feedback_id);
                writer_.U32(0u);
                writer_.U32(1u);
                writer_.U32(0u);
                writer_.U32(0u);
                writer_.U32(tg.w);
                writer_.U32(tg.h);
                writer_.U32(0u);
                writer_.U32(0u);
                writer_.U32(tg.w);
                writer_.U32(tg.h);
                writer_.End(pos);
                return tg.feedback_id;
            }
            if (!tg.depth && draw_rt) {
                float k = tg.gw ? float(tg.w) / float(tg.gw) : 1.0f;
                if (k > 1.01f && (tg.gw > draw_rt->gw || tg.gh > draw_rt->gh)) {
                    wide = k;
                    info.flags |= kTexWide;
                }
            }
            return tg.id;
        }

        // Guest texture upload/cache path
        StagePalette palette;
        ReadStagePalette(s, stage, d, palette);
        const uint64_t key = (uint64_t(palette.va) << 32) | d.data;
        const uint32_t* pal_colours = palette.va ? palette.colours : nullptr;

        const uint64_t frame_num = presents_ + 1u;
        const uint64_t ep = DirtyEpoch(d.data);
        auto it = textures_.find(key);
        const bool same_desc = (it != textures_.end() && SameTexDesc(it->second.d, d));

        if (same_desc && it->second.last_frame == frame_num && it->second.epoch == ep &&
            it->second.pal_hash == palette.hash) {
            return it->second.id;
        }

        const uint32_t total = TotalSize(d);
        uint64_t sig = 0;
        bool need_upload = !same_desc;
        sig = MemSig(d.data, total) ^ ep ^ palette.hash;
        if (!need_upload) {
            Tex& existing = it->second;
            existing.last_frame = frame_num;
            if (sig != existing.sig || existing.pal_hash != palette.hash) {
                need_upload = true;
            } else {
                existing.epoch = ep;
                return existing.id;
            }
        }

        if (same_desc) {
            // Re-upload into existing texture id
        } else {
            if (it != textures_.end()) DestroyTex(it->second.id);
            Tex tx{};
            tx.id = AllocId();
            tx.d = d;
            textures_[key] = tx;
            it = textures_.find(key);

            const bool use_hw_bc = fi.compressed && has_bc_ && ((d.width & 3u) == 0) && ((d.height & 3u) == 0);
            uint32_t fmt = kWgpuTexBgra8;
            if (fi.depth) fmt = kWgpuTexD24S8;
            else if (use_hw_bc) fmt = (fi.host == Host::BC1 ? kWgpuTexBc1 : (fi.host == Host::BC2 ? kWgpuTexBc2 : kWgpuTexBc3));

            const uint32_t dim = d.depth > 1 ? 1u : (d.cube ? 2u : 0u);
            uint32_t layers = d.cube ? 6u : d.depth;
            uint32_t levels = fi.depth ? 1u : std::max(1u, d.levels);
            const size_t pos = writer_.Begin(kOpCreateTexture);
            writer_.U32(tx.id);
            writer_.U32(fmt);
            writer_.U32(dim);
            writer_.U32(d.width);
            writer_.U32(d.height);
            writer_.U32(layers);
            writer_.U32(levels);
            writer_.U32(fi.depth ? 2u : 0u);
            writer_.End(pos);
        }

        Tex& tx = it->second;
        tx.epoch = ep;
        tx.sig = sig;
        tx.pal_hash = palette.hash;
        tx.last_frame = frame_num;

        const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        const uint32_t faces = d.cube ? 6u : 1u;
        const uint32_t levels = fi.depth ? 1u : std::max(1u, d.levels);
        const bool use_hw_bc = fi.compressed && has_bc_ && ((d.width & 3u) == 0) && ((d.height & 3u) == 0);

        uint32_t pitch = 0;
        for (uint32_t f = 0; f < faces; f++) {
            for (uint32_t l = 0; l < levels; l++) {
                uint32_t lw = std::max(1u, d.width >> l);
                uint32_t lh = std::max(1u, d.height >> l);
                uint32_t ld = d.depth > 1 ? std::max(1u, d.depth >> l) : 1u;
                DecodeLevel(d, l, f, guest, pal_colours, scratch_px_, pitch);
                if (fi.depth) {
                    // Upload R32F staging texture and splat frag_depth into depth24plus-stencil8
                    uint32_t tmp_id = AllocId();
                    {
                        const size_t p1 = writer_.Begin(kOpCreateTexture);
                        writer_.U32(tmp_id);
                        writer_.U32(kWgpuTexR32f);
                        writer_.U32(0u);
                        writer_.U32(lw);
                        writer_.U32(lh);
                        writer_.U32(1u);
                        writer_.U32(1u);
                        writer_.U32(0u);
                        writer_.End(p1);
                    }
                    {
                        const size_t p2 = writer_.Begin(kOpUploadTexture);
                        writer_.U32(tmp_id);
                        writer_.U32(0u);
                        writer_.U32(0u);
                        writer_.U32(lw);
                        writer_.U32(lh);
                        writer_.U32(1u);
                        writer_.U32(pitch);
                        writer_.U32(lh);
                        writer_.U32(uint32_t(scratch_px_.size()));
                        writer_.Bytes(scratch_px_.data(), scratch_px_.size());
                        writer_.End(p2);
                    }
                    {
                        const size_t p3 = writer_.Begin(kOpUploadDepth);
                        writer_.U32(tmp_id);
                        writer_.U32(tx.id);
                        writer_.U32(lw);
                        writer_.U32(lh);
                        writer_.End(p3);
                    }
                    DestroyTex(tmp_id);
                } else if (fi.compressed && !use_hw_bc) {
                    uint32_t bgra_pitch = 0;
                    DecodeBcToBgra(fi.host, lw, lh, ld, scratch_px_, pitch, scratch_bgra_, bgra_pitch);
                    const size_t p = writer_.Begin(kOpUploadTexture);
                    writer_.U32(tx.id);
                    writer_.U32(l);
                    writer_.U32(f);
                    writer_.U32(lw);
                    writer_.U32(lh);
                    writer_.U32(ld);
                    writer_.U32(bgra_pitch);
                    writer_.U32(lh);
                    writer_.U32(uint32_t(scratch_bgra_.size()));
                    writer_.Bytes(scratch_bgra_.data(), scratch_bgra_.size());
                    writer_.End(p);
                } else {
                    const uint32_t rows = fi.compressed ? std::max(1u, (lh + 3u) / 4u) : lh;
                    const size_t p = writer_.Begin(kOpUploadTexture);
                    writer_.U32(tx.id);
                    writer_.U32(l);
                    writer_.U32(f);
                    writer_.U32(lw);
                    writer_.U32(lh);
                    writer_.U32(ld);
                    writer_.U32(pitch);
                    writer_.U32(rows);
                    writer_.U32(uint32_t(scratch_px_.size()));
                    writer_.Bytes(scratch_px_.data(), scratch_px_.size());
                    writer_.End(p);
                }
            }
        }
        return tx.id;
    }

    uint32_t EnsureSampler(const State& s, int i, const TexInfo& info, bool clamp, bool fixed) {
        const auto& t = s.tss[size_t(i)];
        uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
        auto lin = [](uint32_t f) { return f >= kTexfLinear; };
        uint32_t minF = lin(min) ? 1u : 0u;
        uint32_t magF = lin(mag) ? 1u : 0u;
        uint32_t mipF = (mip == kTexfLinear) ? 1u : 0u;
        bool c = clamp;
        if (fixed && (((t[kTssTexCoordIndex] >> 16) & 7u) || (t[kTssTextureTransformFlags] & 7u))) c = false;
        uint32_t u = MapAddressMode((c && t[kTssAddressU] == kTaddressWrap) ? kTaddressClamp : t[kTssAddressU]);
        uint32_t v = MapAddressMode((c && t[kTssAddressV] == kTaddressWrap) ? kTaddressClamp : t[kTssAddressV]);
        uint32_t w = MapAddressMode(t[kTssAddressW]);
        uint32_t aniso = (min == kTexfAnisotropic || mag == kTexfAnisotropic) ? std::clamp(t[kTssMaxAnisotropy], 1u, 16u) : 1u;
        if (aniso > 1u && info.shadow) aniso = 1u;
        uint32_t cmpFunc = info.shadow ? (MapCmp0(s.rs[kRsShadowFunc]) + 1u) : 0u;
        if (cmpFunc) {
            minF = magF = (lin(min) || lin(mag)) ? 1u : 0u;
            mipF = 0u;
            u = v = w = 2u;  // clamp-to-edge for shadow comparison
        }
        float lodMin = float(t[kTssMaxMipLevel]);
        float lodMax = mip ? 32.0f : lodMin;

        const uint32_t key_words[9] = {u, v, w, minF, magF, mipF, aniso, cmpFunc, uint32_t(t[kTssMaxMipLevel])};
        const uint64_t kh = Hash(key_words, sizeof(key_words));
        auto it = samplers_.find(kh);
        if (it != samplers_.end()) return it->second;

        uint32_t id = AllocId();
        samplers_[kh] = id;
        const size_t pos = writer_.Begin(kOpCreateSampler);
        writer_.U32(id);
        writer_.U32(u);
        writer_.U32(v);
        writer_.U32(w);
        writer_.U32(minF);
        writer_.U32(magF);
        writer_.U32(mipF);
        writer_.U32(aniso);
        writer_.U32(cmpFunc);
        writer_.F32(lodMin);
        writer_.F32(lodMax);
        writer_.End(pos);
        return id;
    }

    uint32_t EnsureVertexShader(const State& s, const DrawShaders& sh) {
        auto it = vs_ids_.find(sh.vkey);
        if (it != vs_ids_.end()) return it->second;

        std::string wgsl = sh.prog
            ? gpu_webgpu::VertexProgramWgsl(s.vs->program, asm_.regs, sh.decode_wgpu, sh.sprite, sh.quad)
            : gpu_webgpu::FixedVertexWgsl(asm_.regs, asm_.pretransformed, sh.decode_wgpu, sh.fk, sh.quad);

        uint32_t id = AllocId();
        vs_ids_[sh.vkey] = id;
        web_stats::g_shaders_compiled.fetch_add(1, std::memory_order_relaxed);
        const size_t pos = writer_.Begin(kOpCreateShader);
        writer_.U32(id);
        writer_.U32(0u);
        writer_.U32(uint32_t(wgsl.size()));
        writer_.Bytes(wgsl.data(), wgsl.size());
        writer_.End(pos);
        return id;
    }

    uint32_t EnsurePixelShader(const PixelKey& pk, uint64_t pkey) {
        const uint64_t skey = pkey ^ kPixelKeySalt;
        auto it = ps_ids_.find(skey);
        if (it != ps_ids_.end()) return it->second;

        std::string wgsl = gpu_webgpu::PixelWgsl(pk);
        uint32_t id = AllocId();
        ps_ids_[skey] = id;
        web_stats::g_shaders_compiled.fetch_add(1, std::memory_order_relaxed);
        const size_t pos = writer_.Begin(kOpCreateShader);
        writer_.U32(id);
        writer_.U32(1u);
        writer_.U32(uint32_t(wgsl.size()));
        writer_.Bytes(wgsl.data(), wgsl.size());
        writer_.End(pos);
        return id;
    }

    uint32_t EnsurePipeline(const State& s, const Assembled& a, uint32_t vs_id, uint32_t fs_id, bool has_rt,
                            bool has_ds, const Target* ds, const uint32_t tex_kinds[4], uint32_t vb_count,
                            const auto* vbs, const std::array<VertexElement, 16>& elems, uint32_t strip_idx_fmt) {
        WebPsoKey k{};
        k.vs_id = vs_id;
        k.fs_id = fs_id;
        k.regs = a.regs;
        if (a.topology == Topology::Points) k.topo = 0;
        else if (a.topology == Topology::Lines) k.topo = a.strip ? 2 : 1;
        else k.topo = a.strip ? 4 : 3;
        k.strip_idx = a.strip ? uint8_t(strip_idx_fmt) : 0;

        uint32_t cull = s.rs[kRsCullMode];
        k.cull = (cull != 0 && a.topology == Topology::Triangles) ? 1 : 0;
        k.ccw = (cull == kCullCw) ? 1 : 0;
        k.has_rt = has_rt ? 1 : 0;
        k.has_ds = has_ds ? 1 : 0;

        k.blend_en = SimpleState(s, kMBlendEnable) ? 1 : 0;
        if (k.blend_en) {
            uint32_t sb = SimpleState(s, kMSrcBlend), db = SimpleState(s, kMDestBlend), bo = SimpleState(s, kMBlendOp);
            k.src_rgb = uint8_t(MapBlendFactor(sb, false));
            k.dst_rgb = uint8_t(MapBlendFactor(db, false));
            k.op_rgb = uint8_t(MapBlendOp(bo));
            k.src_a = uint8_t(MapBlendFactor(sb, true));
            k.dst_a = uint8_t(MapBlendFactor(db, true));
            k.op_a = k.op_rgb;
        }
        uint32_t cw = SimpleState(s, kMColorWriteEnable);
        k.wmask = uint8_t(((cw >> 16) & 1u) | (((cw >> 8) & 1u) << 1) | ((cw & 1u) << 2) | (((cw >> 24) & 1u) << 3));

        if (has_ds) {
            k.z_en = s.rs[kRsZEnable] ? 1 : 0;
            k.z_write = (k.z_en && SimpleState(s, kMZWriteEnable)) ? 1 : 0;
            k.z_func = k.z_en ? uint8_t(MapReversedCmp0(SimpleState(s, kMZFunc))) : 7;
            k.sten_en = s.rs[kRsStencilEnable] ? 1 : 0;
            if (k.sten_en) {
                k.sten_func = uint8_t(MapCmp0(SimpleState(s, kMStencilFunc)));
                k.sten_rmask = uint8_t(SimpleState(s, kMStencilMask) & 0xFFu);
                k.sten_wmask = uint8_t(SimpleState(s, kMStencilWriteMask) & 0xFFu);
                k.sten_fail = uint8_t(MapStencilOp(s.rs[kRsStencilFail]));
                k.sten_zfail = uint8_t(MapStencilOp(SimpleState(s, kMStencilZFail)));
                k.sten_pass = uint8_t(MapStencilOp(SimpleState(s, kMStencilPass)));
            }
            if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable)) {
                k.z_slope = -Bits(SimpleState(s, kMSlopeScale)) * (ds && ds->gw ? float(ds->w) / ds->gw : 1.0f);
            }
        }
        for (int i = 0; i < 4; i++) k.tex_kinds[i] = uint8_t(tex_kinds[i]);

        // Only serialize buffers up to vb_count that carry at least one active register
        k.vb_count = vb_count;
        for (uint32_t b = 0; b < vb_count; b++) k.strides[b] = vbs[b].stride;
        for (int r = 0; r < 16; r++) {
            if (!((a.regs >> r) & 1u)) continue;
            const VertexElement& e = elems[size_t(r)];
            k.attrs[r] = WgslVertexFmtCode(e.format) | (uint32_t(e.buffer) << 8) | (uint32_t(e.offset) << 16);
            k.attr_count[e.buffer]++;
        }

        const uint64_t kh = Hash(&k, sizeof(k));
        auto it = psos_.find(kh);
        if (it != psos_.end()) return it->second;

        uint32_t pso_id = AllocId();
        psos_[kh] = pso_id;

        const size_t pos = writer_.Begin(kOpCreatePipeline);
        writer_.U32(pso_id);
        writer_.U32(k.vs_id);
        writer_.U32(k.fs_id);
        writer_.U32(k.topo);
        writer_.U32(k.strip_idx);
        writer_.U32(k.cull);
        writer_.U32(k.ccw);
        writer_.U32(k.has_rt);
        writer_.U32(k.has_ds);
        writer_.U32(k.blend_en);
        writer_.U32(k.src_rgb);
        writer_.U32(k.dst_rgb);
        writer_.U32(k.op_rgb);
        writer_.U32(k.src_a);
        writer_.U32(k.dst_a);
        writer_.U32(k.op_a);
        writer_.U32(k.wmask);
        writer_.U32(k.z_en);
        writer_.U32(k.z_write);
        writer_.U32(k.z_func);
        writer_.U32(k.sten_en);
        writer_.U32(k.sten_rmask);
        writer_.U32(k.sten_wmask);
        writer_.U32(k.sten_fail);
        writer_.U32(k.sten_zfail);
        writer_.U32(k.sten_pass);
        writer_.U32(k.sten_func);
        writer_.I32(k.z_bias);
        writer_.F32(k.z_slope);
        for (int i = 0; i < 4; i++) writer_.U32(k.tex_kinds[i]);
        writer_.U32(vb_count);
        for (uint32_t b = 0; b < vb_count; b++) {
            writer_.U32(std::max(4u, k.strides[b]));
            writer_.U32(k.attr_count[b]);
            for (int r = 0; r < 16; r++) {
                if (!((a.regs >> r) & 1u)) continue;
                if (((k.attrs[r] >> 8) & 0xFFu) != b) continue;
                writer_.U32(uint32_t(r));
                writer_.U32(k.attrs[r] & 0xFFu);
                writer_.U32(k.attrs[r] >> 16);
            }
        }
        writer_.End(pos);
        return pso_id;
    }

    uint32_t guest_w_ = 640, guest_h_ = 480;
    float scale_ = 1.0f;
    bool has_bc_ = false;
    uint32_t next_id_ = 1;
    uint64_t presents_ = 0;
    uint8_t ramp_[768]{};
    bool ramp_dirty_ = false, gamma_ = false;

    uint32_t init_flag_ = 0;
    uint32_t caps_ = 0;
    uint32_t sync_flag_ = 0;
    uint32_t flush_flag_ = 0;

    BatchWriter writer_;
    bool has_batch_vsc_ = false, has_batch_dc_ = false;
    std::array<std::array<float, 4>, 192> last_vsc_{};
    DrawConstants last_dc_{};

    Assembled asm_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    std::unordered_map<uint64_t, uint32_t> vs_ids_;
    std::unordered_map<uint64_t, uint32_t> ps_ids_;
    std::unordered_map<uint64_t, uint32_t> psos_;

    std::vector<int32_t> scratch_rects_;
    std::vector<float> scratch_verts_;
    std::vector<uint8_t> scratch_px_, scratch_bgra_;
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<WebGpuBackend>(); }

}  // namespace gpu

extern "C" {

// Exposes helper WGSL shaders to web/webgpu_renderer.js during init without duplicating WGSL source strings.
const char* BlackWebGetBuiltinWgsl(uint32_t kind) {
    static std::string blit = gpu_webgpu::PresentBlitWgsl();
    static std::string clear = gpu_webgpu::ClearQuadWgsl();
    static std::string depth = gpu_webgpu::DepthUploadWgsl();
    if (kind == 0) return blit.c_str();
    if (kind == 1) return clear.c_str();
    return depth.c_str();
}

}  // extern "C"
