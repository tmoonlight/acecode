// AskUserQuestion picker 的纯逻辑 helper。
// 保持与 daemon question_answer 协议兼容,供 React 组件与 Node 单测复用。
//
// AskUserQuestion 双入口(我要补充 / 以上都不是,ask-user-question-dual-entry):
// 每题 answer 状态 = { selected, supplement, exclusiveActive, exclusiveText }
//   selected        已选预设 label(value)
//   supplement      补充说明文本 —— 非空且未独占即激活(打字即生效,无勾选框)
//   exclusiveActive 以上都不是 勾选态(勾选即激活;激活即清空 selected)
//   exclusiveText   以上都不是 手填文本(激活时必填,离开当前题时校验)
// 互斥:激活 exclusive → supplement 停用但文本保留;点回 supplement 输入 →
// 自动取消 exclusiveActive。被压制的 inactive 文本不进 payload(active 过滤)。

function toText(value, fallback = '') {
  if (typeof value === 'string') return value;
  if (value == null) return fallback;
  return String(value);
}

function optionValue(option, index) {
  const label = toText(option?.label, `Option ${index + 1}`);
  const value = option && option.value != null ? toText(option.value, label) : label;
  return { label, value };
}

export function normalizeQuestionRequest(request = {}) {
  const rawQuestions = Array.isArray(request.questions) ? request.questions : [];
  return {
    requestId: toText(request.request_id),
    sessionId: toText(request.session_id),
    questions: rawQuestions.map((q, qi) => {
      const text = toText(q?.text || q?.question || q?.header, `Question ${qi + 1}`);
      const options = Array.isArray(q?.options) ? q.options : [];
      return {
        id: toText(q?.id || q?.question || q?.text, text),
        text,
        header: toText(q?.header),
        multiSelect: !!q?.multiSelect,
        options: options.map((opt, oi) => {
          const normalized = optionValue(opt, oi);
          return {
            ...normalized,
            description: toText(opt?.description),
          };
        }),
      };
    }),
  };
}

export function makeInitialAnswers(questions = []) {
  return questions.map(() => ({
    selected: [],
    supplement: '',
    exclusiveActive: false,
    exclusiveText: '',
  }));
}

// supplement 是否 active(派生值):文本非空 且 未被「以上都不是」压制。
export function isSupplementActive(answer = {}) {
  return !answer.exclusiveActive && toText(answer.supplement).trim().length > 0;
}

// 已作答判定(宽松,供导航/按钮态):≥1 预设 或 补充激活 或 独占勾选。
// 独占勾选但文本为空也算已作答(grill Q1:勾了就算答)——「离开校验」
// validateAnswerCompleteness 在导航/提交动作时兜底拦截,不会带着空文本离开。
export function isQuestionAnswered(answer = {}) {
  return (Array.isArray(answer.selected) && answer.selected.length > 0) ||
    isSupplementActive(answer) ||
    !!answer.exclusiveActive;
}

// 离开当前题强校验(grill Q1):独占勾选但文本为空 → 不可离开。
// 返回 { ok: true } 或 { ok: false, reason }。
export function validateAnswerCompleteness(answer = {}) {
  if (answer.exclusiveActive && toText(answer.exclusiveText).trim().length === 0) {
    return { ok: false, reason: 'exclusive_empty' };
  }
  return { ok: true };
}

export function allQuestionsAnswered(questions = [], answers = []) {
  return questions.length > 0 && questions.every((_, index) => isQuestionAnswered(answers[index]));
}

// 整组提交前的逐题强校验(状态机兜底;逐题离开时已拦,理论不触发)。
// 返回第一个未完成题的 index,全部通过返回 -1。
export function findIncompleteQuestion(questions = [], answers = []) {
  for (let i = 0; i < questions.length; i += 1) {
    if (!validateAnswerCompleteness(answers[i]).ok) return i;
  }
  return -1;
}

