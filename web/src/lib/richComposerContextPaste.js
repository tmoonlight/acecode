export const RICH_COMPOSER_CONTEXT_PASTE_EVENT = 'acecode:rich-composer-context-paste';
export const RICH_COMPOSER_ROOT_ATTRIBUTE = 'data-ace-rich-composer';

export const RICH_COMPOSER_CONTEXT_PASTE_ACTIONS = Object.freeze({
  CAPTURE_SELECTION: 'capture-selection',
  INSERT_TEXT: 'insert-text',
});

export function normalizeRichComposerContextSelection(value) {
  const start = Number(value?.start);
  const end = Number(value?.end);
  if (!Number.isFinite(start) || !Number.isFinite(end)) return null;
  const safeStart = Math.max(0, Math.trunc(start));
  const safeEnd = Math.max(safeStart, Math.trunc(end));
  const direction = value?.direction === 'backward' || value?.direction === 'forward'
    ? value.direction
    : 'none';
  return { start: safeStart, end: safeEnd, direction };
}

export function richComposerRootFromTarget(target) {
  let element = target && typeof target === 'object' ? target : null;
  while (element) {
    if (typeof element.getAttribute === 'function'
      && element.getAttribute(RICH_COMPOSER_ROOT_ATTRIBUTE) === 'true') {
      return element;
    }
    element = element.parentElement || null;
  }
  return null;
}

function defaultCreateEvent(type, init) {
  if (typeof CustomEvent !== 'function') return null;
  return new CustomEvent(type, init);
}

function dispatchRichComposerContextPasteAction(target, detail, { createEvent } = {}) {
  if (!target || typeof target.dispatchEvent !== 'function') return false;
  const event = (createEvent || defaultCreateEvent)(
    RICH_COMPOSER_CONTEXT_PASTE_EVENT,
    { detail, bubbles: true },
  );
  if (!event) return false;
  target.dispatchEvent(event);
  return detail.handled === true;
}

export function captureRichComposerContextSelection(target, options) {
  const detail = {
    action: RICH_COMPOSER_CONTEXT_PASTE_ACTIONS.CAPTURE_SELECTION,
    handled: false,
    selection: null,
  };
  if (!dispatchRichComposerContextPasteAction(target, detail, options)) return null;
  return normalizeRichComposerContextSelection(detail.selection);
}

export function insertRichComposerContextText(target, text, selection, options) {
  const detail = {
    action: RICH_COMPOSER_CONTEXT_PASTE_ACTIONS.INSERT_TEXT,
    handled: false,
    text: String(text ?? ''),
    selection: normalizeRichComposerContextSelection(selection),
  };
  return dispatchRichComposerContextPasteAction(target, detail, options);
}
