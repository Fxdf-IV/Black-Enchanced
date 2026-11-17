// Renderer WebGPU para o runtime WebAssembly de Black Enhanced Online.
// Consome o stream binário emitido por src/web/gpu_webgpu.cpp via window.__blackWebGpu.

const OP_CREATE_SHADER = 1;
const OP_CREATE_SAMPLER = 2;
const OP_CREATE_TEXTURE = 3;
const OP_UPLOAD_TEXTURE = 4;
const OP_UPLOAD_GAMMA = 5;
const OP_DESTROY_TEXTURE = 6;
const OP_CREATE_PIPELINE = 7;
const OP_CLEAR = 8;
const OP_DRAW = 9;
const OP_COPY_TEX = 10;
const OP_PRESENT = 11;
const OP_UPLOAD_DEPTH = 12;

const TEX_FORMATS = [
  "bgra8unorm",
  "bc1-rgba-unorm",
  "bc2-rgba-unorm",
  "bc3-rgba-unorm",
  "r32float",
  "depth24plus-stencil8",
];

const ADDR_MODES = ["repeat", "mirror-repeat", "clamp-to-edge"];
const FILTER_MODES = ["nearest", "linear"];
const CMP_FUNCS = [
  "never",
  "less",
  "equal",
  "less-equal",
  "greater",
  "not-equal",
  "greater-equal",
  "always",
];
const BLEND_FACTORS = [
  "zero",
  "one",
  "src",
  "one-minus-src",
  "src-alpha",
  "one-minus-src-alpha",
  "dst-alpha",
  "one-minus-dst-alpha",
  "dst",
  "one-minus-dst",
  "src-alpha-saturated",
  "constant",
  "one-minus-constant",
];
const BLEND_OPS = ["add", "subtract", "reverse-subtract", "min", "max"];
const STENCIL_OPS = [
  "keep",
  "zero",
  "replace",
  "increment-clamp",
  "decrement-clamp",
  "invert",
  "increment-wrap",
  "decrement-wrap",
];
const TOPOLOGIES = ["point-list", "line-list", "line-strip", "triangle-list", "triangle-strip"];
const VERTEX_FORMATS = [
  "float32",
  "float32x2",
  "float32x3",
  "float32x4",
  "unorm8x4",
  "snorm16x2",
  "snorm16x4",
  "unorm8x2",
  "sint16x2",
  "sint16x4",
  "uint32",
];

const VSC_BYTES = 192 * 16; // 3072
const DC_BYTES = 528 * 4;   // 2112 (match DrawConstants in sdk/runtime/gpu/shaders.h)

function alignUp(v, a) {
  return (v + a - 1) & ~(a - 1);
}

class BinaryReader {
  constructor(bytes, byteOffset = 0, byteLength = bytes ? bytes.byteLength : 0) {
    this.reset(bytes, byteOffset, byteLength);
  }
  reset(bytes, byteOffset = 0, byteLength = bytes ? bytes.byteLength - byteOffset : 0) {
    this.u8 = bytes;
    this.basePos = byteOffset;
    const baseOff = bytes ? bytes.byteOffset + byteOffset : 0;
    this.dv = bytes ? new DataView(bytes.buffer, baseOff, byteLength) : null;
    this.pos = byteOffset;
    this.end = byteOffset + byteLength;
  }
  u32() {
    const v = this.dv.getUint32(this.pos - this.basePos, true);
    this.pos += 4;
    return v;
  }
  i32() {
    const v = this.dv.getInt32(this.pos - this.basePos, true);
    this.pos += 4;
    return v;
  }
  f32() {
    const v = this.dv.getFloat32(this.pos - this.basePos, true);
    this.pos += 4;
    return v;
  }
  bytesAligned(byteLen) {
    const slice = this.u8.subarray(this.pos, this.pos + byteLen);
    this.pos += alignUp(byteLen, 4);
    return slice;
  }
  strAligned(byteLen) {
    const slice = this.bytesAligned(byteLen);
    const view =
      typeof SharedArrayBuffer !== "undefined" && slice.buffer instanceof SharedArrayBuffer ? slice.slice() : slice;
    return new TextDecoder().decode(view);
  }
}

export class BlackWebGpuRenderer {
  constructor(canvas, onLog) {
    this.canvas = canvas;
    this.onLog = onLog || (() => {});
    this.module = null;
    this.device = null;
    this.context = null;
    this.presentationFormat = "bgra8unorm";
    this.bcSupported = false;
    this.adapterName = "Desconhecido";
    this.ready = false;

    this.shaders = new Map();
    this.samplers = new Map();
    this.textures = new Map();
    this.pipelines = new Map();
    this.bgl1Cache = new Map();
    this.bg1Cache = new Map();
    this.texInBg1 = new Set();
    this.dirtyBg1Ids = new Set();
    this.blitBgCache = new Map();
    this.depthBgCache = new Map();
    this.clearPipelines = new Map();
    this.clearBg = null;
    this.clearLayout = null;
    this.pendingDestroyTex = [];
    this.reader = new BinaryReader(new Uint8Array(0));
    this.tmpBp = new Float32Array(8);
    this.tmpBpU8 = new Uint8Array(this.tmpBp.buffer);
    this.tmpClear = new Float32Array(8);
    this.tmpClearU8 = new Uint8Array(this.tmpClear.buffer);

    this.encoder = null;
    this.pass = null;
    this.passRtId = 0;
    this.passDsId = 0;
    this.touchedTexInEncoder = new Set();

    this.uniAlign = 256;
    this.uniBuf = null;
    this.uniStaging = null;
    this.uniCursor = 0;
    this.uniFlushed = 0;
    this.uniGroup0 = null;
    this.bgl0 = null;

    this.geomBuf = null;
    this.geomStaging = null;
    this.geomCursor = 0;
    this.geomFlushed = 0;
    this.dummyVb = null;

    this.vscCache = new Uint8Array(VSC_BYTES);
    this.dcCache = new Uint8Array(DC_BYTES);
    this.hasVscCache = false;
    this.hasDcCache = false;
    this.lastVscOff = 0;
    this.lastDcOff = 0;

    this.gammaLutTex = null;
    this.gammaLutView = null;
    this.defaultSampler = null;

    this.perf = {
      winStart: performance.now(),
      presents: 0,
      batches: 0,
      draws: 0,
      submitMs: 0,
      writeBufCalls: 0,
      writeBufBytes: 0,
      createBgCalls: 0,
      flushEncCalls: 0,
      readbacks: 0,
      readbackMs: 0,
    };
    this.defaultCmpSampler = null;
    this.dummy2D = null;
    this.dummy3D = null;
    this.dummyCube = null;
    this.dummyDepth = null;

    this.blitBgl = null;
    this.blitPipelineCanvas = null;
    this.blitPipelineBgra = null;
    this.clearBgl = null;
    this.clearModule = null;
    this.depthBgl = null;
    this.depthPipeline = null;

    this.errorCount = 0;
  }

  setModule(mod) {
    this.module = mod;
    if (this.ready && this.device && !this.blitPipelineCanvas) {
      this.initBuiltinPipelines();
    }
  }

