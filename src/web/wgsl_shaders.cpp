#include "web/wgsl_shaders.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gpu/vertex.h"

namespace gpu_webgpu {

namespace {

std::string F(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const char* kCommonStruct = R"(
struct VOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) d0 : vec4<f32>,
    @location(1) d1 : vec4<f32>,
    @location(2) b0 : vec4<f32>,
    @location(3) b1 : vec4<f32>,
    @location(4) tc0 : vec4<f32>,
    @location(5) tc1 : vec4<f32>,
    @location(6) tc2 : vec4<f32>,
    @location(7) tc3 : vec4<f32>,
    @location(8) fogamt : f32,
    @location(9) @interpolate(linear) e : vec4<f32>,
};

struct DrawConstants {
    surf : vec4<f32>,
    fogp : vec4<f32>,
    fogcolor : vec4<f32>,
    misc : vec4<f32>,
    pc : array<vec4<f32>, 18>,
    tfactor : vec4<f32>,
    texsize : array<vec4<f32>, 4>,
    bumpmat : array<vec4<f32>, 4>,
    bumplum : array<vec4<f32>, 4>,
    wvp : mat4x4<f32>,
    ext : vec4<f32>,
    spread : array<vec4<f32>, 4>,
    wv : mat4x4<f32>,
    wvn : mat4x4<f32>,
    tm : array<mat4x4<f32>, 4>,
    mat : array<vec4<f32>, 5>,
    amb : vec4<f32>,
    lt : array<vec4<f32>, 56>,
    rtshift : vec4<f32>,
    zparams : vec4<f32>,
};

@group(0) @binding(0) var<uniform> vc : array<vec4<f32>, 192>;
@group(0) @binding(1) var<uniform> dc : DrawConstants;
)";

const char* kVsHelpers = R"(
fn mac_mul(a: vec4<f32>, b: vec4<f32>) -> vec4<f32> {
    return select(a * b, vec4<f32>(0.0), (a == vec4<f32>(0.0)) | (b == vec4<f32>(0.0)));
}
fn mac_arl(v: f32) -> i32 {
    return i32(floor(v + 0.001));
}
fn magclamp(v: f32) -> f32 {
    let m = clamp(abs(v), 5.42101e-20, 1.84467e+19);
    return select(m, -m, v < 0.0);
}
fn ilu_rcp(v: f32) -> vec4<f32> {
    return vec4<f32>(1.0 / v);
}
fn ilu_rcc(v: f32) -> vec4<f32> {
    return vec4<f32>(magclamp(1.0 / v));
}
fn ilu_rsq(v: f32) -> vec4<f32> {
    return select(vec4<f32>(inverseSqrt(max(abs(v), 1e-38))), vec4<f32>(3.402823e+38), v == 0.0);
}
fn ilu_exp(v: f32) -> vec4<f32> {
    let whole = floor(v);
    return vec4<f32>(exp2(whole), v - whole, exp2(v), 1.0);
}
fn ilu_log(v: f32) -> vec4<f32> {
    let mag = abs(v);
    let l2 = log2(max(mag, 1e-38));
    let ex = floor(l2);
    return select(vec4<f32>(ex, mag / exp2(ex), l2, 1.0), vec4<f32>(-3.402823e+38, 1.0, -3.402823e+38, 1.0), mag == 0.0);
}
fn ilu_lit(s: vec4<f32>) -> vec4<f32> {
    let cap = 127.99609375;
    let diffuse = max(s.x, 0.0);
    let spec = exp2(clamp(s.w, -cap, cap) * log2(max(s.y, 1e-38)));
    return vec4<f32>(1.0, diffuse, select(0.0, spec, diffuse > 0.0), 1.0);
}
fn fogfactor(d: f32) -> f32 {
    if (dc.fogp.w == 3.0) { return (dc.fogp.y - d) / max(dc.fogp.y - dc.fogp.x, 1e-6); }
    if (dc.fogp.w == 1.0) { return exp(-d * dc.fogp.z); }
    if (dc.fogp.w == 2.0) { return exp(-(d * dc.fogp.z) * (d * dc.fogp.z)); }
    return 1.0;
}
fn ReversedClip(wz: f32, w: f32) -> f32 {
    if (dc.zparams.x != 0.0) { return dc.zparams.x * (dc.zparams.y - (w + dc.zparams.z)); }
    return wz - dc.zparams.z * w;
}
)";

struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

const char* kMask[16] = {"",   "w",  "z",  "zw",  "y",   "yw",  "yz",  "yzw",
                         "x",  "xw", "xz", "xzw", "xy",  "xyw", "xyz", "xyzw"};
const char* kLanes[5] = {"", "x", "xy", "xyz", "xyzw"};
const char* kOut[16] = {"oPos", "", "", "oD0", "oD1", "oFog", "oPts", "oB0",
                        "oB1",  "oT0", "oT1", "oT2", "oT3", "", "", ""};

constexpr uint32_t kLaneZ = 2;
constexpr uint32_t kLaneW = 1;
constexpr uint32_t kMacDph = 6;
constexpr uint32_t kMacDp4 = 7;
constexpr uint32_t kMacArl = 13;
constexpr uint32_t kViewportScaleConstant = 58 + 96;
constexpr uint32_t kViewportOffsetConstant = 59 + 96;

bool ReadsConstant(const Tok& k, uint32_t index) {
    for (int i = 0; i < 3; i++) {
        uint32_t mux = i == 0 ? k.f(2, 26, 2) : i == 1 ? k.f(2, 11, 2) : k.f(3, 28, 2);
        if (mux == 3 && !k.f(3, 1, 1) && k.f(1, 13, 8) == index) return true;
    }
    return false;
}

struct PosLanes {
    uint32_t dot = 0;
    uint32_t other = 0;
};

PosLanes WrittenPosLanes(const Tok& k) {
    uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
    uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
    bool mac_writes = mac && mac != kMacArl;
    uint32_t mac_lanes = mac_writes && r == 12 ? k.f(3, 24, 4) : 0;
    uint32_t ilu_lanes = ilu && (mac ? 1 : r) == 12 ? k.f(3, 16, 4) : 0;
    if (omask && k.f(3, 11, 1) && (k.f(3, 3, 8) & 0xF) == 0) {
        bool use_ilu = k.f(3, 2, 1) == 1;
        if (use_ilu && ilu) ilu_lanes |= omask;
        if (!use_ilu && mac_writes) mac_lanes |= omask;
    }
    bool dot = mac == kMacDph || mac == kMacDp4;
    return {dot ? mac_lanes : 0, ilu_lanes | (dot ? 0 : mac_lanes)};
}

