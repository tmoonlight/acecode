// Moving keyboard focus into the annotation editor needs two steps in the
// Desktop shell: the main WebUI must first take focus back from the native
// browser surface, then the textarea can receive the caret. DOM focus alone
// leaves the browser page as the active native control, so typing is lost.
export function focusAnnotationDescription(input, {
  win = typeof window === 'undefined' ? null : window,
  documentRef = typeof document === 'undefined' ? null : document,
} = {}) {
  if (!input || typeof input.focus !== 'function') return () => {};

  let cancelled = false;
  let frame = 0;
  let timer = 0;

  const apply = () => {
    if (cancelled || input.isConnected === false) return;
    try {
      input.focus({ preventScroll: true });
    } catch {
      try { input.focus(); } catch { return; }
    }
    const end = String(input.value ?? '').length;
    try { input.setSelectionRange?.(end, end); } catch { /* non-text control */ }
  };

  const detach = () => {
    documentRef?.removeEventListener?.('pointerdown', onUserInput, true);
    documentRef?.removeEventListener?.('keydown', onUserInput, true);
    documentRef?.removeEventListener?.('input', onUserInput, true);
  };

  let cancel = () => {};
  const onUserInput = (event) => {
    if (event?.target !== input) cancel();
  };
  cancel = () => {
    if (cancelled) return;
    cancelled = true;
    if (frame) win?.cancelAnimationFrame?.(frame);
    if (timer) win?.clearTimeout?.(timer);
    frame = timer = 0;
    detach();
  };

  const scheduleRetryOrFinish = () => {
    if (cancelled) return;
    if (typeof win?.setTimeout === 'function') {
      timer = win.setTimeout(() => {
        timer = 0;
        if (cancelled) return;
        apply();
        detach();
      }, 80);
      return;
    }
    detach();
  };

  const paint = () => {
    frame = 0;
    if (cancelled) return;
    apply();
    scheduleRetryOrFinish();
  };

  const schedule = () => {
    if (cancelled) return;
    if (typeof win?.requestAnimationFrame === 'function') {
      frame = win.requestAnimationFrame(paint);
    } else {
      paint();
    }
  };

  documentRef?.addEventListener?.('pointerdown', onUserInput, true);
  documentRef?.addEventListener?.('keydown', onUserInput, true);
  documentRef?.addEventListener?.('input', onUserInput, true);

  const focusWebview = win?.aceDesktop_focusActiveWebview;
  if (typeof focusWebview !== 'function') {
    schedule();
    return cancel;
  }
  try {
    const result = focusWebview();
    if (result && typeof result.then === 'function') {
      result.then(schedule, schedule);
    } else {
      schedule();
    }
  } catch {
    schedule();
  }
  return cancel;
}
