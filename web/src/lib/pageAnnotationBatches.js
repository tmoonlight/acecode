import { apiConnectionScope } from './api.js';
import { runPageAnnotationCommand } from './pageAnnotations.js';
import { AGENT_BROWSER_STATE_EVENT } from './agentBrowser.js';

// The native capture layer reports stable machine codes when the annotated
// target or page changed while sending. Map them to product copy so they join
// the i18n catalog instead of leaking raw codes into the toolbar (mirrors
// localizedTextError in PageAnnotationTextField).
export function localizedSendError(error) {
  if (error === 'annotation_target_or_document_changed' ||
      error === 'Annotation document or target is invalid') {
    return '批注目标已失效，请重新选择元素后再发送';
  }
  if (error === 'capture_document_changed') return '网页内容已变化，请重新批注后再发送';
  if (error === 'Annotation page is unavailable') return '批注页面不可用';
  if (error === 'Annotation capture is already running') return '截图正在进行中，请稍后再试';
  return error;
}

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
  const nativeCommand = async (action, leaseId) => {
    const result = await invoke(pageId, {
      action, document_id: batch.documentId, record_ids: batch.recordIds,
      ...(leaseId ? { lease_id: leaseId } : {}),
    });
    if (result?.ok !== true || result.page_id !== pageId) {
      throw new Error(result?.error || '批注页面不可用');
    }
    return result;
  };
  const releaseLease = async () => {
    if (!batch?.leasePending) return;
    await nativeCommand('release', batch.id);
    batch.leasePending = false;
  };
  const run = async ({ snapshot, sessionId, api, page }) => {
    try {
      publish({ working: true, error: '' });
      if (!sessionId) throw new Error('请先选择对话');
      if (!api) throw new Error('批注页面不可用');
      if (batch && sessionId !== batch.sessionId) throw new Error('请切回发送批注的对话后重试');
      if (batch?.leasePending) await releaseLease();
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
        publish({ phase: 'receipt' });
        const accept = (receipt) => {
          if (receipt?.queued !== true || receipt.annotation_batch_id !== batch.id) {
            throw new Error('尚未收到批注接收回执，请重试');
          }
          batch.accepted = true;
        };
        if (batch.attempted) {
          try {
            accept(await batch.api.sendInput(batch.sessionId, JSON.parse(batch.payload), { reconcileOnly: true }));
          } catch (error) {
            if (error?.status !== 404 || error?.code !== 'annotation_batch_not_accepted') throw error;
            batch.attempted = false;
          }
        }
        if (!batch.accepted) {
          let failure = null;
          try {
            batch.leasePending = true;
            await nativeCommand('validate', batch.id);
            batch.attempted = true;
            try {
              accept(await batch.api.sendInput(batch.sessionId, JSON.parse(batch.payload)));
            } catch (error) {
              if (['annotation_batch_unavailable', 'annotation_batch_capacity', 'annotation_batch_conflict'].includes(error?.code)) {
                batch.attempted = false;
              }
              throw error;
            }
          } catch (error) {
            failure = error;
            throw error;
          } finally {
            try { await releaseLease(); }
            catch (error) { if (!failure) throw error; }
          }
        }
      }
      publish({ phase: 'acknowledge' });
      const result = await nativeCommand('acknowledge');
      batch = null;
      if (!listeners.size) onRelease();
      publish({ phase: 'idle', sessionId: '' });
      return result;
    } catch (error) {
      publish({ error: localizedSendError(String(error?.message || error)) });
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