bool SeparableClipDepth(const std::vector<uint32_t>& prog) {
    int z = 0, w = 0;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        PosLanes p = WrittenPosLanes(k);
        bool viewport = ReadsConstant(k, kViewportScaleConstant) || ReadsConstant(k, kViewportOffsetConstant);
        if ((p.dot & (kLaneZ | kLaneW)) == (kLaneZ | kLaneW) || (p.other & kLaneW)) return false;
        if ((p.other & kLaneZ) && !viewport) return false;
        z += (p.dot & kLaneZ) != 0;
        w += (p.dot & kLaneW) != 0;
        if (k.f(3, 0, 1)) break;
    }
    return z == 1 && w == 1;
}

std::string ClipSnapshot(const Tok& k, const std::string& a, const std::string& b) {
    PosLanes p = WrittenPosLanes(k);
    std::string v = k.f(1, 21, 4) == kMacDph ? "vec4<f32>((" + a + ").xyz, 1.0)" : a;
    if (p.dot & kLaneZ) return "        clip_zv = " + v + ";\n        clip_zr = " + b + ";\n";
    if (p.dot & kLaneW) return "        clip_wv = " + v + ";\n        clip_wr = " + b + ";\n";
    return "";
}

std::string Src(const Tok& k, int which, uint16_t regs) {
    uint32_t neg, sw[4], reg, mux;
    if (which == 0) {
        neg = k.f(1, 8, 1);
        sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
        reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        neg = k.f(2, 25, 1);
        sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
        reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        neg = k.f(2, 10, 1);
        sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
        reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];
    }
    std::string base;
    if (mux == 1) {
        base = reg == 12 ? "oPos" : F("R%u", reg);
    } else if (mux == 2) {
        uint32_t v = k.f(1, 9, 4);
        base = ((regs >> v) & 1) ? F("vi.v%u", v) : (v == 3 ? "vec4<f32>(1.0)" : "vec4<f32>(0.0, 0.0, 0.0, 1.0)");
    } else {
        uint32_t cr = std::min<uint32_t>(k.f(1, 13, 8), 191u);
        base = k.f(3, 1, 1) ? F("vc[u32(clamp(a0 + %u, 0, 191))]", cr) : F("vc[%uu]", cr);
    }
    const char* xyzw = "xyzw";
    return F("(%s(%s).%c%c%c%c)", neg ? "-" : "", base.c_str(), xyzw[sw[0]], xyzw[sw[1]], xyzw[sw[2]], xyzw[sw[3]]);
}

std::string MacExpr(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "vec4<f32>(dot((" + a + ").xyz, (" + b + ").xyz))";
        case 6: return "vec4<f32>(dot(vec4<f32>((" + a + ").xyz, 1.0), " + b + "))";
        case 7: return "vec4<f32>(dot(" + a + ", " + b + "))";
        case 8: return "vec4<f32>(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "select(vec4<f32>(0.0), vec4<f32>(1.0), (" + a + ") < (" + b + "))";
        case 12: return "select(vec4<f32>(0.0), vec4<f32>(1.0), (" + a + ") >= (" + b + "))";
        case 13: return a;
        default: return "vec4<f32>(0.0)";
    }
}

std::string IluExpr(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp((" + c + ").x)";
        case 3: return "ilu_rcc((" + c + ").x)";
        case 4: return "ilu_rsq((" + c + ").x)";
        case 5: return "ilu_exp((" + c + ").x)";
        case 6: return "ilu_log((" + c + ").x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "vec4<f32>(0.0)";
    }
}

std::string RegDest(uint32_t r) { return r == 12 ? "oPos" : F("R%u", r); }

std::string VsInputs(uint16_t regs, uint64_t decode) {
    if (!regs) {
        return "struct VIn {\n"
               "    @builtin(vertex_index) id : u32,\n"
               "};\n"
               "@vertex fn vs_main(vi: VIn) -> VOut {\n";
    }
    if (!decode) {
        std::string s = "struct VIn {\n";
        for (int r = 0; r < 16; r++) {
            if ((regs >> r) & 1) s += F("    @location(%d) v%d : vec4<f32>,\n", r, r);
        }
        return s + "};\n@vertex fn vs_main(vi: VIn) -> VOut {\n";
    }
    std::string raw = "struct VRaw {\n", in = "struct VIn {\n", body;
    bool packed = false;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = uint32_t(decode >> (r * 4)) & 15u;
        const char* type = (d == gpu::kDecodeInt || d == gpu::kDecodeIntW1) ? "vec4<i32>"
                         : (d == gpu::kDecodePacked)                         ? "u32"
                                                                             : "vec4<f32>";
        raw += F("    @location(%d) v%d : %s,\n", r, r, type);
        in += F("    v%d : vec4<f32>,\n", r);
        packed |= (d == gpu::kDecodePacked);
        switch (d) {
            case gpu::kDecodeW1: body += F("    vi.v%d = vec4<f32>(ri.v%d.xyz, 1.0);\n", r, r); break;
            case gpu::kDecodeInt: body += F("    vi.v%d = vec4<f32>(ri.v%d);\n", r, r); break;
            case gpu::kDecodeIntW1: body += F("    vi.v%d = vec4<f32>(vec3<f32>(ri.v%d.xyz), 1.0);\n", r, r); break;
            case gpu::kDecodePacked: body += F("    vi.v%d = UnpackNp3(ri.v%d);\n", r, r); break;
            case gpu::kDecodeFloat2H: body += F("    vi.v%d = vec4<f32>(ri.v%d.xy, 0.0, ri.v%d.z);\n", r, r, r); break;
            case kWebDecodeBgra: body += F("    vi.v%d = ri.v%d.zyxw;\n", r, r); break;
            default: body += F("    vi.v%d = ri.v%d;\n", r, r); break;
        }
    }
    std::string s = raw + "};\n" + in + "};\n";
    if (packed) {
        s += "fn UnpackNp3(u: u32) -> vec4<f32> {\n"
             "    let sx = (i32(u << 21u) >> 21u);\n"
             "    let sy = (i32(u << 10u) >> 21u);\n"
             "    let sz = (i32(u) >> 22u);\n"
             "    let v = vec3<f32>(f32(sx), f32(sy), f32(sz));\n"
             "    return vec4<f32>(max(v / vec3<f32>(1023.0, 1023.0, 511.0), vec3<f32>(-1.0)), 1.0);\n"
             "}\n";
    }
    return s + "@vertex fn vs_main(ri: VRaw) -> VOut {\n    var vi : VIn;\n" + body;
}

