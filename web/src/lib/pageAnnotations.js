import { parseAgentBrowserBridgeResult } from './agentBrowser.js';

export async function runPageAnnotationCommand(pageId, command, win = globalThis.window) {
  if (typeof win?.aceDesktop_agentBrowserAnnotationCommand !== 'function') {
    return { ok: false, error: '当前桌面版本不支持网页批注' };
  }
  return parseAgentBrowserBridgeResult(
    await win.aceDesktop_agentBrowserAnnotationCommand({ page_id: pageId, command }),
  );
}

export function createPageAnnotationController({ pageId, invoke = runPageAnnotationCommand, onChange }) {
  let snapshot = { document_id: '', active: false, popup: false, pending_count: 0, revision: -1 };
  let tail = Promise.resolve();
  let disposed = false;
  const observe = (next) => {
    if (disposed || !next || !Number.isSafeInteger(next.revision)
        || next.revision <= snapshot.revision) return;
    snapshot = next;
    onChange?.(snapshot);
  };
  return {
    observe,
    getSnapshot: () => snapshot,
    command(action, fields = {}) {
      const command = {
        action, document_id: snapshot.document_id,
        ...(action === 'save' ? { target_id: snapshot.target?.id } : {}),
        ...fields,
      };
      const run = async () => {
        if (disposed) return { ok: false, cancelled: true };
        try {
          const result = await invoke(pageId, command);
          if (result?.ok === true && result.page_id === pageId) observe(result.annotations);
          return result;
        } catch (error) {
          return { ok: false, error: String(error?.message || error) };
        }
      };
      const result = tail.then(run);
      tail = result.then(() => undefined);
      return result;
    },
    dispose() { disposed = true; },
  };
}

export function pageAnnotationPopupPosition({ width, height, popupWidth, popupHeight, target, viewport }) {
  const scaleX = viewport?.width > 0 ? width / viewport.width : 1;
  const scaleY = viewport?.height > 0 ? height / viewport.height : 1;
  const left = (target?.bounds?.x || 0) * scaleX;
  const top = (target?.bounds?.y || 0) * scaleY;
  const bottom = top + (target?.bounds?.height || 0) * scaleY;
  const clamp = (value, available) => Math.max(8, Math.min(value, available - 8));
  return {
    left: clamp(left, width - popupWidth),
    top: clamp(bottom + 8 + popupHeight <= height ? bottom + 8 : top - popupHeight - 8, height - popupHeight),
  };
}