export function getNavigationState(currentIndex, questions = [], answers = []) {
  const total = questions.length;
  const currentAnswered = isQuestionAnswered(answers[currentIndex]);
  const isLast = total > 0 && currentIndex >= total - 1;
  return {
    total,
    current: total === 0 ? 0 : currentIndex + 1,
    isLast,
    currentAnswered,
    allAnswered: allQuestionsAnswered(questions, answers),
    canGoPrev: currentIndex > 0,
    canGoNext: total > 0 && !isLast && currentAnswered,
    canSubmit: total > 0 && isLast && allQuestionsAnswered(questions, answers),
  };
}

// 切换预设选择。单选换选、多选切换;任何预设操作都会退出独占态
// (exclusiveActive=false,exclusiveText 保留置灰,grill 数据永不自动清除)。
export function toggleAnswerSelection(answer = {}, value, multiSelect) {
  const selected = Array.isArray(answer.selected) ? answer.selected : [];
  const textValue = toText(value);
  if (!textValue) return { ...answer, selected };
  let nextSelected;
  if (!multiSelect) {
    nextSelected = [textValue];
  } else {
    const hasValue = selected.includes(textValue);
    nextSelected = hasValue
      ? selected.filter((item) => item !== textValue)
      : [...selected, textValue];
  }
  return { ...answer, selected: nextSelected, exclusiveActive: false };
}

// 补充说明框内容:打字即生效(非空即激活);若独占激活中,输入即自动恢复
// 补充并取消「以上都不是」(方案 C 规则 2)。
export function setSupplement(answer = {}, supplement) {
  const text = toText(supplement);
  const wasExclusiveSuppressing = answer.exclusiveActive && text.trim().length > 0;
  return {
    ...answer,
    supplement: text,
    // 从置灰框输入内容 → 取消独占、恢复补充。
    exclusiveActive: wasExclusiveSuppressing ? false : !!answer.exclusiveActive,
  };
}

// 勾选/取消「以上都不是」。激活:清空预设勾选(所见即所得),supplement
// 停用但文本保留(UI 置灰);取消:仅退勾,所有文本保留。
export function toggleExclusive(answer = {}) {
  if (answer.exclusiveActive) {
    return { ...answer, exclusiveActive: false };
  }
  return { ...answer, selected: [], exclusiveActive: true };
}

export function setExclusiveText(answer = {}, text) {
  return { ...answer, exclusiveText: toText(text) };
}

export function hasSelectedTextWithin(target, selection) {
  if (!target || !selection || selection.isCollapsed || selection.rangeCount <= 0) return false;
  try {
    return selection.getRangeAt(0).intersectsNode(target);
  } catch {
    return false;
  }
}

// 构建 question_answer 协议 payload。只输出 **active 入口** 的文本字段
// (grill Q2):exclusive active 时丢弃 supplement_text,即使本地有保留旧文本。
// custom_text 旧字段已移除。
export function buildQuestionAnswerPayload(request = {}, questions = [], answers = []) {
  const payload = {
    request_id: toText(request.requestId || request.request_id),
    session_id: toText(request.sessionId || request.session_id),
    answers: questions.map((q, index) => {
      const answer = answers[index] || {};
      const selected = Array.isArray(answer.selected)
        ? answer.selected.map((item) => toText(item)).filter(Boolean)
        : [];
      const out = {
        question_id: toText(q.id || q.question || q.text),
        selected,
      };
      if (answer.exclusiveActive) {
        const exclusive = toText(answer.exclusiveText).trim();
        // 独占激活时只发 exclusive_text;空文本属未完成,离开校验已拦截,
        // 这里防御性不发。
        if (exclusive) out.exclusive_text = exclusive;
      } else {
        const supplement = toText(answer.supplement).trim();
        if (supplement) out.supplement_text = supplement;
      }
      return out;
    }),
  };
  if (!payload.session_id) delete payload.session_id;
  return payload;
}

export function buildQuestionCancelPayload(request = {}) {
  const payload = {
    request_id: toText(request.requestId || request.request_id),
    session_id: toText(request.sessionId || request.session_id),
    cancelled: true,
  };
  if (!payload.session_id) delete payload.session_id;
  return payload;
}