const char* kVsPrologue = R"(
    var a0 : i32 = 0;
    var R0 = vec4<f32>(0.0); var R1 = vec4<f32>(0.0); var R2 = vec4<f32>(0.0); var R3 = vec4<f32>(0.0);
    var R4 = vec4<f32>(0.0); var R5 = vec4<f32>(0.0); var R6 = vec4<f32>(0.0); var R7 = vec4<f32>(0.0);
    var R8 = vec4<f32>(0.0); var R9 = vec4<f32>(0.0); var R10 = vec4<f32>(0.0); var R11 = vec4<f32>(0.0);
    var oPos = vec4<f32>(0.0);
    var clip_zv = vec4<f32>(0.0);
    var clip_zr = vec4<f32>(0.0);
    var clip_wv = vec4<f32>(1.0);
    var clip_wr = vec4<f32>(0.0);
    var oD0 = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var oD1 = vec4<f32>(0.0);
    var oB0 = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var oB1 = vec4<f32>(0.0);
    var oFog = vec4<f32>(0.0);
    var oPts = vec4<f32>(1.0);
    var oT0 = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var oT1 = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var oT2 = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var oT3 = vec4<f32>(0.0, 0.0, 0.0, 1.0);
)";

std::string VsEpilogue(bool sprite, bool quad) {
    std::string s = "\n    var o : VOut;\n    var p = oPos;\n"
                    "    p.w = select(magclamp(p.w), 1.0, p.w == 0.0);\n"
                    "    o.e = vec4<f32>(0.0);\n";
    if (quad) s += "    o.e = vec4<f32>(p.xy, p.xy);\n";
    s += "    if (dc.surf.w == 0.0 && abs(p.x - (dc.surf.x - 0.5)) < 1e-3) { p.x = dc.surf.x; }\n"
         "    if (dc.surf.w == 0.0 && abs(p.y - (dc.surf.y - 0.5)) < 1e-3) { p.y = dc.surf.y; }\n";
    if (quad) s += "    o.e = vec4<f32>(o.e.xy, p.xy);\n";
    s += "    var q : vec4<f32>;\n"
         "    q.x = p.x * 2.0 / dc.surf.x - 1.0;\n"
         "    q.y = 1.0 - p.y * 2.0 / dc.surf.y;\n"
         "    q.z = p.z * dc.surf.z;\n"
         "    q = vec4<f32>(q.xyz * p.w, p.w);\n"
         "    var wz : f32 = q.w - q.z;\n"
         "    if (all(clip_zv == clip_wv)) {\n"
         "        let scale = vc[58].z * dc.surf.z;\n"
         "        wz = scale * dot(clip_wv, clip_wr - clip_zr) + q.w * (1.0 - scale - vc[59].z * dc.surf.z);\n"
         "    }\n"
         "    o.pos = q;\n"
         "    o.pos.z = ReversedClip(wz, q.w);\n"
         "    o.d0 = saturate(oD0);\n"
         "    o.d1 = saturate(oD1);\n"
         "    o.b0 = saturate(oB0);\n"
         "    o.b1 = saturate(oB1);\n"
         "    o.tc0 = oT0;\n"
         "    o.tc1 = oT1;\n"
         "    o.tc2 = oT2;\n";
    s += sprite ? "    o.tc3 = oPts;\n" : "    o.tc3 = oT3;\n";
    s += "    o.fogamt = fogfactor(oFog.x);\n"
         "    return o;\n}\n";
    return s;
}

// Pixel shader combiner helpers
struct In {
    uint32_t reg, chan, mod;
};
In ParseIn(uint32_t v) { return {v & 0xFu, v & 0x10u, v & 0xE0u}; }

struct Ps {
    const gpu::PixelKey& k;
    int stage = 0;
    std::string e, f;
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;
    std::string code;

    std::string Var(uint32_t reg) {
        uint32_t flags = k.count >> 8;
        switch (reg) {
            case 0: return "vec4<f32>(0.0)";
            case 1: return F("dc.pc[%d]", stage == 8 ? 16 : ((flags & 0x10u) ? stage * 2 : 0));
            case 2: return F("dc.pc[%d]", stage == 8 ? 17 : ((flags & 0x100u) ? stage * 2 + 1 : 1));
            case 3: return "fogin";
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            case 14:
                return F("%s(vec4<f32>((%s).rgb + (%s).rgb, 0.0))", clamp_sum ? "saturate" : "",
                         inv_v1 ? "(vec4<f32>(1.0) - v1)" : "v1", inv_r0 ? "(vec4<f32>(1.0) - r0)" : "r0");
            case 15: return "vec4<f32>((" + e + ") * (" + f + "), 0.0)";
            default: return "vec4<f32>(0.0)";
        }
    }

    std::string Input(In in, bool alpha) {
        std::string r = Var(in.reg);
        if (!alpha) r = "(" + r + ")" + (in.chan ? ".aaa" : ".rgb");
        else r = "(" + r + ")" + (in.chan ? ".a" : ".b");
        const char* z = alpha ? "0.0" : "vec3<f32>(0.0)";
        const char* o = alpha ? "1.0" : "vec3<f32>(1.0)";
        const char* h = alpha ? "0.5" : "vec3<f32>(0.5)";
        switch (in.mod) {
            case 0x00: return "max(" + r + ", " + z + ")";
            case 0x20: return "(" + std::string(o) + " - saturate(" + r + "))";
            case 0x40: return "(2.0 * max(" + r + ", " + z + ") - " + o + ")";
            case 0x60: return "(-2.0 * max(" + r + ", " + z + ") + " + o + ")";
            case 0x80: return "(max(" + r + ", " + z + ") - " + h + ")";
            case 0xA0: return "(-max(" + r + ", " + z + ") + " + h + ")";
            case 0xC0: return r;
            default: return "(-" + r + ")";
        }
    }

    static std::string Map(const std::string& v, uint32_t m) {
        switch (m) {
            case 0x08: return "(" + v + " - 0.5)";
            case 0x10: return "(" + v + " * 2.0)";
            case 0x18: return "((" + v + " - 0.5) * 2.0)";
            case 0x20: return "(" + v + " * 4.0)";
            case 0x30: return "(" + v + " / 2.0)";
            default: return v;
        }
    }

