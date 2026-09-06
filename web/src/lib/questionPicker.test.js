// QuestionPicker helper 单元测试:覆盖双入口状态模型、payload active 过滤、
// 离开校验、导航禁用等纯逻辑(ask-user-question-dual-entry)。

import assert from 'node:assert/strict';
import {
  allQuestionsAnswered,
  buildQuestionAnswerPayload,
  buildQuestionCancelPayload,
  findIncompleteQuestion,
  getNavigationState,
  hasSelectedTextWithin,
  isQuestionAnswered,
  isSupplementActive,
  makeInitialAnswers,
  normalizeQuestionRequest,
  setExclusiveText,
  setSupplement,
  toggleAnswerSelection,
  toggleExclusive,
  validateAnswerCompleteness,
} from './questionPicker.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const request = normalizeQuestionRequest({
  request_id: 'req-1',
  session_id: 'sid-1',
  questions: [
    {
      id: 'q1',
      text: '你想做什么?',
      options: [
        { label: '修复 bug', value: 'fix-bug', description: '诊断问题' },
        { label: '添加功能', value: 'add-feature', description: '实现新能力' },
      ],
    },
    {
      id: 'q2',
      text: '需要哪些质量项?',
      multiSelect: true,
      options: [
        { label: '测试', value: 'tests' },
        { label: '文档', value: 'docs' },
      ],
    },
  ],
});

run('单选 payload 使用 option value', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'fix-bug', false);
  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], { question_id: 'q1', selected: ['fix-bug'] });
});

run('多选 payload 保留多个选中值', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'add-feature', false);
  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[1], { question_id: 'q2', selected: ['tests', 'docs'] });
});

run('预设 + 补充 payload 同时发 selected 与 supplement_text', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'fix-bug', false);
  answers[0] = setSupplement(answers[0], '  预算上限 5000  ');
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], {
    question_id: 'q1',
    selected: ['fix-bug'],
    supplement_text: '预算上限 5000',
  });
});

run('仅补充(独立)payload 只有 supplement_text', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = setSupplement(answers[0], '其它需求');
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], {
    question_id: 'q1',
    selected: [],
    supplement_text: '其它需求',
  });
});

run('激活以上都不是:清空预设,payload 只发 exclusive_text', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  answer = toggleAnswerSelection(answer, 'fix-bug', false);
  answer = setSupplement(answer, '旧补充文本');
  answer = toggleExclusive(answer); // 激活:清空 selected,supplement 停用(文本保留)
  assert.deepEqual(answer.selected, []);
  assert.equal(answer.exclusiveActive, true);
  assert.equal(isSupplementActive(answer), false); // 被压制
  assert.equal(isQuestionAnswered(answer), true); // 勾了就算答
  answer = setExclusiveText(answer, '都不合适');
  const answers = [answer];
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], {
    question_id: 'q1',
    selected: [],
    exclusive_text: '都不合适',
  });
});

run('exclusive active 时 payload 不含 supplement_text(即使文本非空)', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  answer = setSupplement(answer, '留着反悔用的旧文本');
  answer = toggleExclusive(answer);
  answer = setExclusiveText(answer, '都不合适');
  const payload = buildQuestionAnswerPayload(request, request.questions, [answer]);
  assert.deepEqual(payload.answers[0], {
    question_id: 'q1',
    selected: [],
    exclusive_text: '都不合适',
  });
  assert.ok(!('supplement_text' in payload.answers[0]));
});

run('反悔:独占态点预设 → 退出独占、exclusiveText 保留、重新多选', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  answer = toggleExclusive(answer);
  answer = setExclusiveText(answer, '都不合适');
  answer = toggleAnswerSelection(answer, 'fix-bug', false); // 反悔改选预设
  assert.equal(answer.exclusiveActive, false);
  assert.equal(answer.exclusiveText, '都不合适'); // 文本保留(数据永不自动清除)
  assert.deepEqual(answer.selected, ['fix-bug']);
  const payload = buildQuestionAnswerPayload(request, request.questions, [answer]);
  // 退出独占后旧 exclusive 文本不发,但 supplement 为空也不发
  assert.deepEqual(payload.answers[0], { question_id: 'q1', selected: ['fix-bug'] });
});

run('点回置灰补充框输入 → 自动取消独占并恢复补充', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  answer = toggleExclusive(answer);
  answer = setSupplement(answer, '打几个字'); // 从置灰框继续输入
  assert.equal(answer.exclusiveActive, false);
  assert.equal(answer.supplement, '打几个字');
});

run('清空补充文本 = 未激活', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  answer = setSupplement(answer, '一些内容');
  assert.equal(isSupplementActive(answer), true);
  answer = setSupplement(answer, '   ');
  assert.equal(isSupplementActive(answer), false);
});

run('离开校验:exclusive active 空文本不可离开,填字后可离开', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  answer = toggleExclusive(answer);
  assert.deepEqual(validateAnswerCompleteness(answer), { ok: false, reason: 'exclusive_empty' });
  answer = setExclusiveText(answer, '都不合适');
  assert.deepEqual(validateAnswerCompleteness(answer), { ok: true });
});

run('整组强校验:findIncompleteQuestion 返回首个未完成题', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleExclusive(answers[0]); // 空文本独占 → 未完成
  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  assert.equal(findIncompleteQuestion(request.questions, answers), 0);
  answers[0] = setExclusiveText(answers[0], '都不合适');
  assert.equal(findIncompleteQuestion(request.questions, answers), -1);
});

run('isQuestionAnswered:exclusive active 空文本判已作答(宽松导航判定)', () => {
  let answer = makeInitialAnswers(request.questions)[0];
  assert.equal(isQuestionAnswered(answer), false);
  answer = toggleExclusive(answer); // active 但空文本
  assert.equal(isQuestionAnswered(answer), true);
  answer = setSupplement(answer, '补充也算答');
  answer = toggleExclusive(answer);
  answer = setSupplement(answer, '补充也算答');
  assert.equal(isQuestionAnswered(answer), true);
});

run('取消 payload 不包含部分答案', () => {
  assert.deepEqual(buildQuestionCancelPayload(request), {
    request_id: 'req-1',
    session_id: 'sid-1',
    cancelled: true,
  });
});

run('未回答问题禁用推进和提交', () => {
  const answers = makeInitialAnswers(request.questions);
  const state = getNavigationState(0, request.questions, answers);
  assert.equal(isQuestionAnswered(answers[0]), false);
  assert.equal(allQuestionsAnswered(request.questions, answers), false);
  assert.equal(state.canGoNext, false);
  assert.equal(state.canSubmit, false);
});

run('答案卡片内有鼠标选区时保留选区', () => {
  const target = {};
  const selection = {
    isCollapsed: false,
    rangeCount: 1,
    getRangeAt: () => ({ intersectsNode: (node) => node === target }),
  };
  assert.equal(hasSelectedTextWithin(target, selection), true);
  assert.equal(hasSelectedTextWithin({}, selection), false);
});

run('折叠选区不会阻止答案点击', () => {
  const selection = {
    isCollapsed: true,
    rangeCount: 1,
    getRangeAt: () => ({ intersectsNode: () => true }),
  };
  assert.equal(hasSelectedTextWithin({}, selection), false);
});
