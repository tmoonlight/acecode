function containsElement(root, target) {
  if (!root || !target) return false;
  if (root === target) return true;
  if (typeof root.contains !== 'function') return false;
  try {
    return root.contains(target);
  } catch {
    return false;
  }
}

export function isComposerEditorFocused(rootElement) {
  const activeElement = rootElement?.ownerDocument?.activeElement;
  return activeElement?.getAttribute?.('data-ace-rich-composer') === 'true'
    && containsElement(rootElement, activeElement);
}

export function preserveComposerFocusOnPointerDown(event, rootElement) {
  if (event.button !== 0 || event.isPrimary === false || event.defaultPrevented || !isComposerEditorFocused(rootElement)) {
    return false;
  }

  const editorElement = rootElement.ownerDocument.activeElement;
  if (containsElement(editorElement, event.target)) return false;
  const target = event.target?.nodeType === 3 ? event.target.parentElement : event.target;
  if (target?.closest?.('input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"]')) {
    return false;
  }

  // Pointer capture also reaches disabled buttons, which suppress mousedown.
  // Keep Slate's selection and the click action, including React portal menus.
  event.preventDefault();
  return true;
}

function clampOffset(value, max) {
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  return Math.max(0, Math.min(max, number));
}

const DESKTOP_WINDOW_FOCUS_EVENT = 'acecode:desktop-window-focus';

function readTextareaSelection(textareaElement) {
  if (!textareaElement) return null;
  const max = String(textareaElement.value ?? '').length;
  const start = clampOffset(textareaElement.selectionStart, max);
  const end = clampOffset(textareaElement.selectionEnd, max);
  if (start == null || end == null) return null;
  return {
    start,
    end,
    direction: textareaElement.selectionDirection || 'none',
  };
}

export function shouldRestoreComposerTextareaFocus({
  activeElement,
  rootElement,
  textareaElement,
  bodyElement,
  documentElement,
} = {}) {
  if (!textareaElement) return false;
  if (!activeElement) return true;
  if (activeElement === textareaElement) return true;
  if (containsElement(rootElement, activeElement)) return true;
  return activeElement === bodyElement || activeElement === documentElement;
}

export function restoreComposerTextareaCaret({
  textareaElement,
  rootElement,
  selection,
  documentRef = typeof document === 'undefined' ? null : document,
  allowExternalFocus = false,
} = {}) {
  if (!textareaElement) return false;
  const activeElement = documentRef?.activeElement || null;
  if (!allowExternalFocus && !shouldRestoreComposerTextareaFocus({
    activeElement,
    rootElement,
    textareaElement,
    bodyElement: documentRef?.body,
    documentElement: documentRef?.documentElement,
  })) {
    return false;
  }

  const liveSelection = activeElement === textareaElement ? readTextareaSelection(textareaElement) : null;
  const fallbackSelection = readTextareaSelection(textareaElement);
  const targetSelection = liveSelection || selection || fallbackSelection;

  try {
    textareaElement.focus({ preventScroll: true });
  } catch {
    try {
      textareaElement.focus();
    } catch {
      return false;
    }
  }

  if (targetSelection && typeof textareaElement.setSelectionRange === 'function') {
    try {
      const max = String(textareaElement.value ?? '').length;
      const start = clampOffset(targetSelection.start, max);
      const end = clampOffset(targetSelection.end, max);
      if (start != null && end != null) {
        textareaElement.setSelectionRange(start, end, targetSelection.direction || 'none');
      }
    } catch {
      // Some old WebViews can reject selection updates during teardown; focus is still useful.
    }
  }

  return true;
}

export function captureComposerTextareaSelection(textareaElement) {
  return readTextareaSelection(textareaElement);
}

export function shouldAutoFocusDesktopComposer({
  desktopMode,
  chatVisible = false,
  blockingSurfaceOpen = false,
} = {}) {
  return desktopMode === 'shell' && !!chatVisible && !blockingSurfaceOpen;
}