    static const char* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            default: return nullptr;
        }
    }

    std::string Stage(uint32_t inputs, uint32_t output, bool alpha) {
        In d = ParseIn(inputs & 0xFFu), c = ParseIn((inputs >> 8) & 0xFFu), b = ParseIn((inputs >> 16) & 0xFFu),
           a = ParseIn(inputs >> 24);
        uint32_t cd_dst = output & 0xFu, ab_dst = (output >> 4) & 0xFu, sum_dst = (output >> 8) & 0xFu, fl = output >> 12;
        const char* T = alpha ? "f32" : "vec3<f32>";
        const char* n = alpha ? "a" : "c";
        std::string A = Input(a, alpha), B = Input(b, alpha), C = Input(c, alpha), D = Input(d, alpha);
        std::string ab = (fl & 2u) ? (alpha ? "(" + A + " * " + B + ")" : "vec3<f32>(dot(" + A + ", " + B + "))")
                                   : "(" + A + " * " + B + ")";
        std::string cd = (fl & 1u) ? (alpha ? "(" + C + " * " + D + ")" : "vec3<f32>(dot(" + C + ", " + D + "))")
                                   : "(" + C + " * " + D + ")";
        uint32_t map = fl & 0x38u;
        code += F("    let ab%s : %s = clamp(%s(%s), %s(-1.0), %s(1.0));\n", n, T, T, Map(ab, map).c_str(), T, T);
        code += F("    let cd%s : %s = clamp(%s(%s), %s(-1.0), %s(1.0));\n", n, T, T, Map(cd, map).c_str(), T, T);
        std::string mux_cond = ((k.count >> 8) & 1u) ? "r0.a >= 0.5" : "((u32(max(r0.a, 0.0) * 255.0) & 1u) == 1u)";
        std::string sum = (fl & 4u) ? F("select(%s(%s), %s(%s), %s)", T, ab.c_str(), T, cd.c_str(), mux_cond.c_str())
                                    : "(" + ab + " + " + cd + ")";
        code += F("    let ms%s : %s = clamp(%s(%s), %s(-1.0), %s(1.0));\n", n, T, T, Map(sum, map).c_str(), T, T);
        std::string post;
        auto write_dst = [&](const char* r, const char* prefix, bool copy_b_to_a) {
            if (alpha) {
                post += F("    %s.a = %s%s;\n", r, prefix, n);
            } else {
                // WGSL forbids multi-component swizzle writes on LHS (`r0.rgb = ...`).
                std::string alpha_src = copy_b_to_a ? F("%sc.b", prefix) : F("%s.a", r);
                post += F("    %s = vec4<f32>(%s%s, %s);\n", r, prefix, n, alpha_src.c_str());
            }
        };
        if (const char* r = Dest(ab_dst)) write_dst(r, "ab", (fl & 0x80u) != 0);
        if (const char* r = Dest(cd_dst)) write_dst(r, "cd", (fl & 0x40u) != 0);
        if (const char* r = Dest(sum_dst)) write_dst(r, "ms", false);
        return post;
    }
};

std::string DotInput(uint32_t map, uint32_t src) {
    switch (map) {
        case 1: return F("sbyte(t%u.rgb, 128.0, 128.0, 127.0)", src);
        case 2: return F("sbyte(t%u.rgb, -0.5, 255.5, 127.5)", src);
        case 3: return F("sbyte(t%u.rgb, 0.0, 256.0, 127.0)", src);
        default: return F("t%u.rgb", src);
    }
}

std::string TexUv(const gpu::TexInfo& t, int i, const std::string& uv) {
    std::string n = "(" + uv + ")";
    if (t.rect) n += F(" / dc.texsize[%d].xy", i);
    n += F(" + dc.texsize[%d].zw", i);
    if (!(t.flags & gpu::kTexTarget)) return n;
    return n + F(" + dc.rtshift[%d] * (dpdx(%s) + dpdy(%s))", i, n.c_str(), n.c_str());
}

std::string TexFetch(const gpu::PixelKey& k, int i, std::string& kill_code) {
    uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1Fu;
    const gpu::TexInfo& t = k.tex[i];
    uint32_t in = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xFu : (k.input_texture >> 20) & 0xFu;
    uint32_t dm = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xFu;
    const std::string tc = F("tc%d", i);
    const char* q = tc.c_str();
    auto put = [&](const std::string& v) { return F("    var t%d : vec4<f32> = %s;\n", i, v.c_str()); };
    auto dp = [&](int n, int coord, uint32_t map, uint32_t src) {
        return F("    let dp%d%s : f32 = dot(tc%d.xyz, %s);\n", n, coord == n ? "" : "n", coord, DotInput(map, src).c_str());
    };
    auto tex2d = [&](const std::string& uv) {
        if (t.flags & gpu::kTexWide) return F("spreadtap(tex%d, smp%d, %s, dc.spread[%d])", i, i, uv.c_str(), i);
        return F("textureSample(tex%d, smp%d, %s)", i, i, uv.c_str());
    };
    auto cube = [&](const std::string& dir) { return F("textureSample(tex%d, smp%d, %s)", i, i, dir.c_str()); };
    auto sample2 = [&](const std::string& uv) {
        if (t.kind == gpu::kTex2D) return tex2d(TexUv(t, i, uv));
        if (t.kind == gpu::kTexCube) return cube("vec3<f32>(" + uv + ", 1.0)");
        if (t.kind == gpu::kTex3D) return F("textureSample(tex%d, smp%d, vec3<f32>(%s, 0.0))", i, i, uv.c_str());
        return std::string("vec4<f32>(0.0, 0.0, 0.0, 1.0)");
    };
    auto sample3 = [&](const std::string& uvw) {
        if (t.kind == gpu::kTex2D) return tex2d(TexUv(t, i, "(" + uvw + ").xy"));
        if (t.kind == gpu::kTexCube) return cube(uvw);
        if (t.kind == gpu::kTex3D) return F("textureSample(tex%d, smp%d, %s)", i, i, uvw.c_str());
        return std::string("vec4<f32>(0.0, 0.0, 0.0, 1.0)");
    };
    auto depth = [&](const std::string& ref) {
        std::string uv = TexUv(t, i, F("%s.xy / %s.w", q, q));
        return put(F("shadow_sample(tex%d, smp%d, %s, %s, %d)", i, i, uv.c_str(), ref.c_str(), t.shadow));
    };

    std::string s;
    switch (mode) {
        case 0: return put("vec4<f32>(0.0, 0.0, 0.0, 1.0)");
        case 1: return t.shadow ? depth("0.0") : put(sample2(F("%s.xy / %s.w", q, q)));
        case 2: return t.shadow ? depth(F("%s.z / %s.w", q, q)) : put(sample3(F("%s.xyz / %s.w", q, q)));
        case 3: return put(sample3(tc + ".xyz"));
        case 4: return put(tc);
        case 5:
            s = put("vec4<f32>(0.0)");
            for (int j = 0; j < 4; j++) {
                kill_code += F("    if (%s.%c %s 0.0) { kill_frag = true; }\n", q, "xyzw"[j],
                               ((k.compare_mode >> (i * 4 + j)) & 1u) ? ">=" : "<");
            }
            return s;
        case 6:
        case 7:
            s = F("    let duv%d = sbyte(t%u.bgr, 0.0, 256.0, 127.0).xy;\n", i, in);
            s += put(sample2(F("%s.xy + vec2<f32>(dot(dc.bumpmat[%d].xz, duv%d), dot(dc.bumpmat[%d].yw, duv%d))", q, i, i,
                               i, i)));
            if (mode == 7) s += F("    t%d = t%d * (dc.bumplum[%d].x * t%u.r + dc.bumplum[%d].y);\n", i, i, i, in, i);
            return s;
        case 9: return dp(i, i, dm, in) + put(sample2(F("vec2<f32>(dp%d, dp%d)", i - 1, i)));
        case 0xA:
        case 0x11: return dp(i, i, dm, in) + put("vec4<f32>(0.0)");
        case 0xB:
            s = dp(i, i, dm, in) + dp(i, i + 1, (k.dot_mapping >> 8) & 0xFu, (k.input_texture >> 20) & 0xFu);
            return s + put(sample3(F("vec3<f32>(dp%d, dp%d, dp%dn)", i - 1, i, i)));
        case 0xC:
            s = dp(i, i, dm, in);
            s += F("    let nrm%d = vec3<f32>(dp%d, dp%d, dp%d);\n"
                   "    let eye%d = vec3<f32>(tc%d.w, tc%d.w, tc%d.w);\n"
                   "    let refl%d = 2.0 * nrm%d * dot(nrm%d, eye%d) / max(dot(nrm%d, nrm%d), 1e-6) - eye%d;\n",
                   i, i - 2, i - 1, i, i, i - 2, i - 1, i, i, i, i, i, i, i, i);
            return s + put(sample3(F("refl%d", i)));
        case 0xD:
        case 0xE: return dp(i, i, dm, in) + put(sample3(F("vec3<f32>(dp%d, dp%d, dp%d)", i - 2, i - 1, i)));
        case 0xF: return put(sample2(F("t%u.ar", in)));
        case 0x10: return put(sample2(F("t%u.gb", in)));
        default: return put("vec4<f32>(0.0)");
    }
}

