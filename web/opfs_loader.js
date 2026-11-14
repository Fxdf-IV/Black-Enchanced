// OPFS (Origin Private File System) Loader & XBE Verifier — Black Enhanced Online
// Lê a pasta/arquivos locais do jogador via File System Access API, valida o cabeçalho
// do default.xbe (magic "XBEH" = 0x48454258 e Title ID 0x45410083 de BLACK NTSC-U)
// e copia os arquivos para o armazenamento privado do navegador sem enviar nada pela rede.

export const BLACK_TITLE_ID = 0x45410083;
export const XBE_MAGIC = 0x48454258; // "XBEH" little-endian

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

  return {
    valid: titleId === BLACK_TITLE_ID,
    magic,
    baseAddr,
    certAddr,
    titleId,
    byteLength: arrayBuffer.byteLength,
    reason:
      titleId === BLACK_TITLE_ID
        ? "default.xbe NTSC-U de BLACK (Title ID 0x45410083) verificado."
        : `Title ID 0x${titleId.toString(16).toUpperCase()} não corresponde a BLACK (0x45410083).`,
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

export async function stageFilesToOpfs(fileList, onProgress) {
  let root = null;
  if (navigator.storage && navigator.storage.getDirectory) {
    root = await navigator.storage.getDirectory();
  }

  let xbeReport = null;
  let totalBytes = 0;
  let count = 0;

  for (const file of fileList) {
    const relPath = (file.webkitRelativePath || file.name).replace(/\\/g, "/");
    const baseName = relPath.split("/").pop().toLowerCase();

    if (baseName === "default.xbe") {
      const buf = await file.arrayBuffer();
      const info = inspectXbeHeader(buf);
      const hash = await sha256Hex(buf);
      xbeReport = { ...info, sha256: hash, path: relPath };
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
      return { cached: true, info, sizeBytes: file.size };
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
      const hash = await sha256Hex(buf);
      xbeReport = { ...info, sha256: hash, path: relPath };
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
