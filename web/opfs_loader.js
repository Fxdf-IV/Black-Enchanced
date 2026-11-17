// OPFS (Origin Private File System) Loader & XBE Verifier — Black Enhanced Online
// Lê a pasta/arquivos locais do jogador via File System Access API, valida o cabeçalho
// do default.xbe (magic "XBEH" = 0x48454258 e Title ID 0x45410083 de BLACK NTSC-U)
// e copia os arquivos para o armazenamento privado do navegador sem enviar nada pela rede.

export const BLACK_TITLE_ID = 0x45410083;
export const XBE_MAGIC = 0x48454258; // "XBEH" little-endian
export const REAL_XBE_MIN_BYTES = 1_000_000;
export const BLACK_RETAIL_SHA256 = "DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6";

export function inspectXbeHeader(arrayBuffer) {
  if (!arrayBuffer || arrayBuffer.byteLength < 0x180) {
    return { valid: false, reason: "Arquivo menor que um cabeçalho XBE válido (< 384 bytes)." };
  }
  const view = new DataView(arrayBuffer);
  const magic = view.getUint32(0x000, true);
  if (magic !== XBE_MAGIC) {
    return {
      valid: false,
      reason: `Assinatura inválida (esperado XBEH 0x48454258, lido 0x${magic.toString(16).toUpperCase()}).`,
    };
  }

  const baseAddr = view.getUint32(0x104, true);
  const certAddr = view.getUint32(0x118, true);
  const certOffset = certAddr - baseAddr;
  let titleId = 0;
  if (certOffset >= 0 && certOffset + 0x0c <= arrayBuffer.byteLength) {
    titleId = view.getUint32(certOffset + 0x08, true);
  }

  const isBlackTitle = titleId === BLACK_TITLE_ID;
  const isRealSize = arrayBuffer.byteLength >= REAL_XBE_MIN_BYTES;
  let reason = "";
  if (!isBlackTitle) {
    reason = `Title ID 0x${titleId.toString(16).toUpperCase()} não corresponde a BLACK (0x45410083).`;
  } else if (!isRealSize) {
    reason = `default.xbe incompleto (${(arrayBuffer.byteLength / 1024).toFixed(1)} KB; esperado executável Xbox real > 1 MB).`;
  } else {
    reason = "default.xbe real de BLACK (Title ID 0x45410083, XDK 5849) verificado.";
  }

  return {
    valid: isBlackTitle && isRealSize,
    magic,
    baseAddr,
    certAddr,
    titleId,
    byteLength: arrayBuffer.byteLength,
    reason,
  };
}

export async function sha256Hex(arrayBuffer) {
  const digest = await crypto.subtle.digest("SHA-256", arrayBuffer);
  const bytes = new Uint8Array(digest);
  let out = "";
  for (let i = 0; i < bytes.length; i++) {
    out += bytes[i].toString(16).padStart(2, "0");
  }
  return out;
}

function normalizeRelativeEntries(fileList) {
  const list = Array.from(fileList || []);
  const rawPaths = list.map((f) => (f.webkitRelativePath || f.name || "").replace(/\\/g, "/").replace(/^\/+/, ""));
  let commonPrefix = "";
  if (rawPaths.length > 0 && rawPaths.every((p) => p.includes("/"))) {
    const firstSeg = rawPaths[0].split("/")[0];
    if (firstSeg && rawPaths.every((p) => p.startsWith(firstSeg + "/"))) {
      commonPrefix = firstSeg + "/";
    }
  }
  return list.map((file, i) => ({
    file,
    relPath: commonPrefix ? rawPaths[i].slice(commonPrefix.length) : rawPaths[i],
  }));
}

