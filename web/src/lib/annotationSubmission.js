// 发送组装：输入框、工具栏、浮窗 Ctrl+Enter 三条触发点共用这一份。
//
// 组装只有一处（Q35）：把输入框正文 + 会话内全部待发记录拼成一条消息。
// 工具栏的按钮不自己拼载荷、不自己 POST，它只是触发同一个提交。

import { runPageAnnotationCommand } from './pageAnnotations.js';

const SHOT_PREFIX = 'page-annotations';

// 把原生侧/服务端的机器码换成产品文案，避免原始 code 泄漏到工具栏。
export function localizedAnnotationSendError(error) {
  if (error === 'annotation_target_or_document_changed' ||
      error === 'Annotation document or target is invalid') {
    return '批注目标已失效，请重新选择元素后再发送';
  }
  if (error === 'capture_document_changed') return '网页内容已变化，请重新批注后再发送';
  if (error === 'Annotation page is unavailable') return '批注页面不可用';
  return String(error || '发送失败');
}

function dataUrlBase64(value) {
  const source = String(value || '');
  const comma = source.indexOf(',');
  return source.startsWith('data:') && comma >= 0 ? source.slice(comma + 1) : source;
}

// 记录按页签分组，页内按编号升序；发送范围是会话全量（含 orphan）。
export function groupRecordsByPage(records) {
  const groups = [];
  const index = new Map();
  for (const record of records || []) {
    let group = index.get(record.pageId);
    if (!group) {
      group = { pageId: record.pageId, title: record.pageTitle, url: record.pageUrl, records: [] };
      index.set(record.pageId, group);
      groups.push(group);
    }
    group.records.push(record);
  }
  for (const group of groups) group.records.sort((a, b) => a.number - b.number);
  return groups;
}

export function annotationPagesPayload(groups, shotNames = new Map()) {
  return (groups || []).map((group) => ({
    page_id: group.pageId || '',
    url: group.url || '',
    title: group.title || '',
    ...(shotNames.get(group.pageId) ? { shot: shotNames.get(group.pageId) } : {}),
    annotations: group.records.map((record) => ({
      number: record.number,
      element: record.element,
      description: record.description,
      state: record.state,
      ...(Array.isArray(record.targets) && record.targets.length ? { targets: record.targets } : {}),
      ...(record.region ? { region: record.region } : {}),
      ...(Array.isArray(record.differences) && record.differences.length
        ? { differences: record.differences } : {}),
    })),
  }));
}

export function annotationContext(pages) {
  return { type: 'page_annotations', version: 2, pages };
}

export function annotationSubmissionCacheKey(sessionId, text, records) {
  return JSON.stringify({
    sessionId: String(sessionId || ''),
    text: String(text ?? ''),
    recordIds: (records || []).map((record) => String(record?.id || '')),
    pages: annotationPagesPayload(groupRecordsByPage(records)),
  });
}

export function annotationShotName(clientId, index) {
  return `${SHOT_PREFIX}-${clientId}-${index + 1}.png`;
}

// 提交器注册表：输入框所在组件把「组装 + 发送」注册进来，工具栏与浮窗只是触发器。
// 用注册表而不是 props 串联，是因为工具栏长在浏览器面板里，跟输入框不在同一棵子树。
const submitters = new Map();

export function registerAnnotationSubmitter(sessionId, submit) {
  if (!sessionId || typeof submit !== 'function') return () => {};
  submitters.set(sessionId, submit);
  return () => {
    if (submitters.get(sessionId) === submit) submitters.delete(sessionId);
  };
}

export function hasAnnotationSubmitter(sessionId) {
  return submitters.has(sessionId);
}

export async function requestAnnotationSubmit(sessionId) {
  const submit = submitters.get(sessionId);
  if (!submit) return { ok: false, error: '请先打开一个对话再发送批注' };
  try {
    const result = await submit();
    return result && typeof result === 'object'
      ? result
      : { ok: false, error: '发送失败' };
  } catch (error) {
    return { ok: false, error: String(error?.message || error || '发送失败') };
  }
}

// 超时自查（#215）：40 秒没收到确认时，先自己去查这条到底进没进去，再决定怎么办。
// 查到 → 视为成功，不重发；查不到 → 批注退回待发；查询本身失败 → 不回滚。
// 请求本身超时（api.js 默认 30 秒）同样走这条自查，否则 30 秒先抛错，
// 40 秒的兜底永远走不到。
export const ANNOTATION_SEND_TIMEOUT_MS = 40000;

