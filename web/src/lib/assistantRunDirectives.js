// 决定 transcript 中每条 assistant 消息的连续 run 边界和底部 footer。
//
// 规则:
// - 用户消息出现 → 重置 run
// - 同一 run 内的多条 assistant 消息(中间穿插 tool 行 / system 行 不影响),
//   只有第一条是非 continuation,其余为 continuation
// - 工具行(kind === 'tool')和折叠活动摘要不影响 run 状态
// - 空内容(trim 后为空)的 assistant 消息隐藏整行, 且不消耗
//   header 名额(避免 LLM 仅发起 tool_call 时, 头一个空气泡偷走 header)
// - 已结束 run 只有一个 footer owner:有 completion_summary 时归完成总结,
//   否则归最后一条可见 assistant 消息
// - 当前末尾 run 尚未结束时,通过 deferLastFooter 延迟整个 footer
// - termination_notice / error 是显式终止证据,即使 busy 状态滞后也立即结算;
//   合成终止行从同一 turn 最近的真实消息继承 fork messageId

function directForkMessageId(item) {
  if (!item || (item.kind === 'msg' && item.role === 'error')) return '';
  return typeof item.messageId === 'string' ? item.messageId.trim() : '';
}

function latestForkMessageId(item) {
  const nestedLists = [item?.detailItems, item?.collapsedItems];
  for (const list of nestedLists) {
    if (!Array.isArray(list)) continue;
    for (let index = list.length - 1; index >= 0; index -= 1) {
      const nestedId = latestForkMessageId(list[index]);
      if (nestedId) return nestedId;
    }
  }
  return directForkMessageId(item);
}

export function buildAssistantRunDirectives(items, { deferLastFooter = false } = {}) {
  const directives = new Map();
  if (!Array.isArray(items)) return directives;
  let headerUsed = false;
  let visibleAssistantIds = [];
  let completionSummaryIds = [];
  let terminalIds = [];
  let latestTurnForkMessageId = '';

  const finalizeRun = ({ settled = true } = {}) => {
    const footerId = terminalIds[terminalIds.length - 1]
      ?? completionSummaryIds[completionSummaryIds.length - 1]
      ?? visibleAssistantIds[visibleAssistantIds.length - 1];
    for (const id of visibleAssistantIds) {
      const directive = directives.get(id);
      if (directive) directive.showFooter = settled && id === footerId;
    }
    for (const id of completionSummaryIds) {
      const directive = directives.get(id);
      if (directive) directive.showFooter = settled && id === footerId;
    }
    for (const id of terminalIds) {
      const directive = directives.get(id);
      if (directive) directive.showFooter = settled && id === footerId;
    }
    visibleAssistantIds = [];
    completionSummaryIds = [];
    terminalIds = [];
    latestTurnForkMessageId = '';
  };

  for (const it of items) {
    if (!it) continue;
    if (it.kind === 'msg' && it.role === 'user') {
      finalizeRun();
      headerUsed = false;
      latestTurnForkMessageId = latestForkMessageId(it);
      continue;
    }

    const forkMessageId = latestForkMessageId(it);
    if (forkMessageId) latestTurnForkMessageId = forkMessageId;

    const isTerminal = it.kind === 'termination_notice'
      || (it.kind === 'msg' && it.role === 'error');
    if (isTerminal) {
      directives.set(it.id, {
        showFooter: false,
        forkMessageId: latestTurnForkMessageId,
      });
      terminalIds.push(it.id);
      continue;
    }
    if (it.kind === 'completion_summary') {
      directives.set(it.id, { showFooter: false });
      completionSummaryIds.push(it.id);
      continue;
    }
    if (it.kind !== 'msg') continue;
    if (it.role !== 'assistant') continue;
    const content = typeof it.content === 'string' ? it.content : '';
    const isEmpty = !content.trim();
    if (isEmpty) {
      directives.set(it.id, { hide: true });
      continue;
    }
    directives.set(it.id, { showHeader: !headerUsed, showFooter: false });
    visibleAssistantIds.push(it.id);
    headerUsed = true;
  }
  finalizeRun({ settled: !deferLastFooter || terminalIds.length > 0 });
  return directives;
}
