import { normalizeComposerContent } from './composerContent.js';

export function hasFollowUpDraft({ text = '', content = null, attachments = [], contexts = [] } = {}) {
  const parts = normalizeComposerContent(content)?.parts
    ?? (Array.isArray(content?.parts) ? content.parts : []);
  return !!(String(text || '').length || parts.length || attachments.length || contexts.length);
}

// This identity is persisted in turn_timing, so history reload and live events
// name the same completion. Renderer IDs and transient idle states never do.
export function followUpCompletionKey(transcript = {}) {
  if (transcript.loadState !== 'loaded' || transcript.busy || transcript.abortPending
      || transcript.status === 'error') return '';
  const items = Array.isArray(transcript.items) ? transcript.items : [];
  let user;
  for (let index = items.length - 1; index >= 0; index--) {
    if (items[index]?.kind === 'msg' && items[index].role === 'user') { user = items[index]; break; }
  }
  const userId = String(user?.messageId || user?.metadata?.user_message_uuid || '');
  if (!userId) return '';
  const timing = transcript.turnTimings instanceof Map
    ? transcript.turnTimings.get(userId) : transcript.turnTimings?.[userId];
  if (timing?.status !== 'completed' || !Number.isSafeInteger(timing.completedAtMs)
      || timing.completedAtMs <= 0) return '';
  return `${userId}:${timing.completedAtMs}`;
}

export function followUpScope(ref = {}) {
  ref = ref || {};
  return JSON.stringify([ref.port || '', ref.workspaceHash || ref.workspace_hash || '',
    ref.noWorkspace === true || ref.no_workspace === true, ref.sessionId || ref.id || '']);
}

export function validFollowUpPrompt(value) {
  if (typeof value !== 'string') return '';
  const prompt = value.trim();
  return Array.from(prompt).length <= 160 && !/[\u0000-\u001f\u007f]/.test(prompt) ? prompt : '';
}

const EMPTY = Object.freeze({ status: 'idle', prompt: '', consumed: false });

// Shared across ChatView mounts for this application lifetime. Results remain
// separate from drafts and are never written to browser or session storage.
export function createFollowUpPromptStore() {
  const records = new Map();
  const listeners = new Map();
  const identity = (scope, completion) => JSON.stringify([scope, completion]);
  const publish = (key, value) => {
    records.set(key, value);
    for (const listener of listeners.get(key) || []) listener();
  };
  const consume = (scope, completion) => {
    if (!completion) return;
    const key = identity(scope, completion);
    if (records.get(key)?.consumed) return;
    publish(key, { status: 'ready', prompt: '', consumed: true });
  };
  const skipGeneration = (scope, completion) => {
    const record = records.get(identity(scope, completion));
    if (!record || record.status === 'pending') consume(scope, completion);
  };
  return {
    get(scope, completion) { return records.get(identity(scope, completion)) || EMPTY; },
    subscribe(scope, completion, listener) {
      const key = identity(scope, completion);
      if (!listeners.has(key)) listeners.set(key, new Set());
      listeners.get(key).add(listener);
      return () => {
        listeners.get(key)?.delete(listener);
        if (!listeners.get(key)?.size) listeners.delete(key);
      };
    },
    ensure(scope, completion, request, { draftReady = true, hasDraft = false } = {}) {
      if (!scope || !completion || !draftReady) return;
      if (hasDraft) { skipGeneration(scope, completion); return; }
      const key = identity(scope, completion);
      if (records.has(key)) return;
      const pending = { status: 'pending', prompt: '', consumed: false };
      publish(key, pending);
      Promise.resolve().then(() => records.get(key) === pending ? request(completion) : null).then((response) => {
        if (records.get(key) !== pending) return;
        publish(key, { status: 'ready', consumed: pending.consumed,
          prompt: !pending.consumed && response?.completion_key === completion ? validFollowUpPrompt(response.prompt) : '' });
      }).catch(() => {
        if (records.get(key) === pending) publish(key, { ...EMPTY, status: 'failed' });
      });
    },
    consume, skipGeneration,
  };
}

export const followUpPromptStore = createFollowUpPromptStore();
