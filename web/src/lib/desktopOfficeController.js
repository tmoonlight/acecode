import { officeSessionRef, projectDesktopOffice } from './desktopOfficeState.js';

// One App-owned controller; every asynchronous response belongs to a selection
// generation. Inject IO, timers and publishing so lifecycle races are testable.
export function createDesktopOfficeController({
  fetchSnapshot, fetchModel, publish, retainSession = () => {}, releaseSession = () => {},
  schedule = setTimeout, cancel = clearTimeout, initial = {},
} = {}) {
  let follow = initial.follow !== false, active = {}, selected = initial.selected || {};
  let generation = 0, disposed = false, timer = null, timerDelay = Infinity, request = null;
  let snapshot = {}, connected = true, queued = false;
  const retained = new Set();
  const emit = () => !disposed && publish(projectDesktopOffice(snapshot, {follow, connected}));
  const syncRetained = () => {
    const wanted = new Set();
    for (const session of [...(snapshot.offices || []), ...(snapshot.agents || [])]) {
      if (session.active && (session.id === snapshot.selected?.id || session.busy || session.status === 'running')) wanted.add(session.id);
    }
    for (const id of retained) if (!wanted.has(id)) {releaseSession(id); retained.delete(id);}
    for (const id of wanted) if (!retained.has(id)) {retainSession(id); retained.add(id);}
  };
  function later(delay) {
    if (disposed) return;
    if (timer !== null && timerDelay <= delay) return;
    if (timer !== null) cancel(timer);
    timerDelay = delay;
    timer = schedule(() => {timer = null; timerDelay = Infinity; void refresh();}, delay);
  }
  async function refresh() {
    if (disposed) return;
    if (request) {queued = true; return;}
    const version = generation, controller = new AbortController();
    request = controller;
    try {
      const wanted = follow && active.sessionId ? active : selected;
      const next = await fetchSnapshot(wanted, {signal: controller.signal});
      if (disposed || version !== generation || controller.signal.aborted) return;
      // Dormant sessions need their model window; never use lifetime token totals.
      if (next.selected && !next.selected.context_window && fetchModel) {
        try {
          const model = await fetchModel(next.selected.id, next.selected.workspace_hash || '');
          if (disposed || version !== generation || controller.signal.aborted) return;
          const limit = Number(model?.context_window ?? model?.contextWindow) || 0;
          next.selected.context_window = limit;
          for (const agent of next.agents || []) if (agent.id === next.selected.id) agent.context_window = limit;
        } catch { /* Unknown context remains unknown until a later snapshot. */ }
      }
      snapshot = next; connected = true;
      if (next.selected) selected = officeSessionRef(next.selected);
      syncRetained(); emit();
    } catch (error) {
      if (disposed || version !== generation || controller.signal.aborted) return;
      if (error?.status === 404) {
        selected = {}; snapshot = {...snapshot, selected:null, agents:[], offices:error.body?.offices || snapshot.offices || []};
        if (follow) active = {};
        syncRetained();
      } else connected = false;
      emit();
    } finally {
      if (request === controller) request = null;
      if (!disposed && version === generation) {
        const delay = queued ? 100 : snapshot.complete === false ? 500 : 3000;
        queued = false; later(delay);
      }
    }
  }
  function change() {
    generation++;
    request?.abort(); request = null; queued = false;
    snapshot = {...snapshot, selected:null, agents:[]};
    syncRetained(); emit(); later(0);
  }
  return {
    start() {later(0);},
    refresh,
    setActive(value) {
      const next = officeSessionRef(value);
      if (next.sessionId === active.sessionId && next.workspaceHash === active.workspaceHash) return;
      active = next;
      if (follow && next.sessionId) change();
    },
    select(value) {selected = officeSessionRef(value); follow = false; change();},
    setFollow(value) {follow = !!value; change();},
    onEvent(message) {
      if (['token','reasoning','tool_update'].includes(message.type)) return;
      if (request) queued = true;
      else later(100);
    },
    disconnected() {connected = false; emit(); later(500);},
    reconnected() {
      generation++; request?.abort(); request = null; queued = false;
      // Preserve retained subscriptions while recovering: releasing them from
      // an onopen callback would close/reopen an office-only connection forever.
      later(0);
    },
    get state() {return projectDesktopOffice(snapshot, {follow, connected});},
    dispose() {
      disposed = true; generation++; request?.abort(); request = null;
      if (timer !== null) cancel(timer);
      for (const id of retained) releaseSession(id);
      retained.clear();
    },
  };
}