std::string FfArg(uint32_t a, int stage, bool alpha) {
    std::string v;
    switch (a & 0xFu) {
        case 0: v = "v0"; break;
        case 1: v = "cur"; break;
        case 2: v = F("t%d", stage); break;
        case 3: v = "dc.tfactor"; break;
        case 4: v = "v1"; break;
        case 5: v = "tmp"; break;
        default: v = "cur"; break;
    }
    if (a & 0x20u) v = "(" + v + ").aaaa";
    if (a & 0x10u) v = "(vec4<f32>(1.0) - " + v + ")";
    return "(" + v + ")" + (alpha ? ".a" : ".rgb");
}

std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2, const std::string& a0, int stage, bool alpha) {
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2.0";
        case 6: return a1 + " * " + a2 + " * 4.0";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + " - 0.5";
        case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " * (1.0 - " + a1 + ")";
        case 12: return "mix(" + a2 + ", " + a1 + ", v0.a)";
        case 13: return "mix(" + a2 + ", " + a1 + ", cur.a)";
        case 14: return F("mix(%s, %s, t%d.a)", a2.c_str(), a1.c_str(), stage);
        case 15: return "mix(" + a2 + ", " + a1 + ", dc.tfactor.a)";
        case 16: return F("%s + %s * (1.0 - t%d.a)", a1.c_str(), a2.c_str(), stage);
        case 18: return alpha ? a1 : a1 + " + " + a2 + " * " + a1.substr(0, a1.size() - 4) + ".a";
        case 19: return alpha ? a1 : a1 + " * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 20: return alpha ? a1 : a1 + " + " + a2 + " * (1.0 - " + a1.substr(0, a1.size() - 4) + ".a)";
        case 21: return alpha ? a1 : "(1.0 - " + a1 + ") * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 22:
            return F("vec3<f32>(saturate(dot((%s - 0.5) * 2.0, (%s - 0.5) * 2.0)))",
                     alpha ? ("vec3<f32>(" + a1 + ", 0.0, 0.0)").c_str() : a1.c_str(),
                     alpha ? ("vec3<f32>(" + a2 + ", 0.0, 0.0)").c_str() : a2.c_str());
        case 23: return a0 + " + " + a1 + " * " + a2;
        case 24: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";
    }
}

const char* kPsHelpers = R"(
fn sbyte(u: vec3<f32>, lo: f32, hi: f32, d: f32) -> vec3<f32> {
    let b = u * 255.0;
    return (b - select(vec3<f32>(lo), vec3<f32>(hi), b >= vec3<f32>(128.0))) / d;
}
fn shadow_sample(t: texture_depth_2d, s: sampler_comparison, uv: vec2<f32>, z: f32, fmt: i32) -> vec4<f32> {
    let mx = select(16777215.0, 65535.0, fmt == 2);
    let r = 1.0 - clamp(z, 0.0, mx) / mx;
    return vec4<f32>(textureSampleCompareLevel(t, s, uv, r));
}
fn spreadtap(t: texture_2d<f32>, s: sampler, uv: vec2<f32>, w: vec4<f32>) -> vec4<f32> {
    var c = vec4<f32>(0.0);
    let n = clamp(i32(w.z), 1, 8);
    for (var y = 0; y < n; y = y + 1) {
        for (var x = 0; x < n; x = x + 1) {
            let off = vec2<f32>(f32(x), f32(y)) - vec2<f32>((f32(n) - 1.0) * 0.5);
            let v = textureSampleLevel(t, s, uv + w.xy * off, 0.0);
            c = c + vec4<f32>(saturate(v.rgb), v.a);
        }
    }
    return c / f32(n * n);
}
fn alphatest_pass(a: f32) -> bool {
    let f = i32(dc.misc.y);
    let v = i32(round(a * 255.0));
    let r = i32(dc.misc.x);
    if (f == 0) { return false; }
    if (f == 1) { return v < r; }
    if (f == 2) { return v == r; }
    if (f == 3) { return v <= r; }
    if (f == 4) { return v > r; }
    if (f == 5) { return v != r; }
    if (f == 6) { return v >= r; }
    return true;
}
fn unstretch(t: vec4<f32>, e: vec4<f32>) -> vec4<f32> {
    let d = e.zw - e.xy;
    let g = vec2<f32>(dpdx(e.x), dpdy(e.y));
    let dt_x = dpdx(t);
    let dt_y = dpdy(t);
    let sx = select(vec4<f32>(0.0), dt_x * (d.x / g.x), d.x != 0.0 && g.x != 0.0);
    let sy = select(vec4<f32>(0.0), dt_y * (d.y / g.y), d.y != 0.0 && g.y != 0.0);
    return t + sx + sy;
}
)";

// WGSL forbids multi-component swizzle assignments on LHS (e.g. `oPos.xyz = mr.xyz;`).
std::string AssignMask(const std::string& dst, const char* msk, const char* src) {
    if (!msk || !msk[0]) return {};
    if (!msk[1]) return F("        %s.%c = %s.%c;\n", dst.c_str(), msk[0], src, msk[0]);
    auto has = [&](char c) {
        for (const char* p = msk; *p; ++p)
            if (*p == c) return true;
        return false;
    };
    if (has('x') && has('y') && has('z') && has('w')) return F("        %s = %s;\n", dst.c_str(), src);
    auto pick = [&](char c) { return F("%s.%c", has(c) ? src : dst.c_str(), c); };
    return F("        %s = vec4<f32>(%s, %s, %s, %s);\n", dst.c_str(),
             pick('x').c_str(), pick('y').c_str(), pick('z').c_str(), pick('w').c_str());
}

}  // namespace

