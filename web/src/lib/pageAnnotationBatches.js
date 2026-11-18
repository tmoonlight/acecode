import { apiConnectionScope } from './api.js';
import { runPageAnnotationCommand } from './pageAnnotations.js';
import { AGENT_BROWSER_STATE_EVENT } from './agentBrowser.js';

export function annotationIsSendable(record) {
  return record?.status === 'pending' &&
    (String(record.description || '').trim().length > 0 ||
      (Array.isArray(record.differences) && record.differences.length > 0));
}

export function formatAnnotationBatch(records, page) {
  return `Page annotations\n${JSON.stringify({ page, annotations: records.map((record) => ({
    number: record.number, description: record.description,
    targets: record.targets, region: record.region, differences: record.differences,
  })) }, null, 2)}`;
}

export function createAnnotationBatchSender({
  pageId, invoke = runPageAnnotationCommand,
  makeId = () => globalThis.crypto.randomUUID(),
  freezeApi = (client) => client.snapshotConnection(),
  onRetain = () => {}, onRelease = () => {},
}) {
  let batch = null;
  let flight = null;
  let state = { working: false, error: '', phase: 'idle', sessionId: '' };
  const listeners = new Set();
  const publish = (changes) => {
    state = { ...state, ...changes };
    for (const listener of listeners) listener();
  };
  const nativeCommand = async (action) => {
    const result = await invoke(pageId, {
      action, document_id: batch.documentId, record_ids: batch.recordIds,
    });
    if (result?.ok !== true || result.page_id !== pageId) {
      throw new Error(result?.error || '批注页面不可用');
    }
    return result;
  };
  const run = async ({ snapshot, sessionId, api, page }) => {
    try {
      publish({ working: true, error: '' });
      if (!sessionId || !api) throw new Error('请先选择对话');
      if (batch && sessionId !== batch.sessionId) throw new Error('请切回发送批注的对话后重试');
      if (!batch) {
        const records = JSON.parse(JSON.stringify((snapshot?.records || []).filter(annotationIsSendable)));
        if (!records.length) throw new Error('没有可发送的批注');
        if (!snapshot.document_id || records.some((record) => record.valid !== true ||
            record.document_id !== snapshot.document_id)) throw new Error('批注目标已失效');
        batch = {
          id: makeId(), documentId: snapshot.document_id, sessionId,
          recordIds: records.map((record) => record.id),
          text: formatAnnotationBatch(records, { url: page?.url || '', title: page?.title || '' }),
          api: freezeApi(api), capture: null, payload: null, accepted: false, attempted: false,
        };
        onRetain();
        publish({ sessionId });
      }
      if (!batch.payload) {
        if (!batch.capture) {
          publish({ phase: 'capture' });
          const result = await nativeCommand('capture');
          if (result.annotation_document_id !== batch.documentId ||
              result.result?.fullPage !== true || !result.result?.documentId ||
              typeof result.result?.data !== 'string' || !result.result.data) {
            throw new Error('整页截图与批注文档不匹配');
          }
          batch.capture = result.result;
        }
        publish({ phase: 'upload' });
        const uploaded = await batch.api.uploadSessionAttachment(batch.sessionId, {
          name: `page-annotations-${batch.id}.png`, mime_type: 'image/png',
          data_base64: batch.capture.data,
        });
        const attachmentId = uploaded?.attachment?.id;
        if (typeof attachmentId !== 'string' || !attachmentId) throw new Error('截图上传未返回附件标识');
        batch.payload = JSON.stringify({
          annotation_batch_id: batch.id, text: batch.text, attachments: [{ id: attachmentId }],
        });
        batch.capture = null;
      }
      if (!batch.accepted) {
        if (!batch.attempted) await nativeCommand('validate');
        publish({ phase: 'receipt' });
        batch.attempted = true;
        const receipt = await batch.api.sendInput(batch.sessionId, JSON.parse(batch.payload));
        if (receipt?.queued !== true || receipt.annotation_batch_id !== batch.id) {
          throw new Error('尚未收到批注接收回执，请重试');
        }
        batch.accepted = true;
      }
      publish({ phase: 'acknowledge' });
      const result = await nativeCommand('acknowledge');
      batch = null;
      if (!listeners.size) onRelease();
      publish({ phase: 'idle', sessionId: '' });
      return result;
    } catch (error) {
      publish({ error: String(error?.message || error) });
      return { ok: false, error: state.error };
    } finally {
      publish({ working: false });
    }
  };
  return {
    getSnapshot: () => state,
    subscribe(listener) {
      listeners.add(listener);
      onRetain();
      return () => {
        listeners.delete(listener);
        if (!listeners.size && !batch) onRelease();
      };
    },
    send(context) {
      if (flight) return flight;
      flight = run(context).finally(() => { flight = null; });
      return flight;
    },
  };
}

const retainedBatches = new WeakMap();

export function pageAnnotationBatchSender(pageId, api) {
  const scope = apiConnectionScope(api);
  let pages = retainedBatches.get(scope);
  if (!pages) { pages = new Map(); retainedBatches.set(scope, pages); }
  if (pages.has(pageId)) return pages.get(pageId);
  const release = () => {
    if (pages.get(pageId) === sender) pages.delete(pageId);
    globalThis.window?.removeEventListener?.(AGENT_BROWSER_STATE_EVENT, onPageState);
  };
  const onPageState = (event) => {
    if (event.detail?.page_id === pageId && event.detail.closed === true) release();
  };
  const retain = () => {
    pages.set(pageId, sender);
    globalThis.window?.addEventListener?.(AGENT_BROWSER_STATE_EVENT, onPageState);
  };
  const sender = createAnnotationBatchSender({ pageId,
    onRetain: retain,
    onRelease: release,
  });
  retain();
  return sender;
}
