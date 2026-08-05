function parseBridgeResult(value) {
  if (value == null) return {};
  if (typeof value === 'string') {
    try { return JSON.parse(value); } catch { return { ok: false, error: value }; }
  }
  if (typeof value === 'object') return value;
  return { ok: false, error: String(value) };
}

export async function openExternalUrl(url, win = typeof window !== 'undefined' ? window : undefined) {
  const target = String(url || '').trim();
  if (!target) return { ok: false, error: 'url required' };

  const bridge = win && typeof win.aceDesktop_openExternalUrl === 'function'
    ? win.aceDesktop_openExternalUrl
    : null;
  if (bridge) {
    try {
      const result = parseBridgeResult(await bridge(target));
      return result?.ok ? { ok: true, via: 'desktop' } : {
        ok: false,
        via: 'desktop',
        error: result?.error || 'failed to open system browser',
      };
    } catch (error) {
      return {
        ok: false,
        via: 'desktop',
        error: error?.message || String(error),
      };
    }
  }

  if (win && typeof win.open === 'function') {
    win.open(target, '_blank', 'noopener,noreferrer');
    return { ok: true, via: 'window-open' };
  }
  return { ok: false, error: 'no external URL opener available' };
}

export function externalHttpUrlFromNewPageEvent(event) {
  if (!event || event.defaultPrevented) return '';
  if (event.type === 'auxclick') {
    if (event.button !== 1) return '';
  } else if (event.button != null && event.button !== 0) {
    return '';
  }

  const anchor = event.target?.closest?.('a[target="_blank"]');
  if (!anchor) return '';
  if (typeof anchor.hasAttribute === 'function' && anchor.hasAttribute('download')) return '';

  const href = typeof anchor.href === 'string'
    ? anchor.href
    : anchor.getAttribute?.('href');
  const target = String(href || '').trim();
  return /^https?:\/\//i.test(target) ? target : '';
}

function externalLinkFailureMessage(error) {
  if (typeof error === 'string' && error) return error;
  if (error && typeof error.message === 'string' && error.message) return error.message;
  return 'failed to open system browser';
}

export function installDesktopExternalLinkRouter({
  win = typeof window !== 'undefined' ? window : undefined,
  doc = typeof document !== 'undefined' ? document : undefined,
  opener = openExternalUrl,
  onError,
} = {}) {
  if (!win || typeof win.aceDesktop_openExternalUrl !== 'function') return () => {};
  if (!doc || typeof doc.addEventListener !== 'function') return () => {};

  const reportFailure = (error, url) => {
    if (typeof onError !== 'function') return;
    try {
      onError(externalLinkFailureMessage(error), url);
    } catch {
      // Error presentation must not break the delegated document listener.
    }
  };

  const handleNewPage = (event) => {
    const url = externalHttpUrlFromNewPageEvent(event);
    if (!url) return;

    // Suppress WebView popup creation before invoking the bridge. A native
    // launcher failure stays external-only and is reported through onError.
    event.preventDefault?.();
    event.stopPropagation?.();

    let result;
    try {
      result = opener(url, win);
    } catch (error) {
      reportFailure(error, url);
      return;
    }
    Promise.resolve(result).then(
      (value) => {
        if (!value?.ok) reportFailure(value?.error, url);
      },
      (error) => reportFailure(error, url),
    );
  };

  doc.addEventListener('click', handleNewPage, true);
  doc.addEventListener('auxclick', handleNewPage, true);

  let installed = true;
  return () => {
    if (!installed) return;
    installed = false;
    doc.removeEventListener?.('click', handleNewPage, true);
    doc.removeEventListener?.('auxclick', handleNewPage, true);
  };
}