export async function stageFilesToOpfs(fileList, onProgress) {
  let root = null;
  if (navigator.storage && navigator.storage.getDirectory) {
    root = await navigator.storage.getDirectory();
  }

  let xbeReport = null;
  let totalBytes = 0;
  let count = 0;
  const entries = normalizeRelativeEntries(fileList);

  for (const { file, relPath } of entries) {
    const baseName = relPath.split("/").pop().toLowerCase();

    if (baseName === "default.xbe") {
      const buf = await file.arrayBuffer();
      const info = inspectXbeHeader(buf);
      const hash = (await sha256Hex(buf)).toUpperCase();
      xbeReport = {
        ...info,
        sha256: hash,
        retailShaMatch: hash === BLACK_RETAIL_SHA256,
        path: relPath,
      };
    }

    if (root) {
      const segments = relPath.split("/").filter(Boolean);
      const fileName = segments.pop();
      let dir = await root.getDirectoryHandle("black_assets", { create: true });
      for (const seg of segments) {
        dir = await dir.getDirectoryHandle(seg, { create: true });
      }
      const handle = await dir.getFileHandle(fileName, { create: true });
      if (handle.createWritable) {
        const writable = await handle.createWritable();
        await writable.write(await file.arrayBuffer());
        await writable.close();
      }
    }

    totalBytes += file.size;
    count++;
    if (onProgress) {
      onProgress({ count, totalFiles: fileList.length, totalBytes, currentFile: relPath });
    }
  }

  return {
    stagedFiles: count,
    totalBytes,
    opfsAvailable: Boolean(root),
    xbeReport,
  };
}

export async function checkOpfsCached() {
  if (!navigator.storage || !navigator.storage.getDirectory) {
    return { cached: false, reason: "OPFS não suportado neste navegador." };
  }
  try {
    const root = await navigator.storage.getDirectory();
    const dir = await root.getDirectoryHandle("black_assets", { create: false });
    const xbeHandle = await dir.getFileHandle("default.xbe", { create: false });
    const file = await xbeHandle.getFile();
    const buf = await file.arrayBuffer();
    const info = inspectXbeHeader(buf);
    if (info.valid) {
      const hash = (await sha256Hex(buf)).toUpperCase();
      return {
        cached: true,
        info,
        sha256: hash,
        retailShaMatch: hash === BLACK_RETAIL_SHA256,
        sizeBytes: file.size,
      };
    }
    return { cached: false, reason: info.reason };
  } catch {
    return { cached: false, reason: "Nenhum asset gravado ainda no OPFS." };
  }
}

export async function streamAssetsFromRemote(baseUrl = "", onProgress) {
  let root = null;
  if (navigator.storage && navigator.storage.getDirectory) {
    root = await navigator.storage.getDirectory();
  }
  if (!root) {
    throw new Error("OPFS não suportado pelo navegador.");
  }

  const manifestRes = await fetch(`${baseUrl}/api/manifest`);
  if (!manifestRes.ok) {
    throw new Error(`Falha ao obter manifesto de assets: HTTP ${manifestRes.status}`);
  }
  const manifest = await manifestRes.json();
  if (!manifest.available || !manifest.files || manifest.files.length === 0) {
    return { success: false, reason: "Manifesto de assets ainda não disponível no servidor.", manifest };
  }

  let totalBytes = manifest.total_bytes || 0;
  let downloadedBytes = 0;
  let count = 0;
  let xbeReport = null;

  for (const item of manifest.files) {
    const relPath = item.path.replace(/\\/g, "/");
    const fileUrl = manifest.cdn_url ? `${manifest.cdn_url}/${relPath}` : `${baseUrl}/game_assets/${relPath}`;

    const res = await fetch(fileUrl);
    if (!res.ok) {
      throw new Error(`Erro baixando ${relPath}: HTTP ${res.status}`);
    }
    const buf = await res.arrayBuffer();

    const segments = relPath.split("/").filter(Boolean);
    const fileName = segments.pop();
    let dir = await root.getDirectoryHandle("black_assets", { create: true });
    for (const seg of segments) {
      dir = await dir.getDirectoryHandle(seg, { create: true });
    }
    const handle = await dir.getFileHandle(fileName, { create: true });
    if (handle.createWritable) {
      const writable = await handle.createWritable();
      await writable.write(buf);
      await writable.close();
    }

    if (fileName.toLowerCase() === "default.xbe") {
      const info = inspectXbeHeader(buf);
      const hash = (await sha256Hex(buf)).toUpperCase();
      xbeReport = {
        ...info,
        sha256: hash,
        retailShaMatch: hash === BLACK_RETAIL_SHA256,
        path: relPath,
      };
    }

    downloadedBytes += buf.byteLength;
    count++;
    if (onProgress) {
      onProgress({
        count,
        totalFiles: manifest.files.length,
        downloadedBytes,
        totalBytes,
        percent: totalBytes > 0 ? (downloadedBytes / totalBytes) * 100 : 100,
        currentFile: relPath,
      });
    }
  }

  return {
    success: true,
    stagedFiles: count,
    totalBytes: downloadedBytes,
    xbeReport,
  };
}