std::string VertexProgramWgsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode, bool sprite,
                              bool quad) {
    std::string s = kCommonStruct;
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    bool separable = SeparableClipDepth(prog);
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
        if (mac || ilu) {
            std::string a = Src(k, 0, regs), b = Src(k, 1, regs), c = Src(k, 2, regs);
            s += "    {\n";
            if (mac) s += "        let mr = " + MacExpr(mac, a, b, c) + ";\n";
            if (separable) s += ClipSnapshot(k, a, b);
            if (ilu) s += "        let ir = " + IluExpr(ilu, c) + ";\n";
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (mac == kMacArl) {
                s += "        a0 = mac_arl(mr.x);\n";
            } else if (mac) {
                uint32_t m = k.f(3, 24, 4);
                if (ilu && r == 1) m = 0;
                if (m && r <= 12) s += AssignMask(RegDest(r), kMask[m], "mr");
            }
            if (ilu) {
                uint32_t m = k.f(3, 16, 4), ir = mac ? 1 : r;
                if (m && ir <= 12) s += AssignMask(RegDest(ir), kMask[m], "ir");
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != kMacArl))) {
                uint32_t o = k.f(3, 3, 8) & 0xFu;
                const char* src = use_ilu ? "ir" : "mr";
                if (kOut[o][0]) {
                    const char* msk = o == 5 ? kLanes[std::popcount(omask)] : kMask[omask];
                    s += AssignMask(kOut[o], msk, src);
                }
            }
            s += "    }\n";
        }
        if (k.f(3, 0, 1)) break;
    }
    s += VsEpilogue(sprite, quad);
    return s;
}

std::string FixedVertexWgsl(uint16_t regs, bool pretransformed, uint64_t decode, const gpu::FixedKey& f, bool quad) {
    std::string s = kCommonStruct;
    s += kVsHelpers;
    s += VsInputs(regs, decode);
    s += kVsPrologue;
    auto v = [&](int r, const char* def) { return ((regs >> r) & 1) ? F("vi.v%d", r) : std::string(def); };
    bool lit = !pretransformed && (f.light & 1u);
    if (pretransformed) {
        s += "    let rhw = " + v(0, "vec4<f32>(0.0, 0.0, 0.0, 1.0)") + ";\n";
        s += "    let rw = select(1.0 / rhw.w, 1.0, rhw.w == 0.0);\n";
        s += "    oPos = vec4<f32>(rhw.xy, rhw.z / dc.surf.z, rw);\n";
    } else {
        s += "    let ip = vec4<f32>((" + v(0, "vec4<f32>(0.0, 0.0, 0.0, 1.0)") + ").xyz, 1.0);\n";
        s += "    var cp = ip * dc.wvp;\n";
        s += "    let vp = (ip * dc.wv).xyz;\n";
        s += "    var vn = (vec4<f32>((" + v(2, "vec4<f32>(0.0)") + ").xyz, 0.0) * dc.wvn).xyz;\n";
        if (f.light & 8u) s += "    if (dot(vn, vn) > 0.0) { vn = normalize(vn); }\n";
    }
    if (lit) {
        auto src = [&](int field, const char* mat) {
            uint32_t m = (f.light >> (8 + field * 2)) & 3u;
            if (m == 1 && ((regs >> 3) & 1)) return std::string("vi.v3");
            if (m == 2 && ((regs >> 4) & 1)) return std::string("vi.v4");
            return std::string(mat);
        };
        s += "    let md = " + src(0, "dc.mat[0]") + ";\n    let ms = " + src(1, "dc.mat[2]") +
             ";\n    let ma = " + src(2, "dc.mat[1]") + ";\n    let me = " + src(3, "dc.mat[3]") + ";\n";
        s += "    var la = vec3<f32>(0.0); var ld = vec3<f32>(0.0); var ls = vec3<f32>(0.0);\n";
        s += (f.light & 4u) ? "    let eye = -normalize(vp);\n" : "    let eye = vec3<f32>(0.0, 0.0, 1.0);\n";
        for (int n = 0; n < 8; n++) {
            uint32_t type = (f.types >> (n * 2)) & 3u;
            if (!type) continue;
            int b = n * 7;
            s += "    {\n        var l : vec3<f32>;\n        var att : f32 = 1.0;\n";
            if (type == 3) {
                s += F("        l = -normalize(dc.lt[%d].xyz);\n", b + 4);
            } else {
                s += F("        let d = dc.lt[%d].xyz - vp;\n        let dist = length(d);\n        l = d / max(dist, 1e-6);\n",
                       b + 3);
                s += F("        att = select(1.0 / max(dc.lt[%d].x + dc.lt[%d].y * dist + dc.lt[%d].z * dist * dist, 1e-6), 0.0, dist > dc.lt[%d].w);\n",
                       b + 5, b + 5, b + 5, b + 3);
                if (type == 2) {
                    s += F("        let rho = dot(-l, normalize(dc.lt[%d].xyz));\n", b + 4);
                    s += F("        let spot = select(pow(saturate((rho - dc.lt[%d].y) / max(dc.lt[%d].x - dc.lt[%d].y, 1e-6)), dc.lt[%d].w), 0.0, rho <= dc.lt[%d].y);\n",
                           b + 6, b + 6, b + 6, b + 4, b + 6);
                    s += F("        att = att * select(spot, 1.0, rho > dc.lt[%d].x);\n", b + 6);
                }
            }
            s += F("        la = la + att * dc.lt[%d].rgb;\n", b + 2);
            s += "        let nl = dot(vn, l);\n";
            s += F("        ld = ld + att * dc.lt[%d].rgb * max(nl, 0.0);\n", b);
            if (f.light & 2u) {
                s += F("        if (nl > 0.0) { ls = ls + att * dc.lt[%d].rgb * pow(max(dot(vn, normalize(l + eye)), 0.0), max(dc.mat[4].x, 1e-4)); }\n",
                       b + 1);
            }
            s += "    }\n";
        }
        s += "    oD0 = vec4<f32>(me.rgb + ma.rgb * (dc.amb.rgb + la) + md.rgb * ld, md.a);\n";
        s += (f.light & 2u) ? "    oD1 = vec4<f32>(ms.rgb * ls, ms.a);\n" : "    oD1 = vec4<f32>(0.0);\n";
    } else {
        s += "    oD0 = " + v(3, "vec4<f32>(1.0)") + ";\n";
        s += "    oD1 = " + v(4, "vec4<f32>(0.0)") + ";\n";
    }
    s += "    oB0 = oD0;\n    oB1 = oD1;\n";
    for (int t = 0; t < 4; t++) {
        uint32_t u = f.tex[t], set = u & 3u, gen = (u >> 4) & 7u, count = (u >> 8) & 7u, size = (u >> 12) & 7u;
        std::string in = v(9 + int(set), "vec4<f32>(0.0, 0.0, 0.0, 1.0)");
        if (pretransformed || (!gen && !count)) {
            s += F("    oT%d = %s;\n", t, in.c_str());
            continue;
        }
        std::string c = gen == 1   ? "vec4<f32>(vn, 1.0)"
                      : gen == 2   ? "vec4<f32>(vp, 1.0)"
                      : gen == 3   ? "vec4<f32>(reflect(normalize(vp), vn), 1.0)"
                      : size == 1  ? F("vec4<f32>((%s).x, 1.0, 0.0, 0.0)", in.c_str())
                      : size == 2  ? F("vec4<f32>((%s).xy, 1.0, 0.0)", in.c_str())
                      : size == 3  ? F("vec4<f32>((%s).xyz, 1.0)", in.c_str())
                                   : in;
        if (!count) {
            s += F("    oT%d = %s;\n", t, c.c_str());
            continue;
        }
        static const char* kComp[4] = {"x", "y", "z", "w"};
        const uint32_t comp_idx = std::min<uint32_t>(count, 4u) - 1u;
        s += F("    {\n        let tc = (%s) * dc.tm[%d];\n", c.c_str(), t);
        if (u & 0x800u) s += F("        oT%d = vec4<f32>(tc.xy, tc.z, tc.%s);\n", t, kComp[comp_idx]);
        else s += F("        oT%d = vec4<f32>(tc.xy, %s, 1.0);\n", t, count >= 3 ? "tc.z" : "0.0");
        s += "    }\n";
    }
    s += "    oFog = " + v(5, "vec4<f32>(0.0)") + ";\n";
    if (pretransformed) return s + VsEpilogue(false, quad);

    s += "\n    var o : VOut;\n"
         "    let sp = (vec2<f32>(cp.x, -cp.y) / select(cp.w, 1.0, cp.w == 0.0) + vec2<f32>(1.0)) * 0.5 * dc.surf.xy;\n";
    s += quad ? "    let sp_orig = sp;\n" : "";
    s += "    if (dc.surf.w == 0.0 && abs(sp.x - (dc.surf.x - 0.5)) < 1e-3) { cp.x = cp.w; }\n"
         "    if (dc.surf.w == 0.0 && abs(sp.y - (dc.surf.y - 0.5)) < 1e-3) { cp.y = -cp.w; }\n";
    if (quad) {
        s += "    let sp_pulled = (vec2<f32>(cp.x, -cp.y) / select(cp.w, 1.0, cp.w == 0.0) + vec2<f32>(1.0)) * 0.5 * dc.surf.xy;\n"
             "    o.e = vec4<f32>(sp_orig, sp_pulled);\n";
    } else {
        s += "    o.e = vec4<f32>(0.0);\n";
    }
    s += "    o.pos = cp;\n"
         "    let wz = dot(ip, dc.wvp[3] - dc.wvp[2]);\n"
         "    o.pos.z = ReversedClip(wz, cp.w);\n"
         "    o.d0 = saturate(oD0);\n"
         "    o.d1 = saturate(oD1);\n"
         "    o.b0 = o.d0;\n"
         "    o.b1 = o.d1;\n"
         "    o.tc0 = oT0;\n"
         "    o.tc1 = oT1;\n"
         "    o.tc2 = oT2;\n"
         "    o.tc3 = oT3;\n"
         "    o.fogamt = fogfactor(abs(cp.z));\n"
         "    return o;\n}\n";
    return s;
}

