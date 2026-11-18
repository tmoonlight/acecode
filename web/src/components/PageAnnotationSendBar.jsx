import { useMemo, useSyncExternalStore } from 'react';
import { annotationIsSendable, pageAnnotationBatchSender } from '../lib/pageAnnotationBatches.js';

export function PageAnnotationSendBar({ pageId, snapshot, sessionId, api, page, onAccepted }) {
  const sender = useMemo(() => api ? pageAnnotationBatchSender(pageId, api) : null, [pageId, api]);
  const state = useSyncExternalStore(sender?.subscribe || emptySubscribe,
    sender?.getSnapshot || emptySnapshot);
  const pending = (snapshot.records || []).filter((record) => record.status !== 'sent');
  if (!pending.length && !state.error) return null;
  const sendable = pending.filter(annotationIsSendable);
  return (
    <div data-ace-native-overlay="overlap" className="flex items-center gap-3 border-t border-border px-3 py-2 text-xs">
      {pending.length > 0 && <span aria-live="polite">待发送 {pending.length}</span>}
      {state.error && <span role="alert" className="min-w-0 flex-1 text-err">{state.error}</span>}
      <button type="button" disabled={!sender || state.working || (!sendable.length && state.phase === 'idle')}
        className="ml-auto rounded-md bg-accent px-3 py-1.5 text-white disabled:opacity-50"
        onClick={async () => {
          const result = await sender.send({ snapshot, sessionId, api, page });
          if (result?.ok) onAccepted?.(result.annotations);
        }}>
        {state.working ? '发送中' : state.phase === 'idle' ? '发送批注' : '重试'}
      </button>
    </div>
  );
}

const emptyState = Object.freeze({ working: false, error: '', phase: 'idle' });
const emptySnapshot = () => emptyState;
const emptySubscribe = () => () => {};