  getHeapU8() {
    const m = this.module;
    if (!m) return null;
    if (m.wasmMemory && (!m.HEAPU8 || m.HEAPU8.buffer !== m.wasmMemory.buffer)) {
      return new Uint8Array(m.wasmMemory.buffer);
    }
    return m.HEAPU8;
  }

  installGlobal() {
    window.__blackWebGpu = {
      ensureDevice: () => this.ensureDevice(),
      submitBatch: (ptr, len) => this.submitBatch(ptr, len),
      readbackTexture: (texId, w, h, outPtr) => this.readbackTexture(texId, w, h, outPtr),
      waitIdle: () => this.waitIdle(),
    };
  }

  async ensureDevice() {
    if (this.ready && this.device) {
      return { ready: true, bc: this.bcSupported };
    }
    if (!navigator.gpu) {
      return { ready: false, bc: false };
    }

    const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
    if (!adapter) {
      return { ready: false, bc: false };
    }

    if (adapter.info) {
      this.adapterName = adapter.info.description || adapter.info.architecture || adapter.info.vendor || "WebGPU Adapter";
    }
    this.bcSupported = adapter.features.has("texture-compression-bc");
    const requiredFeatures = this.bcSupported ? ["texture-compression-bc"] : [];

    this.device = await adapter.requestDevice({ requiredFeatures });
    this.device.addEventListener("uncapturederror", (ev) => {
      this.errorCount++;
      if (this.errorCount <= 12) {
        this.onLog("WEBGPU", `Validation/GPU error: ${ev.error?.message || ev.error}`);
      }
    });

    this.uniAlign = Math.max(256, this.device.limits.minUniformBufferOffsetAlignment || 256);
    this.context = this.canvas.getContext("webgpu");
    this.presentationFormat = navigator.gpu.getPreferredCanvasFormat();
    this.context.configure({
      device: this.device,
      format: this.presentationFormat,
      alphaMode: "opaque",
      usage: GPUTextureUsage.RENDER_ATTACHMENT,
    });

    this.initStaticResources();
    if (this.module && !this.blitPipelineCanvas) {
      this.initBuiltinPipelines();
    }
    this.ready = true;
    this.onLog("WEBGPU", `Device ativo (${this.adapterName}, format=${this.presentationFormat}, BC=${this.bcSupported ? "SIM" : "SW"}).`);
    return { ready: true, bc: this.bcSupported };
  }

