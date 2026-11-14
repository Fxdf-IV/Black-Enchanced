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