export async function readOpfsJson(relPath) {
  if (!navigator.storage || !navigator.storage.getDirectory) return null;
  try {
    const root = await navigator.storage.getDirectory();
    let dir = await root.getDirectoryHandle("black_assets", { create: false });
    const segments = relPath.replace(/\\/g, "/").split("/").filter(Boolean);
    const fileName = segments.pop();
    for (const seg of segments) {
      dir = await dir.getDirectoryHandle(seg, { create: false });
    }
    const handle = await dir.getFileHandle(fileName, { create: false });
    const file = await handle.getFile();
    return JSON.parse(await file.text());
  } catch {
    return null;
  }
}

export async function listOpfsAssetFiles() {
  if (!navigator.storage || !navigator.storage.getDirectory) {
    return { available: false, mode: "none", hasXbe: false, hasIso: false, totalBytes: 0, files: [] };
  }
  try {
    const root = await navigator.storage.getDirectory();
    const assetsDir = await root.getDirectoryHandle("black_assets", { create: false });
    const files = [];
    let totalBytes = 0;
    let hasXbe = false;
    let isoEntry = null;

    async function walk(dirHandle, prefix) {
      for await (const [name, handle] of dirHandle.entries()) {
        if (name.startsWith(".")) continue;
        const relPath = prefix ? `${prefix}/${name}` : name;
        if (handle.kind === "directory") {
          await walk(handle, relPath);
        } else if (handle.kind === "file") {
          const f = await handle.getFile();
          const low = relPath.toLowerCase();
          if (low === "default.xbe" && f.size >= REAL_XBE_MIN_BYTES) hasXbe = true;
          const item = { path: relPath, size: f.size, handle, file: f };
          if ((low.endsWith(".iso") || low.endsWith(".xiso")) && f.size > 100_000_000) {
            isoEntry = item;
          } else {
            totalBytes += f.size;
            files.push(item);
          }
        }
      }
    }

    await walk(assetsDir, "");
    files.sort((a, b) => a.path.localeCompare(b.path));
    if (hasXbe && files.length > 0) {
      return { available: true, mode: "vfs", hasXbe: true, hasIso: Boolean(isoEntry), totalBytes, files };
    }
    if (isoEntry) {
      return {
        available: true,
        mode: "iso",
        hasXbe: false,
        hasIso: true,
        totalBytes: isoEntry.size,
        files: [isoEntry],
      };
    }
    return { available: false, mode: "none", hasXbe: false, hasIso: false, totalBytes, files };
  } catch {
    return { available: false, mode: "none", hasXbe: false, hasIso: false, totalBytes: 0, files: [] };
  }
}

