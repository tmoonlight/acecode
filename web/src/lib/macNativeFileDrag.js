const composerReceivers = new WeakMap();

export function registerNativeComposerFileDrop(element, receiver) {
  if (!element) return () => {};
  composerReceivers.set(element, receiver);
  return () => {
    if (composerReceivers.get(element) === receiver) composerReceivers.delete(element);
  };
}

export function fileDropDiagnostic(stage, fields = {}, win = globalThis.window) {
  // Callers supply only booleans, counts, ages, and outcomes. Never include paths,
  // input contents, key names, or exception payloads in diagnostics.
  const message = `[file-drop] ${stage} ${JSON.stringify(fields)}`;
  try {
    Promise.resolve(win?.aceDesktop_logFromWeb?.('info', message)).catch(() => {});
  } catch { /* Diagnostics must never affect input or drop behavior. */ }
}

export function nativeFileDropViewportPoint(location, viewport) {
  const xRatio = location?.xRatio;
  const yRatio = location?.yRatio;
  const width = Number(viewport?.width);
  const height = Number(viewport?.height);
  if (!Number.isFinite(xRatio) || !Number.isFinite(yRatio) ||
      !Number.isFinite(width) || !Number.isFinite(height) ||
      xRatio < 0 || xRatio >= 1 || yRatio < 0 || yRatio >= 1 ||
      width <= 0 || height <= 0) return null;
  return { x: xRatio * width, y: yRatio * height };
}

export function nativeFileDropTarget(element) {
  if (!element?.closest) return null;
  if (element.closest('[data-ace-native-overlay]')) return null;
  const composer = element.closest('.ace-composer-card');
  if (composer) {
    if (composer.dataset?.nativeFileDropDisabled === 'true') return null;
    return { kind: 'composer', element: composer };
  }
  const terminal = element.closest('.ace-console-term');
  const tabId = terminal?.dataset?.tabId;
  if (terminal && tabId && terminal.style?.display !== 'none') {
    return { kind: 'console', tabId };
  }
  return null;
}

export function routeNativeFileDrop(payload, win = globalThis.window, doc = globalThis.document) {
  const paths = Array.isArray(payload?.paths) ? payload.paths : [];
  const point = nativeFileDropViewportPoint(payload?.location, {
    width: win?.innerWidth,
    height: win?.innerHeight,
  });
  if (paths.length === 0 || !point || !doc?.elementFromPoint) {
    fileDropDiagnostic('drop-result', {
      count: paths.length, accepted: false, target: 'none',
      reason: paths.length === 0 ? 'empty' : (!point ? 'invalid-coordinate' : 'hit-test-unavailable'),
    }, win);
    return false;
  }

  const target = nativeFileDropTarget(doc.elementFromPoint(point.x, point.y));
  if (!target) {
    fileDropDiagnostic('drop-result', {
      count: paths.length, accepted: false, target: 'none', reason: 'ineligible-target',
    }, win);
    return false;
  }
  if (target?.kind === 'composer') {
    const receiver = composerReceivers.get(target.element);
    if (typeof receiver === 'function') {
      receiver({ paths, nativeLocation: true });
      return true;
    }
  }
  if (target?.kind === 'console' &&
      typeof win?.__aceConsoleAcceptFileDrop === 'function') {
    win.__aceConsoleAcceptFileDrop({ paths, nativeLocation: true, tabId: target.tabId });
    return true;
  }
  fileDropDiagnostic('drop-result', {
    count: paths.length, accepted: false, target: target.kind, reason: 'receiver-missing',
  }, win);
  return false;
}

export function installNativeFileDropRouter(win = globalThis.window, doc = globalThis.document) {
  if (!win) return () => {};
  let dragTarget = null;
  const leave = () => {
    if (dragTarget) dragTarget.dispatchEvent(new win.DragEvent('dragleave', { bubbles: true }));
    dragTarget = null;
  };
  const drag = (location) => {
    const point = nativeFileDropViewportPoint(location, { width: win.innerWidth, height: win.innerHeight });
    const hit = point && doc?.elementFromPoint?.(point.x, point.y);
    const scope = nativeFileDropTarget(hit);
    const target = scope?.kind === 'composer' ? scope.element
      : scope?.kind === 'console' ? hit.closest('.ace-console-term') : null;
    if (target !== dragTarget) leave();
    if (!target || typeof win.DataTransfer !== 'function' || typeof win.DragEvent !== 'function') return;
    const dataTransfer = new win.DataTransfer();
    // Hover only: the native drop carries the real paths separately. Never
    // dispatch a synthetic file drop or upload this placeholder File.
    dataTransfer.items.add(new win.File([], 'native-file'));
    const init = { bubbles: true, cancelable: true, dataTransfer, clientX: point.x, clientY: point.y };
    if (!dragTarget) target.dispatchEvent(new win.DragEvent('dragenter', init));
    dragTarget = target;
    target.dispatchEvent(new win.DragEvent('dragover', init));
  };
  const handler = (payload) => routeNativeFileDrop(payload, win, doc);
  win.__aceRouteNativeFileDrop = handler;
  win.__aceRouteNativeFileDrag = drag;
  return () => {
    leave();
    if (win.__aceRouteNativeFileDrag === drag) delete win.__aceRouteNativeFileDrag;
    if (win.__aceRouteNativeFileDrop !== handler) return;
    try { delete win.__aceRouteNativeFileDrop; }
    catch { win.__aceRouteNativeFileDrop = undefined; }
  };
}
