export function revokeObjectUrl(urlApi, url) {
  if (!url) return;
  try {
    urlApi?.revokeObjectURL?.(url);
  } catch {
    // Object URL cleanup is best effort during document teardown.
  }
}

export function downloadBlob(blob, filename, {
  document: doc = globalThis.document,
  URLApi = globalThis.URL,
  schedule = globalThis.setTimeout?.bind(globalThis),
} = {}) {
  if (!blob || !filename || typeof doc?.createElement !== 'function'
      || typeof URLApi?.createObjectURL !== 'function') {
    return { ok: false, error: 'download unavailable' };
  }

  let objectUrl = '';
  let anchor = null;
  try {
    objectUrl = URLApi.createObjectURL(blob);
    if (!objectUrl) return { ok: false, error: 'download URL unavailable' };
    anchor = doc.createElement('a');
    anchor.href = objectUrl;
    anchor.download = filename;
    anchor.style.display = 'none';
    (doc.body || doc.documentElement)?.append?.(anchor);
    anchor.click();
    anchor.remove?.();
    anchor = null;
    const cleanup = () => revokeObjectUrl(URLApi, objectUrl);
    if (typeof schedule === 'function') schedule(cleanup, 0);
    else cleanup();
    return { ok: true, filename };
  } catch (error) {
    try { anchor?.remove?.(); } catch { /* best effort */ }
    revokeObjectUrl(URLApi, objectUrl);
    return { ok: false, error: String(error?.message || error) };
  }
}
