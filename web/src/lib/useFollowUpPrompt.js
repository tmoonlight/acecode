import { useCallback, useEffect, useSyncExternalStore } from 'react';
import { followUpCompletionKey, followUpScope, followUpPromptStore, hasFollowUpDraft } from './followUpPrompt.js';

export function useFollowUpPrompt({ sessionRef, transcript, api, enabled = true, draftReady = false, draft }) {
  const sid = sessionRef?.sessionId || sessionRef?.id || '';
  const scope = followUpScope(sessionRef);
  const completion = enabled && sid ? followUpCompletionKey(transcript) : '';
  const hasDraft = hasFollowUpDraft(draft);
  const subscribe = useCallback((notify) => followUpPromptStore.subscribe(scope, completion, notify), [scope, completion]);
  const getSnapshot = useCallback(() => followUpPromptStore.get(scope, completion), [scope, completion]);
  const record = useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
  // One gate covers both the first loaded idle view and a live turn ending,
  // including tool-only endings. No focus listener, debounce or polling.
  useEffect(() => {
    if (!completion) return;
    followUpPromptStore.ensure(scope, completion, (key) => api.followUpPrompt(sid, key), { draftReady, hasDraft });
  }, [api, scope, sid, completion, draftReady, hasDraft]);
  const consume = useCallback(() => followUpPromptStore.consume(scope, completion), [scope, completion]);
  const skipGeneration = useCallback(() => {
    if (draftReady && completion) followUpPromptStore.skipGeneration(scope, completion);
  }, [scope, completion, draftReady]);
  return { prompt: draftReady && !hasDraft && completion ? record.prompt : '', consume, skipGeneration };
}
