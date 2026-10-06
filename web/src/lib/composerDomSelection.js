import { Range } from 'slate';

function containsNode(root, node) {
  if (!root || !node) return false;
  if (root === node) return true;
  try {
    return typeof root.contains === 'function' && root.contains(node);
  } catch {
    return false;
  }
}

// Slate copies the browser caret into editor.selection only from its throttled
// selectionchange listener, and skips that listener while it believes a drag,
// a selection write or a composition is in progress. If one of those flags is
// left behind, Slate keeps an old selection while the user sees the caret
// elsewhere. Chrome compositions and Backspace/Delete then apply to the old
// Slate selection, so IME text lands at the previous position (often the end).
//
// Returns the Slate range of the live DOM selection when it must replace
// editor.selection before the editor consumes input, otherwise null.
export function composerDomSelectionToAdopt({
  editor,
  editableElement,
  toSlateRange,
  renderPending = false,
} = {}) {
  if (!editor || !editableElement || typeof toSlateRange !== 'function') return null;
  // Unrendered operations mean the DOM still shows the previous document or
  // selection. Slate's own sync also waits; mapping now would undo the change.
  if (renderPending || (editor.operations?.length || 0) > 0) return null;
  const documentRef = editableElement.ownerDocument;
  if (!documentRef || documentRef.activeElement !== editableElement) return null;
  const domSelection = typeof documentRef.getSelection === 'function'
    ? documentRef.getSelection()
    : documentRef.defaultView?.getSelection?.();
  if (!domSelection || !(domSelection.rangeCount > 0)) return null;
  if (!containsNode(editableElement, domSelection.anchorNode)
    || !containsNode(editableElement, domSelection.focusNode)) {
    return null;
  }
  let range = null;
  try {
    range = toSlateRange(domSelection);
  } catch {
    return null;
  }
  if (!range?.anchor || !range?.focus) return null;
  if (editor.selection && Range.equals(range, editor.selection)) return null;
  return range;
}