std::string PixelWgsl(const gpu::PixelKey& k) {
    std::string s = kCommonStruct;
    s += kPsHelpers;
    for (int i = 0; i < 4; i++) {
        const gpu::TexInfo& t = k.tex[i];
        const char* ttype = t.shadow                       ? "texture_depth_2d"
                          : t.kind == gpu::kTex3D          ? "texture_3d<f32>"
                          : t.kind == gpu::kTexCube        ? "texture_cube<f32>"
                                                           : "texture_2d<f32>";
        const char* stype = t.shadow ? "sampler_comparison" : "sampler";
        s += F("@group(1) @binding(%d) var tex%d : %s;\n", i, i, ttype);
        s += F("@group(1) @binding(%d) var smp%d : %s;\n", 4 + i, i, stype);
    }
    s += "@fragment fn fs_main(i: VOut, @builtin(front_facing) front: bool) -> @location(0) vec4<f32> {\n";
    if (k.quad) {
        for (int i = 0; i < 4; i++) s += F("    let tc%d = unstretch(i.tc%d, i.e);\n", i, i);
    } else {
        s += "    let tc0 = i.tc0; let tc1 = i.tc1; let tc2 = i.tc2; let tc3 = i.tc3;\n";
    }
    s += k.two_sided ? "    var v0 = select(i.b0, i.d0, front);\n    var v1 = select(i.b1, i.d1, front);\n"
                     : "    var v0 = i.d0;\n    var v1 = i.d1;\n";
    s += "    if (dc.misc.w == 0.0) { v1 = vec4<f32>(0.0, 0.0, 0.0, 1.0); }\n";
    s += "    let fogin = vec4<f32>(dc.fogcolor.rgb, saturate(i.fogamt));\n";
    s += "    var kill_frag : bool = false;\n";

    std::string kill_code;
    if (k.programmable) {
        for (int i = 0; i < 4; i++) s += TexFetch(k, i, kill_code);
        for (int i = 0; i < 4; i++) {
            uint32_t m = (k.tex_modes >> (i * 5)) & 0x1Fu;
            bool fetch = m && m != 4 && m != 5 && m != 0xA && m != 0x11;
            if (fetch && ((k.alphakill >> i) & 1u)) kill_code += F("    if (t%d.a == 0.0) { kill_frag = true; }\n", i);
        }
        uint32_t mode0 = k.tex_modes & 0x1Fu;
        s += F("    var r0 = vec4<f32>(0.0, 0.0, 0.0, %s);\n    var r1 = vec4<f32>(0.0);\n", mode0 ? "t0.a" : "1.0");
        Ps ps{k};
        uint32_t n = std::min<uint32_t>(k.count & 0xFFu, 8u);
        for (uint32_t st = 0; st < n; st++) {
            ps.stage = int(st);
            ps.code += "    {\n";
            std::string wc = ps.Stage(k.rgb_in[st], k.rgb_out[st], false);
            std::string wa = ps.Stage(k.a_in[st], k.a_out[st], true);
            ps.code += wc + wa + "    }\n";
        }
        s += ps.code;
        if (k.final_abcd || k.final_efg) {
            ps.stage = 8;
            uint32_t fl = k.final_efg & 0xFFu;
            ps.clamp_sum = (fl & 0x80u) != 0;
            ps.inv_v1 = (fl & 0x40u) != 0;
            ps.inv_r0 = (fl & 0x20u) != 0;
            ps.e = ps.Input(ParseIn(k.final_efg >> 24), false);
            ps.f = ps.Input(ParseIn((k.final_efg >> 16) & 0xFFu), false);
            std::string A = ps.Input(ParseIn(k.final_abcd >> 24), false);
            std::string B = ps.Input(ParseIn((k.final_abcd >> 16) & 0xFFu), false);
            std::string C = ps.Input(ParseIn((k.final_abcd >> 8) & 0xFFu), false);
            std::string D = ps.Input(ParseIn(k.final_abcd & 0xFFu), false);
            std::string G = ps.Input(ParseIn((k.final_efg >> 8) & 0xFFu), true);
            s += "    var outc : vec4<f32>;\n";
            s += "    outc = vec4<f32>((" + D + ") + mix(" + C + ", " + B + ", " + A + "), " + G + ");\n";
        } else {
            s += "    var outc = vec4<f32>(mix(fogin.rgb, saturate(r0.rgb), fogin.a), saturate(r0.a));\n";
        }
    } else {
        s += "    var cur = v0;\n    var tmp = vec4<f32>(0.0);\n";
        // Pre-fetch all active fixed stages first so textureSample remains in uniform control flow.
        int active_stages = 0;
        for (int i = 0; i < 4; i++) {
            const uint32_t* f = k.ff[i];
            if (f[0] <= 1) break;
            active_stages = i + 1;
            const gpu::TexInfo& t = k.tex[i];
            std::string uv = TexUv(t, i, ((k.proj >> i) & 1u) ? F("tc%d.xy / tc%d.w", i, i) : F("tc%d.xy", i));
            if (t.shadow) {
                std::string suv = TexUv(t, i, F("tc%d.xy / tc%d.w", i, i));
                s += F("    let t%d = shadow_sample(tex%d, smp%d, %s, tc%d.z / tc%d.w, %d);\n", i, i, i, suv.c_str(),
                       i, i, t.shadow);
            } else if (t.kind == gpu::kTex2D && (t.flags & gpu::kTexWide)) {
                s += F("    let t%d = spreadtap(tex%d, smp%d, %s, dc.spread[%d]);\n", i, i, i, uv.c_str(), i);
            } else if (t.kind == gpu::kTex2D) {
                s += F("    let t%d = textureSample(tex%d, smp%d, %s);\n", i, i, i, uv.c_str());
            } else if (t.kind == gpu::kTexCube || t.kind == gpu::kTex3D) {
                s += F("    let t%d = textureSample(tex%d, smp%d, tc%d.xyz);\n", i, i, i, i);
            } else {
                s += F("    let t%d = vec4<f32>(1.0);\n", i);
            }
            if ((k.alphakill >> i) & 1u) kill_code += F("    if (t%d.a == 0.0) { kill_frag = true; }\n", i);
        }
        for (int i = 0; i < active_stages; i++) {
            const uint32_t* f = k.ff[i];
            const char* dst = f[8] == 5 ? "tmp" : "cur";
            std::string rgb = FfOp(f[0], FfArg(f[1], i, false), FfArg(f[2], i, false), FfArg(f[3], i, false), i, false);
            std::string a = f[4] <= 1 ? std::string("cur.a")
                                      : FfOp(f[4], FfArg(f[5], i, true), FfArg(f[6], i, true), FfArg(f[7], i, true), i, true);
            s += F("    {\n        let c3 = saturate(%s);\n        let c1 = saturate(%s);\n        %s = vec4<f32>(c3, c1);\n    }\n",
                   rgb.c_str(), a.c_str(), dst);
        }
        s += "    cur = vec4<f32>(saturate(cur.rgb + v1.rgb), cur.a);\n";
        s += "    var outc = vec4<f32>(mix(fogin.rgb, cur.rgb, fogin.a), cur.a);\n";
    }
    s += kill_code;
    if (k.alphatest) s += "    if (!alphatest_pass(outc.a)) { kill_frag = true; }\n";
    s += "    if (kill_frag) { discard; }\n";
    s += "    outc = vec4<f32>(saturate(outc.rgb), outc.a);\n    return outc;\n}\n";
    return s;
}

