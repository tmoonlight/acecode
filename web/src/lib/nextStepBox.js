// 回合末尾的「下一步」框(仿 Claude 桌面版 wrap_up)。
//
// task_complete 成功后在 tool metadata.wrap_up 里带 daemon 规范化过的结果:
// { recap, box?, ask?, actions?: [{label}], blocked? }。只有 box 为 true 才有问题 /
// 按钮;recap 仍由完成总结(completion_summary)显示,这里不重复。
//
// 只有 transcript 的最后一项是带框的完成总结、回合已结束、会话可发消息时,框才可
// 操作(画在 transcript 末尾,点按钮把按钮文字作为用户回复发出);其余情况(历史回合、
// 用户已关闭)问题以静态文字留在完成总结里。

export const NEXT_STEP_DISMISSED_STORAGE_KEY = 'acecode.nextStepDismissed.v1';
const DISMISSED_CAP = 200;
const MAX_ACTIONS = 2;

function plainObject(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

export function wrapUpFromTool(tool) {
  const raw = plainObject(tool?.metadata?.wrap_up);
  if (!raw) return null;
  const box = raw.box === true;
  const actions = box && Array.isArray(raw.actions)
    ? raw.actions
      .map((action) => String((typeof action === 'string' ? action : action?.label) ?? '').trim())
      .filter(Boolean)
      .slice(0, MAX_ACTIONS)
    : [];
  return {
    recap: String(raw.recap ?? '').trim(),
    box,
    ask: box ? String(raw.ask ?? '').trim() : '',
    actions,
    blocked: raw.blocked === true,
  };
}

export function nextStepKey(item) {
  return String(item?.messageId || item?.sourceItem?.tool?.toolCallId || item?.id || '').trim();
}

export function hasNextStepQuestion(item) {
  const wrapUp = item?.wrapUp;
  return !!(wrapUp?.box && (wrapUp.ask || wrapUp.actions?.length > 0));
}

export function activeNextStep(items, { busy = false, interactive = true, dismissed = null } = {}) {
  if (busy || !interactive || !Array.isArray(items) || items.length === 0) return null;
  const last = items[items.length - 1];
  if (last?.kind !== 'completion_summary' || !hasNextStepQuestion(last)) return null;
  const key = nextStepKey(last);
  if (!key || (dismissed && Object.prototype.hasOwnProperty.call(dismissed, key))) return null;
  return {
    key,
    itemId: last.id,
    ask: last.wrapUp.ask,
    actions: last.wrapUp.actions,
    blocked: last.wrapUp.blocked,
  };
}

export function readDismissedNextSteps(storage = globalThis.localStorage) {
  try {
    const parsed = JSON.parse(storage?.getItem(NEXT_STEP_DISMISSED_STORAGE_KEY) || '{}');
    return plainObject(parsed) || {};
  } catch {
    return {};
  }
}

// 记住关闭过的框(按完成总结的消息 id),只留最近 200 条。
export function withDismissedNextStep(dismissed, key, now = Date.now()) {
  if (!key) return dismissed || {};
  const entries = Object.entries({ ...(dismissed || {}), [key]: now })
    .sort((a, b) => (Number(a[1]) || 0) - (Number(b[1]) || 0));
  return Object.fromEntries(entries.slice(Math.max(0, entries.length - DISMISSED_CAP)));
}

export function writeDismissedNextSteps(dismissed, storage = globalThis.localStorage) {
  try {
    storage?.setItem(NEXT_STEP_DISMISSED_STORAGE_KEY, JSON.stringify(dismissed || {}));
  } catch {
    // 隐私窗口 / 存储被禁用时只在本次页面内生效。
  }
}
