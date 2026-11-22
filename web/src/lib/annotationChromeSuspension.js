// The annotation chrome (element outline + hover style card) is painted inside
// the annotated page, which sits above the WebUI. Anything the app floats over
// that page — the discard confirmation dialog, a tab/page context menu — would
// otherwise be visually cut by a chrome card that sticks out on both sides.
//
// Owners register a named suspension source instead of toggling the page
// runtime directly, so two overlapping blockers cannot release each other too
// early: only the outer transition (none -> some, some -> none) is forwarded.
const sources = new Set();

export function annotationChromeSuspended() {
  return sources.size > 0;
}

export function setAnnotationChromeSuspended(source, enabled, emit) {
  const before = sources.size > 0;
  if (enabled) sources.add(source);
  else sources.delete(source);
  const after = sources.size > 0;
  if (before !== after) emit?.(after);
  return after;
}