export async function normalizePickedFiles(fileList) {
  const entries = normalizeRelativeEntries(fileList).filter((it) => it.relPath && !it.relPath.split("/").pop().startsWith("."));
  if (entries.length === 0) {
    return { valid: false, mode: "none", files: [], totalBytes: 0, xbeReport: null, reason: "Nenhum arquivo selecionado." };
  }

  let isoCandidate = null;
  let xbeEntry = null;
  let totalBytes = 0;

  for (const it of entries) {
    const low = it.relPath.toLowerCase();
    totalBytes += it.file.size;
    if (low === "default.xbe") {
      xbeEntry = it;
    } else if (!xbeEntry && (low === "default.xbe" || low.endsWith("/default.xbe"))) {
      xbeEntry = it;
    }
    if (low.endsWith(".iso") || low.endsWith(".xiso")) {
      if (!isoCandidate || it.file.size > isoCandidate.file.size) isoCandidate = it;
    }
  }

  if (xbeEntry) {
    const buf = await xbeEntry.file.arrayBuffer();
    const info = inspectXbeHeader(buf);
    const hash = (await sha256Hex(buf)).toUpperCase();
    const xbeReport = {
      ...info,
      sha256: hash,
      retailShaMatch: hash === BLACK_RETAIL_SHA256,
      path: xbeEntry.relPath,
    };
    const files = entries.map((it, idx) => ({
      id: idx,
      path: it.relPath,
      size: it.file.size,
      file: it.file,
    }));
    return {
      valid: info.valid,
      mode: "vfs",
      files,
      totalBytes,
      xbeReport,
      sourceLabel: `${files.length} arquivos locais (${(totalBytes / (1024 * 1024)).toFixed(1)} MB)`,
      reason: info.reason,
    };
  }

  if (isoCandidate) {
    return {
      valid: true,
      mode: "iso",
      files: [{ id: 0, path: isoCandidate.relPath.split("/").pop(), size: isoCandidate.file.size, file: isoCandidate.file }],
      totalBytes: isoCandidate.file.size,
      xbeReport: null,
      sourceLabel: `ISO Xbox (${isoCandidate.file.name}, ${(isoCandidate.file.size / (1024 * 1024)).toFixed(1)} MB)`,
      reason: `Imagem ISO selecionada (${isoCandidate.file.name}).`,
    };
  }

  return {
    valid: false,
    mode: "none",
    files: [],
    totalBytes,
    xbeReport: null,
    reason: "Selecione a pasta contendo default.xbe de BLACK ou um arquivo .iso/.xiso.",
  };
}