export function bindDesktopComposerAutoFocus({
  enabled = false,
  onFocus,
  win = typeof window === 'undefined' ? null : window,
  documentRef = typeof document === 'undefined' ? null : document,
} = {}) {
  if (!enabled || typeof onFocus !== 'function' || !win || !documentRef) return () => {};

  let disposed = false;
  let generation = 0;
  let pending = false;
  let nativeActivationPending = false;
  let composing = false;
  let frame = 0;
  let timer = 0;
  const cancel = () => {
    generation += 1;
    pending = false;
    nativeActivationPending = false;
    win.cancelAnimationFrame(frame);
    win.clearTimeout(timer);
    frame = timer = 0;
  };
  const available = () => (
    !disposed && !composing && documentRef.visibilityState !== 'hidden'
    && !documentRef.querySelector?.('[role="dialog"], .ace-global-find')
  );
  const focusIfCurrentWindow = (event) => {
    if (pending) return;
    const native = event?.type === DESKTOP_WINDOW_FOCUS_EVENT || nativeActivationPending;
    if (!available()) {
      // Restore can notify native activation before WebView reports visible.
      // Keep that intent until visibility catches up; the OS bridge still
      // rejects it if the user has already switched to another application.
      if (native && documentRef.visibilityState === 'hidden') nativeActivationPending = true;
      return;
    }
    nativeActivationPending = false;
    // Native activation may precede DOM focus. The bridge checks the OS window
    // instead; visibility/pageshow alone must never activate a background page.
    if (!native && documentRef.hasFocus?.() === false) return;
    pending = true;
    const request = ++generation;
    const current = () => request === generation && available();
    const canFocus = () => current() && documentRef.hasFocus?.() !== false;
    const restore = (result) => {
      if (request !== generation || disposed) return;
      if (!available()) { cancel(); return; }
      if (result?.ok === false) { cancel(); return; }
      frame = win.requestAnimationFrame(() => {
        frame = 0;
        if (!canFocus()) { cancel(); return; }
        onFocus(canFocus);
        // One bounded retry covers native restore/layout and Slate commit order.
        // Recheck every time; clicks, typing, blur and cleanup cancel this work.
        timer = win.setTimeout(() => {
          timer = 0;
          if (canFocus()) onFocus(canFocus);
          pending = false;
        }, 80);
      });
    };
    frame = win.requestAnimationFrame(() => {
      frame = 0;
      if (!current()) { cancel(); return; }
      // A focused document already receives keyboard input. Repeating MoveFocus
      // can terminate a newly started IME composition while its promise is pending.
      if (documentRef.hasFocus?.() === true || typeof win.aceDesktop_focusActiveWebview !== 'function') {
        restore();
        return;
      }
      try {
        Promise.resolve(win.aceDesktop_focusActiveWebview()).then(restore, () => {
          if (current()) cancel();
        });
      } catch {
        cancel();
      }
    });
  };
  const onVisibilityChange = () => {
    if (documentRef.visibilityState === 'visible') focusIfCurrentWindow();
    else onCompositionEnd();
  };
  const onCompositionStart = () => {
    composing = true;
    cancel();
  };
  const onCompositionEnd = () => {
    composing = false;
    cancel();
  };

  // A native blur can end composition without delivering compositionend to a
  // disappearing editor. Do not leave the next window activation blocked.
  win.addEventListener('blur', onCompositionEnd);
  documentRef.addEventListener('pointerdown', cancel, true);
  documentRef.addEventListener('keydown', cancel, true);
  // IMEs and native text services can edit without a keydown. MoveFocus during
  // composition commits the candidate in WebView2, so cancel before the bridge.
  documentRef.addEventListener('beforeinput', cancel, true);
  documentRef.addEventListener('input', cancel, true);
  documentRef.addEventListener('compositionstart', onCompositionStart, true);
  documentRef.addEventListener('compositionend', onCompositionEnd, true);
  win.addEventListener('focus', focusIfCurrentWindow);
  win.addEventListener('pageshow', focusIfCurrentWindow);
  win.addEventListener(DESKTOP_WINDOW_FOCUS_EVENT, focusIfCurrentWindow);
  documentRef.addEventListener('visibilitychange', onVisibilityChange);
  return () => {
    disposed = true;
    cancel();
    win.removeEventListener('blur', onCompositionEnd);
    documentRef.removeEventListener('pointerdown', cancel, true);
    documentRef.removeEventListener('keydown', cancel, true);
    documentRef.removeEventListener('beforeinput', cancel, true);
    documentRef.removeEventListener('input', cancel, true);
    documentRef.removeEventListener('compositionstart', onCompositionStart, true);
    documentRef.removeEventListener('compositionend', onCompositionEnd, true);
    win.removeEventListener('focus', focusIfCurrentWindow);
    win.removeEventListener('pageshow', focusIfCurrentWindow);
    win.removeEventListener(DESKTOP_WINDOW_FOCUS_EVENT, focusIfCurrentWindow);
    documentRef.removeEventListener('visibilitychange', onVisibilityChange);
  };
}

export function requestDesktopWindowFocus(win = typeof window === 'undefined' ? null : window) {
  const focusWindow = win?.aceDesktop_focusWindow;
  if (typeof focusWindow !== 'function') return false;
  try {
    Promise.resolve(focusWindow()).catch(() => {});
    return true;
  } catch {
    return false;
  }
}

export function requestDesktopFileDragActivation(win = typeof window === 'undefined' ? null : window) {
  const activateWindow = win?.aceDesktop_activateFileDropWindow;
  if (typeof activateWindow !== 'function') return false;
  try {
    Promise.resolve(activateWindow()).catch(() => {});
    return true;
  } catch {
    return false;
  }
}

export function requestDesktopFileDropFocus(win = typeof window === 'undefined' ? null : window) {
  // Older desktop shells still get a best-effort retry at acceptance.
  const focusWindow = win?.aceDesktop_focusFileDropWindow || win?.aceDesktop_activateFileDropWindow;
  if (typeof focusWindow !== 'function') return false;
  try {
    Promise.resolve(focusWindow()).catch(() => {});
    return true;
  } catch {
    return false;
  }
}
