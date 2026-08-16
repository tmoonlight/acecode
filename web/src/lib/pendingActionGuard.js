// Synchronous guard for async UI actions. React state can lag behind a second
// click in the same render batch, so correctness lives in this mutable set;
// component state remains presentation-only.
export function createPendingActionGuard() {
  const pendingKeys = new Set();

  const normalizedKey = (key) => {
    if (key == null) return '';
    return String(key);
  };

  return {
    acquire(key) {
      const normalized = normalizedKey(key);
      if (!normalized || pendingKeys.has(normalized)) return false;
      pendingKeys.add(normalized);
      return true;
    },

    release(key) {
      return pendingKeys.delete(normalizedKey(key));
    },

    isPending(key) {
      return pendingKeys.has(normalizedKey(key));
    },

    get size() {
      return pendingKeys.size;
    },
  };
}