export function createBrowserVfsBridge({ onLog } = {}) {
  const CHUNK_SIZE = 256 * 1024;
  const MAX_CACHED_CHUNKS = 192; // ~48 MiB máx em RAM

  const bridge = {
    mode: "none", // "none" | "vfs" | "iso"
    source: "none", // "none" | "local" | "opfs" | "http+opfs"
    entries: [],
    fileCache: new Map(),
    chunkCache: new Map(),
    inflight: new Map(),
    module: null,
    stats: {
      reads: 0,
      bytesServed: 0,
      cacheHits: 0,
      rangeFetches: 0,
      lastFile: "",
    },

    setModule(mod) {
      this.module = mod;
    },

    getHeapU8() {
      const m = this.module;
      if (!m) return null;
      if (m.wasmMemory && (!m.HEAPU8 || m.HEAPU8.buffer !== m.wasmMemory.buffer)) {
        return new Uint8Array(m.wasmMemory.buffer);
      }
      return m.HEAPU8;
    },

    clearCache() {
      this.fileCache.clear();
      this.chunkCache.clear();
      this.inflight.clear();
    },

    setFromLocalPicked(picked) {
      this.clearCache();
      if (!picked || !picked.valid) {
        this.mode = "none";
        this.source = "none";
        this.entries = [];
        return;
      }
      this.mode = picked.mode;
      this.source = "local";
      this.entries = picked.files.map((f, idx) => ({
        id: idx,
        path: f.path,
        size: f.size,
        file: f.file,
        handle: null,
        remoteUrl: null,
      }));
      if (onLog) onLog(`VFS local montado em modo ${this.mode.toUpperCase()} (${this.entries.length} entrada(s)).`);
    },

    setFromOpfsAndManifest({ opfsScan, manifest, baseUrl = "" }) {
      this.clearCache();
      const opfsByPath = new Map();
      if (opfsScan && Array.isArray(opfsScan.files)) {
        for (const f of opfsScan.files) {
          opfsByPath.set(f.path.toLowerCase(), f);
        }
      }

      if (manifest && manifest.available && Array.isArray(manifest.files) && manifest.files.length > 0) {
        this.mode = "vfs";
        let opfsCount = 0;
        this.entries = manifest.files.map((item, idx) => {
          const relPath = item.path.replace(/\\/g, "/");
          const localHit = opfsByPath.get(relPath.toLowerCase());
          if (localHit) opfsCount++;
          const encodedRel = relPath.split("/").map(encodeURIComponent).join("/");
          const remoteUrl = manifest.cdn_url ? `${manifest.cdn_url}/${encodedRel}` : `${baseUrl}/game_assets/${encodedRel}`;
          return {
            id: idx,
            path: relPath,
            size: item.size,
            file: localHit ? localHit.file : null,
            handle: localHit ? localHit.handle : null,
            remoteUrl,
          };
        });
        this.source = opfsCount === this.entries.length ? "opfs" : "http+opfs";
        if (onLog) {
          onLog(
            `VFS streaming pronto (${this.entries.length} arquivos, ${opfsCount} no OPFS + HTTP Range 256 KB sob demanda).`
          );
        }
        return true;
      }

      if (opfsScan && opfsScan.available && opfsScan.files.length > 0) {
        this.mode = opfsScan.mode;
        this.source = "opfs";
        this.entries = opfsScan.files.map((f, idx) => ({
          id: idx,
          path: f.path,
          size: f.size,
          file: f.file,
          handle: f.handle,
          remoteUrl: null,
        }));
        if (onLog) {
          onLog(`VFS OPFS pronto em modo ${this.mode.toUpperCase()} (${this.entries.length} entrada(s)).`);
        }
        return true;
      }

      this.mode = "none";
      this.source = "none";
      this.entries = [];
      return false;
    },

    hasCatalog() {
      return this.mode !== "none" && this.entries.length > 0;
    },

    buildManifestTsv() {
      if (!this.hasCatalog()) return "";
      const lines = [];
      if (this.mode === "iso") {
        lines.push("#ISO");
        const f = this.entries[0];
        lines.push(`0\t${f.size}\t${f.path}`);
      } else {
        for (const e of this.entries) {
          lines.push(`${e.id}\t${e.size}\t${e.path}`);
        }
      }
      return lines.join("\n") + "\n";
    },

    writeManifestToMemfs(mod, dataDir = "/black_assets") {
      this.module = mod;
      const tsv = this.buildManifestTsv();
      if (!tsv) {
        throw new Error("Nenhum arquivo ou ISO carregado para gerar .vfs_manifest.tsv.");
      }
      const ensureDir = (dir) => {
        const parts = dir.split("/").filter(Boolean);
        let cur = "";
        for (const p of parts) {
          cur += "/" + p;
          try {
            mod.FS.mkdir(cur);
          } catch {}
        }
      };
      ensureDir("/tmp");
      ensureDir(dataDir);
      mod.FS.writeFile(`${dataDir}/.vfs_manifest.tsv`, tsv);
    },

    putChunk(key, data) {
      if (this.chunkCache.has(key)) this.chunkCache.delete(key);
      this.chunkCache.set(key, data);
      while (this.chunkCache.size > MAX_CACHED_CHUNKS) {
        const oldest = this.chunkCache.keys().next().value;
        this.chunkCache.delete(oldest);
      }
    },

    async fetchRemoteChunk(entry, chunkIdx) {
      const key = `${entry.id}:${chunkIdx}`;
      const cached = this.chunkCache.get(key);
      if (cached) {
        this.putChunk(key, cached);
        this.stats.cacheHits++;
        return cached;
      }
      const existing = this.inflight.get(key);
      if (existing) return existing;

      const start = chunkIdx * CHUNK_SIZE;
      const end = Math.min(entry.size - 1, start + CHUNK_SIZE - 1);
      const promise = (async () => {
        this.stats.rangeFetches++;
        // Redes lentas/túneis falham de vez em quando: tenta de novo antes de
        // devolver erro de leitura (o jogo mostra "disco danificado").
        let ab = null;
        let res = null;
        let lastErr = null;
        for (let attempt = 0; attempt < 5; attempt++) {
          try {
            const ctl = new AbortController();
            const timer = setTimeout(() => ctl.abort(), 20000);
            try {
              res = await fetch(entry.remoteUrl, {
                headers: { Range: `bytes=${start}-${end}` },
                signal: ctl.signal,
              });
              if (!res.ok && res.status !== 206) {
                throw new Error(`HTTP ${res.status} lendo ${entry.path} [${start}-${end}]`);
              }
              ab = await res.arrayBuffer();
            } finally {
              clearTimeout(timer);
            }
            break;
          } catch (err) {
            lastErr = err;
            ab = null;
            this.stats.retries = (this.stats.retries || 0) + 1;
            await new Promise((r) => setTimeout(r, 300 * 2 ** attempt));
          }
        }
        if (!ab) throw lastErr || new Error(`falha lendo ${entry.path}`);
        let bytes = new Uint8Array(ab);
        if (res.status === 200 && bytes.byteLength > end - start + 1) {
          bytes = bytes.subarray(start, end + 1);
        }
        this.putChunk(key, bytes);
        return bytes;
      })();

      this.inflight.set(key, promise);
      try {
        return await promise;
      } finally {
        this.inflight.delete(key);
      }
    },

    async readSlice(fileId, offset, outPtr, len) {
      const entry = this.entries[fileId];
      const heap = this.getHeapU8();
      if (!entry || !heap || len <= 0 || offset < 0 || offset >= entry.size) {
        return 0;
      }

      const want = Math.min(len, entry.size - offset);
      this.stats.reads++;
      this.stats.lastFile = entry.path;

      if (entry.file || entry.handle) {
        let f = entry.file || this.fileCache.get(fileId);
        if (!f && entry.handle) {
          f = await entry.handle.getFile();
          this.fileCache.set(fileId, f);
        }
        const ab = await f.slice(offset, offset + want).arrayBuffer();
        const bytes = new Uint8Array(ab);
        const dst = this.getHeapU8();
        dst.set(bytes, outPtr);
        this.stats.bytesServed += bytes.byteLength;
        return bytes.byteLength;
      }

      if (entry.remoteUrl) {
        let copied = 0;
        while (copied < want) {
          const curPos = offset + copied;
          const chunkIdx = Math.floor(curPos / CHUNK_SIZE);
          const chunk = await this.fetchRemoteChunk(entry, chunkIdx);
          const inChunkOff = curPos - chunkIdx * CHUNK_SIZE;
          if (inChunkOff >= chunk.byteLength) break;
          const take = Math.min(want - copied, chunk.byteLength - inChunkOff);
          this.getHeapU8().set(chunk.subarray(inChunkOff, inChunkOff + take), outPtr + copied);
          copied += take;
        }
        this.stats.bytesServed += copied;
        return copied;
      }

      return 0;
    },

    installGlobal() {
      window.__blackVfs = {
        readSlice: (fileId, offset, outPtr, len) => this.readSlice(fileId, offset, outPtr, len),
        getStats: () => ({ ...this.stats }),
      };
    },
  };

  return bridge;
}
