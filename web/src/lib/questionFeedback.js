// AskUserQuestion 提交/取消/插话反馈卡(全部提交完成 / 已取消全部回答 /
// 已改为直接输入)的派生逻辑。
//
// 卡片必须能在会话中持久展示,所以数据来源是「已落盘的 AskUserQuestion 工具
// 消息元数据」(tool_end 的 metadata.ask_user_question_result),而不是组件里的
// 临时 state。这一点很关键:回合结束时的 transcript self-heal 会覆写最近一轮,
// 对不上的行换成新的 item id(见 lib/transcriptSelfHeal.js 的 withStableIds),
// 缓存下来的锚点 id 可能失效 —— 卡片随之消失。按 item 就地派生就没有这个问题。
//
// 判定不看工具名:历史分页可能只剩工具结果消息,或者工具被改名(改名后的调用
// 名不再是 AskUserQuestion)。唯一可靠的判据是落盘元数据本身,它只由
// AskUserQuestion 写出 —— 这也是「取消结果不被折叠进历史活动」的前提。
//
const NOT_ANSWERED_TEXT = 'Not answered';

function toText(value, fallback = '') {
  if (typeof value === 'string') return value;
  if (value == null) return fallback;
  return String(value);
}

// C++ 元数据使用 snake_case,历史规范化后使用 camelCase,两种都接受。
function summaryFromResultItems(resultItems) {
  if (!Array.isArray(resultItems)) return [];
  return resultItems
    .map((item) => {
      const question = toText(item?.question);
      const answer = toText(item?.answer);
      const trimmed = answer.trim();
      const notAnswered = item?.not_answered === true
        || item?.notAnswered === true
        || !trimmed
        || trimmed === NOT_ANSWERED_TEXT;
      return {
        question,
        multiSelect: item?.multi_select === true || item?.multiSelect === true,
        answer: notAnswered ? '' : answer,
        notAnswered,
      };
    })
    .filter((item) => item.question || item.answer);
}

export function questionFeedbackForTool(tool) {
  if (!tool || tool.isDone !== true) return null;
  const result = tool.askUserQuestionResult;
  if (!result || typeof result !== 'object' || Array.isArray(result)) return null;
  // 显式取消优先于插话标记:两者不会同时落盘,这里只固定判定顺序。
  if (result.cancelled === true) return { kind: 'cancel' };
  // 插话取消作答:用户没选项,而是直接输入了一条消息(紧跟其后的 user 气泡)。
  if (result.interjected === true) return { kind: 'interject' };
  if (tool.success === false) return null;
  const summary = summaryFromResultItems(result.items);
  return summary.length > 0 ? { kind: 'submit', summary } : null;
}

// 只按当前工具结果派生,避免连续提问或切换会话后误用上一题的答案。
export function questionFeedbackForItem(item) {
  return item?.kind === 'tool' ? questionFeedbackForTool(item.tool) : null;
}