export function isAnnotationTimeoutError(error) {
  return error?.status === 408 ||
    error?.code === 'TIMEOUT' ||
    error?.name === 'AbortError';
}

export async function findMessageByClientId(api, sessionId, clientMessageId) {
  if (!clientMessageId) return false;
  const page = await api.getMessages(sessionId, { limit: 100 });
  const messages = Array.isArray(page) ? page : (Array.isArray(page?.messages) ? page.messages : []);
  return messages.some((message) => message?.metadata?.client_message_id === clientMessageId);
}

export async function annotationSendOutcome({
  api, sessionId, clientMessageId, send, timeoutMs = ANNOTATION_SEND_TIMEOUT_MS,
}) {
  let timer = null;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(() => resolve({ status: 'timeout' }), timeoutMs);
  });
  const settled = Promise.resolve(send).then(
    (value) => ({ status: 'accepted', value }),
    (error) => ({ status: 'rejected', error }),
  );
  let result;
  try {
    result = await Promise.race([settled, timeout]);
  } finally {
    if (timer) clearTimeout(timer);
  }
  if (result.status === 'accepted') return result;
  if (result.status === 'rejected' && !isAnnotationTimeoutError(result.error)) {
    throw result.error;
  }
  try {
    const found = await findMessageByClientId(api, sessionId, clientMessageId);
    return found ? { status: 'confirmed' } : { status: 'lost' };
  } catch {
    // 自查本身失败：不回滚，交给用户决定重试还是撤销。
    return { status: 'unknown' };
  }
}

// 把待发记录组装成一条消息：上传每页的整屏图（已发卡片配图），批注结构走
// contexts/page_annotations，正文只保留用户输入的自然语言。
// 返回的 settle(ok) 负责发送成功后的清理：活着页签上删掉已发记录，orphan 只清本地。
export async function prepareAnnotationSubmission({
  sessionId, text, attachments = [], records = [], outbox = null, api,
  invoke = runPageAnnotationCommand,
  makeId = () => globalThis.crypto?.randomUUID?.() || String(Date.now()),
  retry = null,
}) {
  const groups = groupRecordsByPage(records).filter((group) => group.records.length > 0);
  const bodyText = String(text ?? '');
  const cacheKey = annotationSubmissionCacheKey(sessionId, bodyText, records);
  if (retry?.cacheKey === cacheKey) return retry.prepared;
  if (!groups.length) {
    return {
      text: bodyText, contexts: [], attachments, recordIds: [], groups, pages: [],
      cacheKey, clientMessageId: '', settle: () => {},
    };
  }
  const clientId = makeId();
  const shotNames = new Map();
  const shotAttachments = [];
  if (api?.uploadSessionAttachment) {
    for (const [index, group] of groups.entries()) {
      const page = outbox?.getSnapshot?.().pages?.find((item) => item.id === group.pageId);
      const shot = page?.pageShot;
      if (typeof shot !== 'string' || !shot) continue;
      const name = annotationShotName(clientId, index);
      const uploaded = await api.uploadSessionAttachment(sessionId, {
        name, mime_type: 'image/png', data_base64: dataUrlBase64(shot),
      });
      const id = uploaded?.attachment?.id;
      if (typeof id !== 'string' || !id) continue;
      shotNames.set(group.pageId, name);
      shotAttachments.push({ id, name });
    }
  }
  const pages = annotationPagesPayload(groups, shotNames);
  const recordIds = groups.flatMap((group) => group.records.map((record) => record.id));
  const settle = (ok) => {
    if (!ok) return;
    for (const group of groups) {
      const page = outbox?.getSnapshot?.().pages?.find((item) => item.id === group.pageId);
      if (!page?.closed) {
        // 活页签：让原生存储把已发记录删掉，否则下一次快照会把它们复活。
        void Promise.resolve(invoke(group.pageId, {
          action: 'remove', document_id: '', record_ids: group.records.map((record) => record.id),
        })).catch(() => {});
      }
    }
    outbox?.markSent(recordIds);
  };
  return {
    text: bodyText,
    contexts: [annotationContext(pages)],
    attachments: [...attachments, ...shotAttachments.map((item) => ({ ...item, page_shot: true }))],
    recordIds,
    groups,
    pages,
    shotAttachments,
    cacheKey,
    // 发送方用它作为 client_message_id：超时自查和重试都认同一条消息。
    clientMessageId: clientId,
    settle,
  };
}
