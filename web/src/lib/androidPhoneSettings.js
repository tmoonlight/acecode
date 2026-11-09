// Keep native setting requests ordered across section switches and resume events.
export function createPhoneSettingsStore(request) {
  let snapshot = { status: null, pending: 0, error: '', failed: null, draftPet: null };
  let queue = Promise.resolve();
  let petRevision = 0;
  const listeners = new Set();
  const publish = (patch) => {
    snapshot = { ...snapshot, ...patch };
    for (const listener of listeners) listener();
  };
  const run = (action, fields = {}) => {
    const revision = action === 'pet' ? ++petRevision : petRevision;
    publish({ pending: snapshot.pending + 1, ...(action === 'pet' ? { draftPet: fields.kind } : {}) });
    const operation = queue.then(async () => {
      try {
        const status = await request(action, fields);
        // A background refresh must not erase a failed write or its retry target.
        const clearFailure = action !== 'status' || snapshot.failed?.action === 'status';
        const failure = clearFailure ? null : snapshot.failed;
        const patch = { status, ...(clearFailure ? { error: '', failed: null } : {}) };
        if (revision === petRevision && !failure) {
          if (action === 'pet') patch.draftPet = fields.kind !== 'off' && !status.overlay ? fields.kind : null;
          else if (status.overlay && status.pet === snapshot.draftPet) patch.draftPet = null;
        }
        publish(patch);
        return true;
      } catch (error) {
        if (action !== 'status' || !snapshot.failed || snapshot.failed.action === 'status') {
          publish({ error: error?.message || '无法更新手机设置', failed: { action, fields } });
        }
        return false;
      } finally {
        publish({ pending: snapshot.pending - 1 });
      }
    });
    queue = operation;
    return operation;
  };
  return {
    getSnapshot: () => snapshot,
    subscribe: (listener) => { listeners.add(listener); return () => listeners.delete(listener); },
    refresh: () => run('status'),
    setPet: (kind) => run('pet', { kind }),
    accessibility: () => run('accessibility'),
    retry: () => snapshot.failed ? run(snapshot.failed.action, snapshot.failed.fields) : run('status'),
  };
}
