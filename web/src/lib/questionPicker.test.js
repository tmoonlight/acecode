// QuestionPicker helper 单元测试:覆盖 payload、取消和导航禁用等纯逻辑。

import assert from 'node:assert/strict';
import {
  allQuestionsAnswered,
  buildQuestionAnswerPayload,
  buildQuestionCancelPayload,
  getNavigationState,
  hasSelectedTextWithin,
  isQuestionAnswered,
  makeInitialAnswers,
  normalizeQuestionRequest,
  selectAnswerCustom,
  toggleAnswerSelection,
  setAnswerCustom,
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

run('自定义答案 payload 写入 custom_text', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = setAnswerCustom(answers[0], '  其它需求  ');
  answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], {
    question_id: 'q1',
    selected: [],
    custom_text: '其它需求',
  });
});

run('单选改填其他时清除普通选项且仅提交自定义答案', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'fix-bug', false);
  answers[0] = setAnswerCustom(answers[0], '  独立自定义答案  ', false);
  assert.deepEqual(answers[0].selected, []);
  assert.deepEqual(buildQuestionAnswerPayload(request, request.questions, answers).answers[0], {
    question_id: 'q1',
    selected: [],
    custom_text: '独立自定义答案',
  });
});

run('单选选中空白其他时立即清除普通选项并禁止推进和提交', () => {
  for (const custom of ['', '   ']) {
    const answers = makeInitialAnswers(request.questions);
    answers[0] = { ...toggleAnswerSelection(answers[0], 'fix-bug', false), custom };
    answers[0] = selectAnswerCustom(answers[0], false);
    answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
    assert.equal(answers[0].customSelected, true);
    assert.deepEqual(answers[0].selected, []);
    assert.equal(isQuestionAnswered(answers[0]), false);
    assert.equal(getNavigationState(0, request.questions, answers).canGoNext, false);
    assert.equal(getNavigationState(1, request.questions, answers).canSubmit, false);
  }
});

run('其他与普通单选互斥且切回其他保留草稿', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = setAnswerCustom(answers[0], '  自定义草稿  ');
  answers[0] = toggleAnswerSelection(answers[0], 'add-feature', false);
  assert.equal(answers[0].customSelected, false);
  assert.equal(answers[0].custom, '  自定义草稿  ');
  assert.deepEqual(buildQuestionAnswerPayload(request, request.questions, answers).answers[0], {
    question_id: 'q1',
    selected: ['add-feature'],
  });

  answers[0] = selectAnswerCustom(answers[0], false);
  assert.equal(answers[0].customSelected, true);
  assert.deepEqual(answers[0].selected, []);
  assert.equal(isQuestionAnswered(answers[0]), true);
  assert.deepEqual(buildQuestionAnswerPayload(request, request.questions, answers).answers[0], {
    question_id: 'q1',
    selected: [],
    custom_text: '自定义草稿',
  });
});

run('未选中的自定义草稿不算回答也不进入 payload', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0].custom = '尚未选中的草稿';
  assert.equal(isQuestionAnswered(answers[0]), false);
  assert.equal(getNavigationState(0, request.questions, answers).canGoNext, false);
  assert.deepEqual(buildQuestionAnswerPayload(request, request.questions, answers).answers[0], {
    question_id: 'q1',
    selected: [],
  });
});

run('清空其他文本后仍选中其他但不能借旧普通选项推进', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'fix-bug', false);
  answers[0] = setAnswerCustom(answers[0], '其他内容');
  answers[0] = setAnswerCustom(answers[0], '');
  assert.equal(answers[0].customSelected, true);
  assert.deepEqual(answers[0].selected, []);
  assert.equal(isQuestionAnswered(answers[0]), false);
  assert.equal(getNavigationState(0, request.questions, answers).canGoNext, false);
});

run('多选可组合普通选项和其他也可仅提交其他', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  answers[1] = selectAnswerCustom(answers[1], true);
  assert.deepEqual(answers[1].selected, ['tests']);
  answers[1] = setAnswerCustom(answers[1], '  性能检查  ', true);
  answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
  assert.equal(answers[1].customSelected, true);
  assert.deepEqual(buildQuestionAnswerPayload(request, request.questions, answers).answers[1], {
    question_id: 'q2',
    selected: ['tests', 'docs'],
    custom_text: '性能检查',
  });

  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
  assert.equal(isQuestionAnswered(answers[1]), true);
  assert.deepEqual(buildQuestionAnswerPayload(request, request.questions, answers).answers[1], {
    question_id: 'q2',
    selected: [],
    custom_text: '性能检查',
  });
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
