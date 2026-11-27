// Toolbar counts belong to the visible page. The pending card separately shows
// the session-wide total through the annotation outbox.
export function pageAnnotationPendingCount(snapshot = {}) {
  const explicit = Number(snapshot?.pending_count);
  if (Number.isInteger(explicit) && explicit >= 0) return explicit;
  return (Array.isArray(snapshot?.records) ? snapshot.records : [])
    .filter((record) => record?.status !== 'sent').length;
}
