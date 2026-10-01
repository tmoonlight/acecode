// Ephemeral measurements only; no message content or credentials are retained.
const pending = new Map();
const now = () => globalThis.performance?.now?.() ?? Date.now();

export function beginSessionOpen(sessionId, startedAt = now()) {
  const id = String(sessionId || '');
  if (!id) return null;
  const measurement = { sessionId: id, startedAt, historyRequests: 0, historyBytes: 0 };
  pending.set(id, measurement);
  // Abandoned navigation must not leave an unbounded collection.
  while (pending.size > 32) pending.delete(pending.keys().next().value);
  return measurement;
}

export function ensureSessionOpen(sessionId) {
  return pending.get(String(sessionId || '')) || beginSessionOpen(sessionId);
}

export function trackSessionHistoryRequest(path) {
  const match = /^\/api\/sessions\/([^/?]+)\/messages\?(.*)$/.exec(path);
  if (!match) return null;
  const params = new URLSearchParams(match[2]);
  if (Number(params.get('since') || 0) !== 0) return null;
  const measurement = pending.get(decodeURIComponent(match[1]));
  if (measurement) measurement.historyRequests += 1;
  return measurement || null;
}

export function recordSessionHistoryBytes(measurement, bytes) {
  if (measurement && pending.get(measurement.sessionId) === measurement) {
    measurement.historyBytes += Math.max(0, Number(bytes) || 0);
  }
}

export function sessionOpenPayload(measurement, renderedAt = now()) {
  return {
    session_id: measurement.sessionId,
    elapsed_ms: Math.max(0, renderedAt - measurement.startedAt),
    history_requests: measurement.historyRequests,
    history_bytes: measurement.historyBytes,
  };
}

export function reportSessionOpen(sessionId, send, renderedAt = now()) {
  const measurement = pending.get(String(sessionId || ''));
  if (!measurement) return Promise.resolve();
  pending.delete(measurement.sessionId);
  const payload = sessionOpenPayload(measurement, renderedAt);
  return Promise.resolve().then(() => send(payload)).catch(() => {});
}