std::string PresentBlitWgsl() {
    return R"(
struct BlitParams {
    uv_rect : vec4<f32>,
    flags : vec4<f32>,
};
@group(0) @binding(0) var<uniform> bp : BlitParams;
@group(0) @binding(1) var tex0 : texture_2d<f32>;
@group(0) @binding(2) var lut0 : texture_2d<f32>;
@group(0) @binding(3) var smp0 : sampler;

struct BOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv : vec2<f32>,
};

@vertex fn vs_blit(@builtin(vertex_index) vid : u32) -> BOut {
    var p = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    let xy = p[vid];
    let t = vec2<f32>(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);
    var o : BOut;
    o.pos = vec4<f32>(xy, 0.0, 1.0);
    o.uv = mix(bp.uv_rect.xy, bp.uv_rect.zw, t);
    return o;
}

@fragment fn fs_blit(i: BOut) -> @location(0) vec4<f32> {
    var c = textureSample(tex0, smp0, i.uv);
    if (bp.flags.x > 0.5) {
        let r = textureSample(lut0, smp0, vec2<f32>(c.r, 0.5)).r;
        let g = textureSample(lut0, smp0, vec2<f32>(c.g, 0.5)).g;
        let b = textureSample(lut0, smp0, vec2<f32>(c.b, 0.5)).b;
        c = vec4<f32>(r, g, b, c.a);
    }
    return vec4<f32>(c.rgb, 1.0);
}
)";
}

std::string ClearQuadWgsl() {
    return R"(
struct ClearUniform {
    col : vec4<f32>,
    z_val : vec4<f32>,
};
@group(0) @binding(0) var<uniform> cu : ClearUniform;

@vertex fn vs_clear(@builtin(vertex_index) vid : u32) -> @builtin(position) vec4<f32> {
    var p = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    return vec4<f32>(p[vid], cu.z_val.x, 1.0);
}

@fragment fn fs_clear() -> @location(0) vec4<f32> {
    return cu.col;
}
)";
}

std::string DepthUploadWgsl() {
    return R"(
@group(0) @binding(0) var src_depth : texture_2d<f32>;

@vertex fn vs_depth(@builtin(vertex_index) vid : u32) -> @builtin(position) vec4<f32> {
    var p = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    return vec4<f32>(p[vid], 0.0, 1.0);
}

@fragment fn fs_depth(@builtin(position) pos : vec4<f32>) -> @builtin(frag_depth) f32 {
    let coord = vec2<i32>(i32(pos.x), i32(pos.y));
    return 1.0 - saturate(textureLoad(src_depth, coord, 0).r);
}
)";
}

}  // namespace gpu_webgpu