  initStaticResources() {
    this.bgl0 = this.device.createBindGroupLayout({
      label: "black_bgl0_uniforms",
      entries: [
        {
          binding: 0,
          visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
          buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: VSC_BYTES },
        },
        {
          binding: 1,
          visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
          buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: DC_BYTES },
        },
      ],
    });

    this.allocUniArena(8 * 1024 * 1024);
    this.allocGeomArena(16 * 1024 * 1024);
    this.dummyVb = this.device.createBuffer({
      label: "black_dummy_vb",
      size: 64,
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });

    this.defaultSampler = this.device.createSampler({
      magFilter: "linear",
      minFilter: "linear",
      mipmapFilter: "linear",
      addressModeU: "clamp-to-edge",
      addressModeV: "clamp-to-edge",
      addressModeW: "clamp-to-edge",
    });
    this.defaultCmpSampler = this.device.createSampler({
      magFilter: "linear",
      minFilter: "linear",
      addressModeU: "clamp-to-edge",
      addressModeV: "clamp-to-edge",
      addressModeW: "clamp-to-edge",
      compare: "less",
    });

    const mkDummyTex = (label, format, dimension, layers, pxData) => {
      const tex = this.device.createTexture({
        label,
        size: [1, 1, layers],
        dimension,
        format,
        usage:
          format === "depth24plus-stencil8"
            ? GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT
            : GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      if (pxData) {
        this.device.queue.writeTexture(
          { texture: tex },
          pxData,
          { bytesPerRow: 4, rowsPerImage: 1 },
          [1, 1, layers]
        );
      }
      return tex;
    };

    const black4 = new Uint8Array([0, 0, 0, 255]);
    const cube24 = new Uint8Array(24);
    for (let i = 0; i < 6; i++) cube24[i * 4 + 3] = 255;

    const d2d = mkDummyTex("dummy_2d", "bgra8unorm", "2d", 1, black4);
    this.dummy2D = d2d.createView({ dimension: "2d" });
    const d3d = mkDummyTex("dummy_3d", "bgra8unorm", "3d", 1, black4);
    this.dummy3D = d3d.createView({ dimension: "3d" });
    const dcube = mkDummyTex("dummy_cube", "bgra8unorm", "2d", 6, cube24);
    this.dummyCube = dcube.createView({ dimension: "cube" });
    const ddepth = mkDummyTex("dummy_depth", "depth24plus-stencil8", "2d", 1, null);
    this.dummyDepth = ddepth.createView({ aspect: "depth-only" });

    this.gammaLutTex = this.device.createTexture({
      label: "gamma_lut",
      size: [256, 1, 1],
      format: "rgba8unorm",
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    const lutInit = new Uint8Array(256 * 4);
    for (let i = 0; i < 256; i++) {
      lutInit[i * 4 + 0] = i;
      lutInit[i * 4 + 1] = i;
      lutInit[i * 4 + 2] = i;
      lutInit[i * 4 + 3] = 255;
    }
    this.device.queue.writeTexture({ texture: this.gammaLutTex }, lutInit, { bytesPerRow: 1024 }, [256, 1, 1]);
    this.gammaLutView = this.gammaLutTex.createView();
  }

  getBuiltinWgsl(kind) {
    if (this.module && typeof this.module._BlackWebGetBuiltinWgsl === "function") {
      const ptr = this.module._BlackWebGetBuiltinWgsl(kind);
      if (ptr) return this.module.UTF8ToString(ptr);
    }
    return "";
  }

  initBuiltinPipelines() {
    const blitCode = this.getBuiltinWgsl(0);
    const clearCode = this.getBuiltinWgsl(1);
    const depthCode = this.getBuiltinWgsl(2);
    if (!blitCode || !clearCode || !depthCode) {
      return;
    }

    this.blitBgl = this.device.createBindGroupLayout({
      label: "black_blit_bgl",
      entries: [
        {
          binding: 0,
          visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
          buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: 32 },
        },
        { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "float" } },
        { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "float" } },
        { binding: 3, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "filtering" } },
      ],
    });
    const blitMod = this.device.createShaderModule({ label: "black_blit_wgsl", code: blitCode });
    const blitLayout = this.device.createPipelineLayout({ bindGroupLayouts: [this.blitBgl] });
    const mkBlitPso = (format) =>
      this.device.createRenderPipeline({
        label: `black_blit_${format}`,
        layout: blitLayout,
        vertex: { module: blitMod, entryPoint: "vs_blit" },
        fragment: { module: blitMod, entryPoint: "fs_blit", targets: [{ format }] },
        primitive: { topology: "triangle-list" },
      });
    this.blitPipelineCanvas = mkBlitPso(this.presentationFormat);
    this.blitPipelineBgra = this.presentationFormat === "bgra8unorm" ? this.blitPipelineCanvas : mkBlitPso("bgra8unorm");

    this.clearBgl = this.device.createBindGroupLayout({
      label: "black_clear_bgl",
      entries: [
        {
          binding: 0,
          visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
          buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: 32 },
        },
      ],
    });
    this.clearLayout = this.device.createPipelineLayout({ bindGroupLayouts: [this.clearBgl] });
    this.clearBg = this.device.createBindGroup({
      label: "black_clear_bg",
      layout: this.clearBgl,
      entries: [{ binding: 0, resource: { buffer: this.uniBuf, offset: 0, size: 32 } }],
    });
    this.clearModule = this.device.createShaderModule({ label: "black_clear_wgsl", code: clearCode });

    this.depthBgl = this.device.createBindGroupLayout({
      label: "black_depth_upload_bgl",
      entries: [{ binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: "unfilterable-float" } }],
    });
    const depthMod = this.device.createShaderModule({ label: "black_depth_upload_wgsl", code: depthCode });
    this.depthPipeline = this.device.createRenderPipeline({
      label: "black_depth_upload_pso",
      layout: this.device.createPipelineLayout({ bindGroupLayouts: [this.depthBgl] }),
      vertex: { module: depthMod, entryPoint: "vs_depth" },
      fragment: { module: depthMod, entryPoint: "fs_depth", targets: [] },
      primitive: { topology: "triangle-list" },
      depthStencil: { format: "depth24plus-stencil8", depthWriteEnabled: true, depthCompare: "always" },
    });
  }

  allocUniArena(size) {
    if (this.uniBuf) this.uniBuf.destroy();
    this.uniBuf = this.device.createBuffer({
      label: "black_uni_arena",
      size,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    this.uniStaging = new Uint8Array(size);
    this.uniCursor = 0;
    this.uniFlushed = 0;
    this.uniGroup0 = this.device.createBindGroup({
      label: "black_group0_dynamic",
      layout: this.bgl0,
      entries: [
        { binding: 0, resource: { buffer: this.uniBuf, offset: 0, size: VSC_BYTES } },
        { binding: 1, resource: { buffer: this.uniBuf, offset: 0, size: DC_BYTES } },
      ],
    });
    if (this.clearBgl) {
      this.clearBg = this.device.createBindGroup({
        label: "black_clear_bg",
        layout: this.clearBgl,
        entries: [{ binding: 0, resource: { buffer: this.uniBuf, offset: 0, size: 32 } }],
      });
    }
    this.blitBgCache.clear();
    this.hasVscCache = false;
    this.hasDcCache = false;
  }

  allocGeomArena(size) {
    if (this.geomBuf) this.geomBuf.destroy();
    this.geomBuf = this.device.createBuffer({
      label: "black_geom_arena",
      size,
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
    });
    this.geomStaging = new Uint8Array(size);
    this.geomCursor = 0;
    this.geomFlushed = 0;
  }

  uploadUniSlice(bytes) {
    const aligned = alignUp(bytes.byteLength, this.uniAlign);
    if (this.uniCursor + aligned > this.uniBuf.size) {
      this.flushActiveEncoder();
      const nextSize = Math.max(this.uniBuf.size * 2, aligned * 64);
      this.allocUniArena(nextSize);
    }
    const off = this.uniCursor;
    this.uniStaging.set(bytes, off);
    this.uniCursor += aligned;
    return off;
  }

  uploadGeomSlice(bytes) {
    if (bytes.byteLength === 0) return 0;
    const aligned = alignUp(bytes.byteLength, 4);
    if (this.geomCursor + aligned > this.geomBuf.size) {
      this.flushActiveEncoder();
      const nextSize = Math.max(this.geomBuf.size * 2, aligned * 16);
      this.allocGeomArena(nextSize);
    }
    const off = this.geomCursor;
    this.geomStaging.set(bytes, off);
    if (aligned > bytes.byteLength) {
      this.geomStaging.fill(0, off + bytes.byteLength, off + aligned);
    }
    this.geomCursor += aligned;
    return off;
  }

  ensureEncoder() {
    if (!this.encoder) {
      this.encoder = this.device.createCommandEncoder();
    }
  }

  endActivePass() {
    if (this.pass) {
      this.pass.end();
      this.pass = null;
      this.passRtId = 0;
      this.passDsId = 0;
    }
  }

  flushActiveEncoder() {
    this.endActivePass();
    if (this.uniCursor > this.uniFlushed) {
      const len = this.uniCursor - this.uniFlushed;
      this.device.queue.writeBuffer(this.uniBuf, this.uniFlushed, this.uniStaging, this.uniFlushed, len);
      this.perf.writeBufCalls++;
      this.perf.writeBufBytes += len;
      this.uniFlushed = this.uniCursor;
    }
    if (this.geomCursor > this.geomFlushed) {
      const len = this.geomCursor - this.geomFlushed;
      this.device.queue.writeBuffer(this.geomBuf, this.geomFlushed, this.geomStaging, this.geomFlushed, len);
      this.perf.writeBufCalls++;
      this.perf.writeBufBytes += len;
      this.geomFlushed = this.geomCursor;
    }
    if (this.encoder) {
      this.perf.flushEncCalls++;
      this.device.queue.submit([this.encoder.finish()]);
      this.encoder = null;
      this.touchedTexInEncoder.clear();
    }
    if (this.pendingDestroyTex.length > 0) {
      for (const tex of this.pendingDestroyTex) tex.destroy();
      this.pendingDestroyTex.length = 0;
    }
  }

  getBgl1(kinds) {
    const key = `${kinds[0]},${kinds[1]},${kinds[2]},${kinds[3]}`;
    let bgl = this.bgl1Cache.get(key);
    if (bgl) return bgl;

    const entries = [];
    for (let i = 0; i < 4; i++) {
      const k = kinds[i];
      if (k === 3) {
        entries.push({ binding: i, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: "2d", sampleType: "depth" } });
        entries.push({ binding: 4 + i, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "comparison" } });
      } else {
        const dim = k === 1 ? "3d" : k === 2 ? "cube" : "2d";
        entries.push({ binding: i, visibility: GPUShaderStage.FRAGMENT, texture: { viewDimension: dim, sampleType: "float" } });
        entries.push({ binding: 4 + i, visibility: GPUShaderStage.FRAGMENT, sampler: { type: "filtering" } });
      }
    }
    bgl = this.device.createBindGroupLayout({ label: `black_bgl1_${key}`, entries });
    this.bgl1Cache.set(key, bgl);
    return bgl;
  }

  getTextureView(texId, kind, rtId, dsId) {
    if (!texId || texId === rtId || texId === dsId) {
      if (kind === 3) return this.dummyDepth;
      if (kind === 1) return this.dummy3D;
      if (kind === 2) return this.dummyCube;
      return this.dummy2D;
    }
    const t = this.textures.get(texId);
    if (!t) {
      return kind === 3 ? this.dummyDepth : kind === 1 ? this.dummy3D : kind === 2 ? this.dummyCube : this.dummy2D;
    }
    this.touchedTexInEncoder.add(texId);
    if (kind === 3) return t.viewDepth || this.dummyDepth;
    if (kind === 1) return t.dim === 1 ? t.viewSample : this.dummy3D;
    if (kind === 2) return t.dim === 2 ? t.viewSample : this.dummyCube;
    return t.dim === 0 ? t.viewSample : this.dummy2D;
  }

  getSampler(smpId, isDepth) {
    const s = smpId ? this.samplers.get(smpId) : null;
    if (isDepth) {
      return s && s.isCmp ? s.obj : this.defaultCmpSampler;
    }
    return s && !s.isCmp ? s.obj : this.defaultSampler;
  }

  pruneBg1Cache() {
    if (this.dirtyBg1Ids.size === 0) return;
    if (this.dirtyBg1Ids.size >= 8 || this.bg1Cache.size > 4096) {
      this.bg1Cache.clear();
      this.texInBg1.clear();
      this.dirtyBg1Ids.clear();
      return;
    }
    for (const k of this.bg1Cache.keys()) {
      const midStart = k.indexOf(":") + 1;
      const midEnd = k.indexOf(":", midStart);
      if (midStart <= 0 || midEnd <= midStart) continue;
      const parts = k.slice(midStart, midEnd).split(",");
      for (let i = 0; i < 4; i++) {
        if (this.dirtyBg1Ids.has(Number(parts[i]))) {
          this.bg1Cache.delete(k);
          break;
        }
      }
    }
    this.dirtyBg1Ids.clear();
  }

  getBg1(texKinds, texIds, smpIds, rtId, dsId) {
    const resolvedTex = [0, 0, 0, 0];
    const resolvedSmp = [0, 0, 0, 0];
    for (let i = 0; i < 4; i++) {
      const tid = texIds[i];
      const valid = tid && tid !== rtId && tid !== dsId && this.textures.has(tid) ? tid : 0;
      resolvedTex[i] = valid;
      if (valid) this.touchedTexInEncoder.add(valid);
      resolvedSmp[i] = smpIds[i] || 0;
    }
    const key =
      `${texKinds[0]}${texKinds[1]}${texKinds[2]}${texKinds[3]}:` +
      `${resolvedTex[0]},${resolvedTex[1]},${resolvedTex[2]},${resolvedTex[3]}:` +
      `${resolvedSmp[0]},${resolvedSmp[1]},${resolvedSmp[2]},${resolvedSmp[3]}`;
    this.pruneBg1Cache();
    let bg = this.bg1Cache.get(key);
    if (bg) return bg;

    const bgl1 = this.getBgl1(texKinds);
    const entries = [];
    for (let i = 0; i < 4; i++) {
      const k = texKinds[i];
      entries.push({ binding: i, resource: this.getTextureView(texIds[i], k, rtId, dsId) });
      entries.push({ binding: 4 + i, resource: this.getSampler(smpIds[i], k === 3) });
      if (resolvedTex[i]) this.texInBg1.add(resolvedTex[i]);
    }
    bg = this.device.createBindGroup({ layout: bgl1, entries });
    if (this.bg1Cache.size > 4096) {
      this.bg1Cache.clear();
      this.texInBg1.clear();
    }
    this.bg1Cache.set(key, bg);
    return bg;
  }

  getBlitBindGroup(texId, viewSample) {
    let bg = this.blitBgCache.get(texId);
    if (bg) return bg;
    bg = this.device.createBindGroup({
      layout: this.blitBgl,
      entries: [
        { binding: 0, resource: { buffer: this.uniBuf, offset: 0, size: 32 } },
        { binding: 1, resource: viewSample },
        { binding: 2, resource: this.gammaLutView },
        { binding: 3, resource: this.defaultSampler },
      ],
    });
    if (this.blitBgCache.size > 512) this.blitBgCache.clear();
    this.blitBgCache.set(texId, bg);
    return bg;
  }

  getClearPipeline(hasRt, wmask, hasDs, zWrite, stenWrite) {
    const key = `${hasRt ? 1 : 0}:${wmask}:${hasDs ? 1 : 0}:${zWrite ? 1 : 0}:${stenWrite ? 1 : 0}`;
    let pso = this.clearPipelines.get(key);
    if (pso) return pso;

    const desc = {
      label: `black_clear_${key}`,
      layout: this.clearLayout,
      vertex: { module: this.clearModule, entryPoint: "vs_clear" },
      fragment: hasRt
        ? { module: this.clearModule, entryPoint: "fs_clear", targets: [{ format: "bgra8unorm", writeMask: wmask }] }
        : { module: this.clearModule, entryPoint: "fs_clear", targets: [] },
      primitive: { topology: "triangle-list" },
    };
    if (hasDs) {
      desc.depthStencil = {
        format: "depth24plus-stencil8",
        depthWriteEnabled: zWrite,
        depthCompare: "always",
        stencilFront: {
          compare: "always",
          failOp: "keep",
          depthFailOp: "keep",
          passOp: stenWrite ? "replace" : "keep",
        },
        stencilBack: {
          compare: "always",
          failOp: "keep",
          depthFailOp: "keep",
          passOp: stenWrite ? "replace" : "keep",
        },
        stencilWriteMask: stenWrite ? 0xff : 0x00,
      };
    }
    pso = this.device.createRenderPipeline(desc);
    this.clearPipelines.set(key, pso);
    return pso;
  }

  submitBatch(ptr, len) {
    const heap = this.getHeapU8();
    if (!this.ready || !heap || !len) return;
    const t0 = performance.now();
    this.perf.batches++;
    const r = this.reader;
    r.reset(heap, ptr, len);

    while (r.pos + 8 <= r.end) {
      const op = r.u32();
      const payloadBytes = r.u32();
      const nextPos = r.pos + payloadBytes;
      this.dispatchOp(op, r, nextPos);
      r.pos = nextPos;
    }
    this.flushActiveEncoder();
    // writeBuffer enfileira antes de queue.submit; resetar após submit é seguro na timeline WebGPU.
    this.uniCursor = 0;
    this.uniFlushed = 0;
    this.geomCursor = 0;
    this.geomFlushed = 0;
    this.hasVscCache = false;
    this.hasDcCache = false;
    this.perf.submitMs += performance.now() - t0;
  }

  dispatchOp(op, r, _endPos) {
    switch (op) {
      case OP_CREATE_SHADER: {
        const id = r.u32();
        const stage = r.u32();
        const strLen = r.u32();
        const code = r.strAligned(strLen);
        const mod = this.device.createShaderModule({ label: `sh_${stage ? "fs" : "vs"}_${id}`, code });
        mod.getCompilationInfo().then((info) => {
          for (const msg of info.messages) {
            if (msg.type === "error") {
              this.errorCount++;
              if (this.errorCount <= 12) {
                this.onLog("WGSL", `Erro shader #${id} (L${msg.lineNum}): ${msg.message}`);
              }
            }
          }
        });
        this.shaders.set(id, mod);
        break;
      }

      case OP_CREATE_SAMPLER: {
        const id = r.u32();
        const u = r.u32();
        const v = r.u32();
        const w = r.u32();
        const minF = r.u32();
        const magF = r.u32();
        const mipF = r.u32();
        const maxAniso = r.u32();
        const cmpFunc = r.u32();
        const lodMin = r.f32();
        const lodMax = r.f32();
        const isCmp = cmpFunc > 0;
        const canAniso = !isCmp && minF === 1 && magF === 1 && mipF === 1;
        const desc = {
          addressModeU: ADDR_MODES[u] || "repeat",
          addressModeV: ADDR_MODES[v] || "repeat",
          addressModeW: ADDR_MODES[w] || "repeat",
          minFilter: FILTER_MODES[minF] || "nearest",
          magFilter: FILTER_MODES[magF] || "nearest",
          mipmapFilter: FILTER_MODES[mipF] || "nearest",
          maxAnisotropy: canAniso ? Math.max(1, Math.min(16, maxAniso)) : 1,
          lodMinClamp: Math.max(0, lodMin),
          lodMaxClamp: Math.max(lodMin, lodMax),
        };
        if (isCmp) desc.compare = CMP_FUNCS[(cmpFunc - 1) & 7] || "less";
        this.samplers.set(id, { obj: this.device.createSampler(desc), isCmp });
        break;
      }

      case OP_CREATE_TEXTURE: {
        const id = r.u32();
        const fmtIdx = r.u32();
        const dimIdx = r.u32();
        const w = r.u32();
        const h = r.u32();
        const layers = r.u32();
        const mips = r.u32();
        const usageFlags = r.u32();
        const format = TEX_FORMATS[fmtIdx] || "bgra8unorm";
        const isDepth = format === "depth24plus-stencil8";
        let usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC;
        if (!isDepth) usage |= GPUTextureUsage.COPY_DST;
        if (usageFlags !== 0) usage |= GPUTextureUsage.RENDER_ATTACHMENT;
        const tex = this.device.createTexture({
          label: `tex_${id}`,
          size: [w, h, layers],
          dimension: dimIdx === 1 ? "3d" : "2d",
          mipLevelCount: Math.max(1, mips),
          format,
          usage,
        });
        const viewDim = dimIdx === 1 ? "3d" : dimIdx === 2 ? "cube" : "2d";
        const viewSample = isDepth ? null : tex.createView({ dimension: viewDim });
        const viewRt = isDepth ? tex.createView({ aspect: "all" }) : tex.createView({ dimension: "2d" });
        const viewDepth = isDepth ? tex.createView({ aspect: "depth-only" }) : null;
        this.textures.set(id, { tex, viewSample, viewRt, viewDepth, format, dim: dimIdx, w, h, layers });
        break;
      }

      case OP_UPLOAD_TEXTURE: {
        const id = r.u32();
        const mip = r.u32();
        const layerOrZ = r.u32();
        const w = r.u32();
        const h = r.u32();
        const depth = r.u32();
        const bytesPerRow = r.u32();
        const rowsPerImage = r.u32();
        const dataLen = r.u32();
        const data = r.bytesAligned(dataLen);
        const entry = this.textures.get(id);
        if (!entry) break;
        if (this.touchedTexInEncoder.has(id)) {
          this.flushActiveEncoder();
        }
        const origin = entry.dim === 2 ? { x: 0, y: 0, z: layerOrZ } : { x: 0, y: 0, z: 0 };
        const isBc = entry.format.startsWith("bc");
        const cw = isBc ? alignUp(w, 4) : w;
        const ch = isBc ? alignUp(h, 4) : h;
        const uploadBytes =
          typeof SharedArrayBuffer !== "undefined" && data.buffer instanceof SharedArrayBuffer ? data.slice() : data;
        this.device.queue.writeTexture(
          { texture: entry.tex, mipLevel: mip, origin },
          uploadBytes,
          { offset: 0, bytesPerRow, rowsPerImage },
          [cw, ch, depth]
        );
        break;
      }

      case OP_UPLOAD_GAMMA: {
        const lut = r.bytesAligned(1024);
        const lutCopy =
          typeof SharedArrayBuffer !== "undefined" && lut.buffer instanceof SharedArrayBuffer ? lut.slice() : lut;
        this.device.queue.writeTexture({ texture: this.gammaLutTex }, lutCopy, { bytesPerRow: 1024 }, [256, 1, 1]);
        break;
      }

      case OP_DESTROY_TEXTURE: {
        const id = r.u32();
        const t = this.textures.get(id);
        if (t) {
          this.textures.delete(id);
          this.blitBgCache.delete(id);
          this.depthBgCache.delete(id);
          if (this.touchedTexInEncoder.has(id)) {
            this.pendingDestroyTex.push(t.tex);
          } else {
            t.tex.destroy();
          }
          if (this.texInBg1.has(id)) {
            this.texInBg1.delete(id);
            this.dirtyBg1Ids.add(id);
            if (this.dirtyBg1Ids.size >= 8) {
              this.bg1Cache.clear();
              this.texInBg1.clear();
              this.dirtyBg1Ids.clear();
            }
          }
        }
        break;
      }

      case OP_UPLOAD_DEPTH: {
        const srcId = r.u32();
        const dstId = r.u32();
        const w = r.u32();
        const h = r.u32();
        const src = this.textures.get(srcId);
        const dst = this.textures.get(dstId);
        if (!src || !dst) break;
        this.endActivePass();
        this.ensureEncoder();
        let bg = this.depthBgCache.get(srcId);
        if (!bg) {
          bg = this.device.createBindGroup({
            layout: this.depthBgl,
            entries: [{ binding: 0, resource: src.viewSample }],
          });
          this.depthBgCache.set(srcId, bg);
        }
        const pass = this.encoder.beginRenderPass({
          colorAttachments: [],
          depthStencilAttachment: {
            view: dst.viewRt,
            depthLoadOp: "load",
            depthStoreOp: "store",
            stencilLoadOp: "load",
            stencilStoreOp: "store",
          },
        });
        pass.setViewport(0, 0, w, h, 0, 1);
        pass.setScissorRect(0, 0, w, h);
        pass.setPipeline(this.depthPipeline);
        pass.setBindGroup(0, bg);
        pass.draw(3);
        pass.end();
        this.touchedTexInEncoder.add(srcId);
        this.touchedTexInEncoder.add(dstId);
        break;
      }

      case OP_CREATE_PIPELINE: {
        const psoId = r.u32();
        const vsId = r.u32();
        const fsId = r.u32();
        const topo = r.u32();
        const stripIdx = r.u32();
        const cull = r.u32();
        const ccw = r.u32();
        const hasRt = r.u32();
        const hasDs = r.u32();
        const blendEn = r.u32();
        const srcRgb = r.u32();
        const dstRgb = r.u32();
        const opRgb = r.u32();
        const srcA = r.u32();
        const dstA = r.u32();
        const opA = r.u32();
        const wmask = r.u32();
        const zEn = r.u32();
        const zWrite = r.u32();
        const zFunc = r.u32();
        const stenEn = r.u32();
        const stenRmask = r.u32();
        const stenWmask = r.u32();
        const stenFail = r.u32();
        const stenZfail = r.u32();
        const stenPass = r.u32();
        const stenFunc = r.u32();
        const zBias = r.i32();
        const zSlope = r.f32();
        const texKinds = [r.u32(), r.u32(), r.u32(), r.u32()];
        const vbCount = r.u32();
        const buffers = [];
        for (let b = 0; b < vbCount; b++) {
          const stride = r.u32();
          const attrCount = r.u32();
          const attributes = [];
          for (let a = 0; a < attrCount; a++) {
            const shaderLocation = r.u32();
            const fmtCode = r.u32();
            const offset = r.u32();
            attributes.push({
              shaderLocation,
              format: VERTEX_FORMATS[fmtCode] || "float32x4",
              offset,
            });
          }
          buffers.push({ arrayStride: stride, stepMode: "vertex", attributes });
        }

        const vsMod = this.shaders.get(vsId);
        const fsMod = this.shaders.get(fsId);
        if (!vsMod || !fsMod) break;

        const targetDesc = { format: "bgra8unorm", writeMask: wmask & 0xf };
        if (blendEn) {
          targetDesc.blend = {
            color: {
              operation: BLEND_OPS[opRgb] || "add",
              srcFactor: BLEND_FACTORS[srcRgb] || "one",
              dstFactor: BLEND_FACTORS[dstRgb] || "zero",
            },
            alpha: {
              operation: BLEND_OPS[opA] || "add",
              srcFactor: BLEND_FACTORS[srcA] || "one",
              dstFactor: BLEND_FACTORS[dstA] || "zero",
            },
          };
        }

        const isStrip = topo === 2 || topo === 4;
        const isTri = topo === 3 || topo === 4;
        const primitive = {
          topology: TOPOLOGIES[topo] || "triangle-list",
          cullMode: cull ? "back" : "none",
          frontFace: ccw ? "ccw" : "cw",
        };
        if (isStrip && stripIdx) {
          primitive.stripIndexFormat = stripIdx === 2 ? "uint32" : "uint16";
        }

        const layout = this.device.createPipelineLayout({
          bindGroupLayouts: [this.bgl0, this.getBgl1(texKinds)],
        });
        const psoDesc = {
          label: `pso_${psoId}`,
          layout,
          vertex: { module: vsMod, entryPoint: "vs_main", buffers },
          fragment: { module: fsMod, entryPoint: "fs_main", targets: hasRt ? [targetDesc] : [] },
          primitive,
        };
        if (hasDs) {
          const zc = zEn ? CMP_FUNCS[zFunc & 7] || "always" : "always";
          const dsDesc = {
            format: "depth24plus-stencil8",
            depthWriteEnabled: Boolean(zEn && zWrite),
            depthCompare: zc,
          };
          if (stenEn) {
            const st = {
              compare: CMP_FUNCS[stenFunc & 7] || "always",
              failOp: STENCIL_OPS[stenFail] || "keep",
              depthFailOp: STENCIL_OPS[stenZfail] || "keep",
              passOp: STENCIL_OPS[stenPass] || "keep",
            };
            dsDesc.stencilFront = st;
            dsDesc.stencilBack = st;
            dsDesc.stencilReadMask = stenRmask & 0xff;
            dsDesc.stencilWriteMask = stenWmask & 0xff;
          }
          if (isTri && (zBias !== 0 || zSlope !== 0)) {
            dsDesc.depthBias = zBias;
            dsDesc.depthBiasSlopeScale = zSlope;
          }
          psoDesc.depthStencil = dsDesc;
        }
        this.pipelines.set(psoId, { pso: this.device.createRenderPipeline(psoDesc), vbCount });
        break;
      }

      case OP_CLEAR: {
        const rtId = r.u32();
        const dsId = r.u32();
        const clearFlags = r.u32();
        const colorMask = r.u32();
        const col = [r.f32(), r.f32(), r.f32(), r.f32()];
        const zRev = r.f32();
        const stencil = r.u32();
        const rectCount = r.u32();
        const rects = [];
        for (let i = 0; i < rectCount; i++) {
          rects.push([r.i32(), r.i32(), r.i32(), r.i32()]);
        }

        const rt = rtId ? this.textures.get(rtId) : null;
        const ds = dsId ? this.textures.get(dsId) : null;
        if (!rt && !ds) break;
        const refW = rt ? rt.w : ds.w;
        const refH = rt ? rt.h : ds.h;

        this.endActivePass();
        this.ensureEncoder();
        if (rt) this.touchedTexInEncoder.add(rtId);
        if (ds) this.touchedTexInEncoder.add(dsId);

        const isFullSingle =
          rects.length === 1 &&
          rects[0][0] <= 0 &&
          rects[0][1] <= 0 &&
          rects[0][2] >= refW &&
          rects[0][3] >= refH &&
          (!(clearFlags & 1) || colorMask === 0xf);

        if (isFullSingle) {
          const rpDesc = {
            colorAttachments: rt
              ? [
                  {
                    view: rt.viewRt,
                    loadOp: clearFlags & 1 ? "clear" : "load",
                    storeOp: "store",
                    clearValue: { r: col[0], g: col[1], b: col[2], a: col[3] },
                  },
                ]
              : [],
          };
          if (ds) {
            rpDesc.depthStencilAttachment = {
              view: ds.viewRt,
              depthLoadOp: clearFlags & 2 ? "clear" : "load",
              depthStoreOp: "store",
              depthClearValue: Math.max(0, Math.min(1, zRev)),
              stencilLoadOp: clearFlags & 4 ? "clear" : "load",
              stencilStoreOp: "store",
              stencilClearValue: stencil & 0xff,
            };
          }
          const pass = this.encoder.beginRenderPass(rpDesc);
          pass.end();
        } else {
          this.tmpClear[0] = col[0];
          this.tmpClear[1] = col[1];
          this.tmpClear[2] = col[2];
          this.tmpClear[3] = col[3];
          this.tmpClear[4] = Math.max(0, Math.min(1, zRev));
          this.tmpClear[5] = 0;
          this.tmpClear[6] = 0;
          this.tmpClear[7] = 0;
          const off = this.uploadUniSlice(this.tmpClearU8);
          const pso = this.getClearPipeline(
            Boolean(rt),
            clearFlags & 1 ? colorMask & 0xf : 0,
            Boolean(ds),
            Boolean(ds && (clearFlags & 2)),
            Boolean(ds && (clearFlags & 4))
          );
          const rpDesc = {
            colorAttachments: rt ? [{ view: rt.viewRt, loadOp: "load", storeOp: "store" }] : [],
          };
          if (ds) {
            rpDesc.depthStencilAttachment = {
              view: ds.viewRt,
              depthLoadOp: "load",
              depthStoreOp: "store",
              stencilLoadOp: "load",
              stencilStoreOp: "store",
            };
          }
          const pass = this.encoder.beginRenderPass(rpDesc);
          pass.setPipeline(pso);
          pass.setBindGroup(0, this.clearBg, [off]);
          pass.setStencilReference(stencil & 0xff);
          pass.setViewport(0, 0, refW, refH, 0, 1);
          for (const rc of rects) {
            const x0 = Math.max(0, Math.min(refW, rc[0]));
            const y0 = Math.max(0, Math.min(refH, rc[1]));
            const x1 = Math.max(0, Math.min(refW, rc[2]));
            const y1 = Math.max(0, Math.min(refH, rc[3]));
            if (x1 <= x0 || y1 <= y0) continue;
            pass.setScissorRect(x0, y0, x1 - x0, y1 - y0);
            pass.draw(3);
          }
          pass.end();
        }
        break;
      }

      case OP_DRAW: {
        this.perf.draws++;
        const psoId = r.u32();
        const rtId = r.u32();
        const dsId = r.u32();
        const texIds = [r.u32(), r.u32(), r.u32(), r.u32()];
        const smpIds = [r.u32(), r.u32(), r.u32(), r.u32()];
        const texKinds = [r.u32(), r.u32(), r.u32(), r.u32()];
        const sx = r.u32();
        const sy = r.u32();
        const sw = r.u32();
        const sh = r.u32();
        const bf = [r.f32(), r.f32(), r.f32(), r.f32()];
        const stenRef = r.u32();
        const indexed = r.u32();
        const count = r.u32();
        const baseVertex = r.i32();
        const idxBytes = r.u32();
        const vbCount = r.u32();
        const vscDirty = r.u32();
        const dcDirty = r.u32();

        const vscBytes = vscDirty ? r.bytesAligned(VSC_BYTES) : null;
        const dcBytes = dcDirty ? r.bytesAligned(DC_BYTES) : null;
        const idxSlice = idxBytes > 0 ? r.bytesAligned(idxBytes) : null;
        const vbSlices = [];
        for (let b = 0; b < vbCount; b++) {
          const vbLen = r.u32();
          vbSlices.push(r.bytesAligned(vbLen));
        }

        const psoEntry = this.pipelines.get(psoId);
        const rt = rtId ? this.textures.get(rtId) : null;
        const ds = dsId ? this.textures.get(dsId) : null;
        if (!psoEntry || (!rt && !ds) || sw === 0 || sh === 0) break;

        // Evita textura sendo usada como RT/DS no mesmo pass (substitui por dummy na bindgroup).
        this.ensureEncoder();

        let uniNeed = 0;
        if (vscBytes || !this.hasVscCache) uniNeed += alignUp(VSC_BYTES, this.uniAlign);
        if (dcBytes || !this.hasDcCache) uniNeed += alignUp(DC_BYTES, this.uniAlign);
        if (this.uniCursor + uniNeed > this.uniBuf.size) {
          this.flushActiveEncoder();
          this.allocUniArena(Math.max(this.uniBuf.size * 2, uniNeed * 32));
        }

        let vscOff = this.lastVscOff;
        let dcOff = this.lastDcOff;
        if (vscBytes || !this.hasVscCache) {
          if (vscBytes) this.vscCache.set(vscBytes);
          vscOff = this.uploadUniSlice(this.vscCache);
          this.lastVscOff = vscOff;
          this.hasVscCache = true;
        }
        if (dcBytes || !this.hasDcCache) {
          if (dcBytes) this.dcCache.set(dcBytes);
          dcOff = this.uploadUniSlice(this.dcCache);
          this.lastDcOff = dcOff;
          this.hasDcCache = true;
        }

        let geomNeed = idxSlice ? alignUp(idxSlice.byteLength, 4) : 0;
        for (let b = 0; b < vbCount; b++) geomNeed += alignUp(vbSlices[b].byteLength, 4);
        if (this.geomCursor + geomNeed > this.geomBuf.size) {
          this.flushActiveEncoder();
          this.allocGeomArena(Math.max(this.geomBuf.size * 2, geomNeed * 8));
        }

        const idxOff = idxSlice ? this.uploadGeomSlice(idxSlice) : 0;
        const vbOffsets = [];
        for (let b = 0; b < vbCount; b++) {
          vbOffsets.push(this.uploadGeomSlice(vbSlices[b]));
        }

        if (!this.pass || this.passRtId !== rtId || this.passDsId !== dsId) {
          this.endActivePass();
          const rpDesc = {
            colorAttachments: rt ? [{ view: rt.viewRt, loadOp: "load", storeOp: "store" }] : [],
          };
          if (ds) {
            rpDesc.depthStencilAttachment = {
              view: ds.viewRt,
              depthLoadOp: "load",
              depthStoreOp: "store",
              stencilLoadOp: "load",
              stencilStoreOp: "store",
            };
          }
          this.pass = this.encoder.beginRenderPass(rpDesc);
          this.passRtId = rtId;
          this.passDsId = dsId;
          if (rt) this.touchedTexInEncoder.add(rtId);
          if (ds) this.touchedTexInEncoder.add(dsId);
        }

        const refW = rt ? rt.w : ds.w;
        const refH = rt ? rt.h : ds.h;
        this.pass.setViewport(0, 0, refW, refH, 0, 1);
        this.pass.setScissorRect(sx, sy, sw, sh);
        this.pass.setBlendConstant({ r: bf[0], g: bf[1], b: bf[2], a: bf[3] });
        this.pass.setStencilReference(stenRef & 0xff);
        this.pass.setPipeline(psoEntry.pso);
        this.pass.setBindGroup(0, this.uniGroup0, [vscOff, dcOff]);
        this.pass.setBindGroup(1, this.getBg1(texKinds, texIds, smpIds, rtId, dsId));

        for (let b = 0; b < psoEntry.vbCount; b++) {
          const sl = vbSlices[b];
          if (sl && sl.byteLength > 0) {
            this.pass.setVertexBuffer(b, this.geomBuf, vbOffsets[b], alignUp(sl.byteLength, 4));
          } else {
            this.pass.setVertexBuffer(b, this.dummyVb);
          }
        }

        if (indexed && idxSlice) {
          this.pass.setIndexBuffer(
            this.geomBuf,
            indexed === 2 ? "uint32" : "uint16",
            idxOff,
            alignUp(idxBytes, 4)
          );
          this.pass.drawIndexed(count, 1, 0, baseVertex, 0);
        } else {
          this.pass.draw(count);
        }
        break;
      }

      case OP_COPY_TEX: {
        const srcId = r.u32();
        const dstId = r.u32();
        const useBlit = r.u32();
        const n = r.u32();
        const rects = [];
        for (let i = 0; i < n; i++) {
          rects.push({
            sx: r.u32(),
            sy: r.u32(),
            sw: r.u32(),
            sh: r.u32(),
            dx: r.u32(),
            dy: r.u32(),
            dw: r.u32(),
            dh: r.u32(),
          });
        }
        const src = this.textures.get(srcId);
        const dst = this.textures.get(dstId);
        if (!src || !dst) break;

        this.endActivePass();
        this.ensureEncoder();
        this.touchedTexInEncoder.add(srcId);
        this.touchedTexInEncoder.add(dstId);

        for (const rc of rects) {
          const sw = Math.min(rc.sw, Math.max(0, src.w - rc.sx));
          const sh = Math.min(rc.sh, Math.max(0, src.h - rc.sy));
          const dw = Math.min(rc.dw, Math.max(0, dst.w - rc.dx));
          const dh = Math.min(rc.dh, Math.max(0, dst.h - rc.dy));
          if (sw === 0 || sh === 0 || dw === 0 || dh === 0) continue;

          if (!useBlit && sw === dw && sh === dh) {
            this.encoder.copyTextureToTexture(
              { texture: src.tex, origin: { x: rc.sx, y: rc.sy, z: 0 } },
              { texture: dst.tex, origin: { x: rc.dx, y: rc.dy, z: 0 } },
              [sw, sh, 1]
            );
          } else {
            this.tmpBp[0] = rc.sx / src.w;
            this.tmpBp[1] = rc.sy / src.h;
            this.tmpBp[2] = (rc.sx + sw) / src.w;
            this.tmpBp[3] = (rc.sy + sh) / src.h;
            this.tmpBp[4] = 0;
            this.tmpBp[5] = 0;
            this.tmpBp[6] = 0;
            this.tmpBp[7] = 0;
            const off = this.uploadUniSlice(this.tmpBpU8);
            const bg = this.getBlitBindGroup(srcId, src.viewSample);
            const pass = this.encoder.beginRenderPass({
              colorAttachments: [{ view: dst.viewRt, loadOp: "load", storeOp: "store" }],
            });
            pass.setPipeline(this.blitPipelineBgra);
            pass.setBindGroup(0, bg, [off]);
            pass.setViewport(rc.dx, rc.dy, dw, dh, 0, 1);
            pass.setScissorRect(rc.dx, rc.dy, dw, dh);
            pass.draw(3);
            pass.end();
          }
        }
        break;
      }

      case OP_PRESENT: {
        const frontId = r.u32();
        const wide = r.u32();
        const gammaOn = r.u32();
        const _dstW = r.u32();
        const _dstH = r.u32();

        this.endActivePass();
        this.ensureEncoder();

        const swapTex = this.context.getCurrentTexture();
        const cw = swapTex.width;
        const ch = swapTex.height;
        const swapView = swapTex.createView();

        const front = frontId ? this.textures.get(frontId) : null;
        if (!front || cw === 0 || ch === 0) {
          const pass = this.encoder.beginRenderPass({
            colorAttachments: [
              { view: swapView, loadOp: "clear", storeOp: "store", clearValue: { r: 0, g: 0, b: 0, a: 1 } },
            ],
          });
          pass.end();
          break;
        }

        this.touchedTexInEncoder.add(frontId);
        const aspect = wide ? 16 / 9 : 4 / 3;
        let vw = cw;
        let vh = ch;
        if (cw / ch > aspect) {
          vw = Math.round(ch * aspect);
        } else {
          vh = Math.round(cw / aspect);
        }
        const vx = Math.floor((cw - vw) * 0.5);
        const vy = Math.floor((ch - vh) * 0.5);

        this.tmpBp[0] = 0;
        this.tmpBp[1] = 0;
        this.tmpBp[2] = 1;
        this.tmpBp[3] = 1;
        this.tmpBp[4] = gammaOn ? 1 : 0;
        this.tmpBp[5] = 0;
        this.tmpBp[6] = 0;
        this.tmpBp[7] = 0;
        const off = this.uploadUniSlice(this.tmpBpU8);
        const bg = this.getBlitBindGroup(frontId, front.viewSample);
        const pass = this.encoder.beginRenderPass({
          colorAttachments: [
            { view: swapView, loadOp: "clear", storeOp: "store", clearValue: { r: 0, g: 0, b: 0, a: 1 } },
          ],
        });
        pass.setPipeline(this.blitPipelineCanvas);
        pass.setBindGroup(0, bg, [off]);
        pass.setViewport(vx, vy, Math.max(1, vw), Math.max(1, vh), 0, 1);
        pass.setScissorRect(0, 0, cw, ch);
        pass.draw(3);
        pass.end();
        this.perf.presents++;
        const now = performance.now();
        const dt = Math.max(1, now - this.perf.winStart);
        if (this.perf.presents >= 60 || (this.perf.presents >= 2 && dt >= 2000)) {
          const p = this.perf.presents;
          window.__blackGpuPerf = {
            fps: +(p * 1000 / dt).toFixed(1),
            frameMs: +(dt / p).toFixed(2),
            drawsPerFrame: +(this.perf.draws / p).toFixed(1),
            batchesPerFrame: +(this.perf.batches / p).toFixed(2),
            submitMsPerFrame: +(this.perf.submitMs / p).toFixed(2),
            writeBufCallsPerFrame: +(this.perf.writeBufCalls / p).toFixed(1),
            writeBufKBPerFrame: +(this.perf.writeBufBytes / (1024 * p)).toFixed(1),
            flushEncPerFrame: +(this.perf.flushEncCalls / p).toFixed(2),
            readbacks: this.perf.readbacks,
            readbackMs: +this.perf.readbackMs.toFixed(2),
            canvasW: cw,
            canvasH: ch,
            dpr: window.devicePixelRatio || 1,
          };
          this.perf.winStart = now;
          this.perf.presents = 0;
          this.perf.batches = 0;
          this.perf.draws = 0;
          this.perf.submitMs = 0;
          this.perf.writeBufCalls = 0;
          this.perf.writeBufBytes = 0;
          this.perf.flushEncCalls = 0;
          this.perf.readbacks = 0;
          this.perf.readbackMs = 0;
        }
        break;
      }
    }
  }

  async readbackTexture(texId, w, h, outPtr) {
    if (!this.ready || !this.module) return;
    const t0 = performance.now();
    this.perf.readbacks++;
    const entry = this.textures.get(texId);
    if (!entry || w === 0 || h === 0) return;

    this.flushActiveEncoder();
    const copyW = Math.min(w, entry.w);
    const copyH = Math.min(h, entry.h);
    const bpr = alignUp(copyW * 4, 256);
    const staging = this.device.createBuffer({
      size: bpr * copyH,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = this.device.createCommandEncoder();
    enc.copyTextureToBuffer(
      { texture: entry.tex, origin: { x: 0, y: 0, z: 0 } },
      { buffer: staging, offset: 0, bytesPerRow: bpr, rowsPerImage: copyH },
      [copyW, copyH, 1]
    );
    this.device.queue.submit([enc.finish()]);

    await staging.mapAsync(GPUMapMode.READ);
    const mapped = new Uint8Array(staging.getMappedRange());
    const heap = this.getHeapU8();
    const rowBytes = copyW * 4;
    for (let y = 0; y < copyH; y++) {
      heap.set(mapped.subarray(y * bpr, y * bpr + rowBytes), outPtr + y * w * 4);
    }
    staging.unmap();
    staging.destroy();
    this.perf.readbackMs += performance.now() - t0;
  }

  async waitIdle() {
    if (!this.device) return;
    this.flushActiveEncoder();
    await this.device.queue.onSubmittedWorkDone();
  }
}
