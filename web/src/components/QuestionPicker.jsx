// AskUserQuestion 内联 picker:停靠在输入框上方,不使用全屏 modal。
// 支持单选 / 多选 / 多题分页 / 键盘操作。
// AskUserQuestion 双入口(ask-user-question-dual-entry,方案 C · 主输入区):
//   - 「以上都不是」= 列表末行(勾选 + 行内输入),暖色强调独占;勾选即清空预设
//   - 「补充说明」= 题目下方常驻输入区,无勾选框,打字即生效(非空即激活)
//   - 互斥:勾「以上都不是」→ 补充区置灰停用(文本保留);点回置灰框输入 →
//     自动恢复并取消「以上都不是」
//   - 校验:离开当前题时(下一步/提交)独占激活但文本为空 → 就地拦截报错
// 状态模型与纯逻辑见 lib/questionPicker.js(其单测覆盖全部状态迁移)。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { connection } from '../lib/connection.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import {
  buildQuestionAnswerPayload,
  buildQuestionCancelPayload,
  getNavigationState,
  hasSelectedTextWithin,
  isQuestionAnswered,
  makeInitialAnswers,
  normalizeQuestionRequest,
  setExclusiveText,
  setSupplement,
  toggleAnswerSelection,
  toggleExclusive,
  validateAnswerCompleteness,
} from '../lib/questionPicker.js';

const READABLE_TEXT_STYLE = { overflowWrap: 'anywhere', wordBreak: 'break-word' };
const SELECTABLE_OPTION_STYLE = {
  WebkitUserSelect: 'text',
  userSelect: 'text',
};

const VALIDATION_MESSAGES = {
  exclusive_empty: '「以上都不是」需要填写内容后才能继续',
};

function focusSoon(ref) {
  requestAnimationFrame(() => ref.current?.focus());
}

export function QuestionPicker({ request, onResolve, originLabel = '' }) {
  const normalized = useMemo(() => normalizeQuestionRequest(request), [request]);
  const { questions } = normalized;
  const [answers, setAnswers] = useState(() => makeInitialAnswers(questions));
  const [currentIndex, setCurrentIndex] = useState(0);
  const [focusIndex, setFocusIndex] = useState(0);
  const [collapsed, setCollapsed] = useState(false);
  const [validationError, setValidationError] = useState(null);
  const rootRef = useRef(null);
  const exclusiveInputRef = useRef(null);
  const supplementRef = useRef(null);

  useEffect(() => {
    setAnswers(makeInitialAnswers(questions));
    setCurrentIndex(0);
    setFocusIndex(0);
    setCollapsed(false);
    setValidationError(null);
    focusSoon(rootRef);
  }, [normalized.requestId, questions]);

  const question = questions[currentIndex];
  const answer = answers[currentIndex] || makeInitialAnswers([{}])[0];
  const optionCount = question?.options?.length || 0;
  const exclusiveRowIndex = optionCount; // 预设后追加的「以上都不是」行
  const nav = getNavigationState(currentIndex, questions, answers);

  const updateAnswer = useCallback((index, updater) => {
    setAnswers((prev) => prev.map((item, i) => i === index ? updater(item) : item));
  }, []);

  const resolve = useCallback(() => {
    onResolve?.();
  }, [onResolve]);

  const cancel = useCallback(() => {
    connection.sendQuestionAnswer(buildQuestionCancelPayload(normalized));
    resolve();
  }, [normalized, resolve]);

  // 离开当前题强校验(grill Q1):失败则就地拦截并定位到空输入框。
  const ensureCurrentComplete = useCallback((index, focusOnError) => {
    const check = validateAnswerCompleteness(answers[index] || {});
    if (!check.ok) {
      setValidationError(check.reason);
      if (focusOnError && check.reason === 'exclusive_empty') {
        focusSoon(exclusiveInputRef);
      }
      return false;
    }
    setValidationError(null);
    return true;
  }, [answers]);

  const submit = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (!state.canSubmit) return;
    if (!ensureCurrentComplete(currentIndex, true)) return;
    connection.sendQuestionAnswer(buildQuestionAnswerPayload(normalized, questions, answers));
    resolve();
  }, [answers, currentIndex, ensureCurrentComplete, normalized, questions, resolve]);

  const goPrev = useCallback(() => {
    setCurrentIndex((value) => Math.max(0, value - 1));
    setFocusIndex(0);
    setValidationError(null);
    focusSoon(rootRef);
  }, []);

  const goNext = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (!state.canGoNext) return;
    if (!ensureCurrentComplete(currentIndex, true)) return;
    setCurrentIndex((value) => Math.min(questions.length - 1, value + 1));
    setFocusIndex(0);
    focusSoon(rootRef);
  }, [answers, currentIndex, ensureCurrentComplete, questions]);

  const primaryAction = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (state.isLast) submit();
    else goNext();
  }, [answers, currentIndex, goNext, questions, submit]);

  const selectOption = useCallback((optionIndex) => {
    const opt = question?.options?.[optionIndex];
    if (!opt) return;
    setFocusIndex(optionIndex);
    setValidationError(null);
    // 切换预设会退出独占态(纯函数内处理)。
    updateAnswer(currentIndex, (item) => toggleAnswerSelection(item, opt.value, !!question.multiSelect));
  }, [currentIndex, question, updateAnswer]);

  const onToggleExclusive = useCallback(() => {
    setFocusIndex(exclusiveRowIndex);
    setValidationError(null);
    updateAnswer(currentIndex, (item) => toggleExclusive(item));
  }, [currentIndex, exclusiveRowIndex, updateAnswer]);

  const onExclusiveText = useCallback((value) => {
    updateAnswer(currentIndex, (item) => setExclusiveText(item, value));
  }, [currentIndex, updateAnswer]);

  const onSupplement = useCallback((value) => {
    updateAnswer(currentIndex, (item) => setSupplement(item, value));
  }, [currentIndex, updateAnswer]);

  const moveFocus = useCallback((delta) => {
    // 可聚焦行 = 预设 + 「以上都不是」行(补充说明为常驻区,不参与箭头焦点)
    const count = optionCount + 1;
    setFocusIndex((value) => Math.min(count - 1, Math.max(0, value + delta)));
  }, [optionCount]);

  const onKeyDown = useCallback((event) => {
    if (!question) return;
    const target = event.target;
    const tag = target?.tagName;
    const inTextInput = tag === 'INPUT' || tag === 'TEXTAREA';

    if (event.key === 'Escape') {
      event.preventDefault();
      if (inTextInput) {
        target.blur();
        setValidationError(null);
        focusSoon(rootRef);
      } else {
        cancel();
      }
      return;
    }

    if (inTextInput) {
      if (event.key === 'Enter') {
        event.preventDefault();
        primaryAction();
      }
      return;
    }

    if (/^[1-9]$/.test(event.key)) {
      const index = Number(event.key) - 1;
      if (index < optionCount) {
        event.preventDefault();
        selectOption(index);
      }
      return;
    }

    if (event.key === 'ArrowDown') {
      event.preventDefault();
      moveFocus(1);
      return;
    }
    if (event.key === 'ArrowUp') {
      event.preventDefault();
      moveFocus(-1);
      return;
    }
    if (event.key === ' ') {
      event.preventDefault();
      if (focusIndex < optionCount) selectOption(focusIndex);
      else if (focusIndex === exclusiveRowIndex) onToggleExclusive();
      return;
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      if (nav.currentAnswered) {
        primaryAction();
      } else if (focusIndex < optionCount) {
        selectOption(focusIndex);
      } else if (focusIndex === exclusiveRowIndex) {
        focusSoon(exclusiveInputRef);
      } else {
        supplementRef.current?.focus();
      }
    }
  }, [cancel, exclusiveRowIndex, focusIndex, moveFocus, nav.currentAnswered, onToggleExclusive, optionCount, primaryAction, question, selectOption]);

  if (!question) return null;

  const exclusiveSelected = !!answer.exclusiveActive;
  const supplementDisabled = exclusiveSelected; // 独占激活时补充区置灰停用
  const exclusiveHasText = (answer.exclusiveText || '').trim().length > 0;
  const supplementHasText = (answer.supplement || '').trim().length > 0;
  const showExclusiveError = validationError === 'exclusive_empty';

  return (
    <section
      ref={rootRef}
      tabIndex={-1}
      onKeyDown={onKeyDown}
      aria-label="AskUserQuestion"
      className="mx-2.5 mb-2 shrink min-h-0 rounded-xl border border-border bg-surface ace-shadow-lg outline-none overflow-hidden flex flex-col"
    >
      <div className="min-h-10 shrink-0 px-3 py-2 border-b border-border bg-surface-alt flex items-start gap-2">
        <div className="min-w-0 flex-1">
          {originLabel && (
            <div className="text-[10px] text-fg-mute mb-0.5 truncate" title={originLabel}>
              {originLabel}
            </div>
          )}
          {question.header && (
            <div
              className="text-[10px] uppercase tracking-wide text-accent font-semibold whitespace-pre-wrap break-words"
              style={READABLE_TEXT_STYLE}
            >
              {question.header}
            </div>
          )}
          <div
            className="text-[13px] leading-[17px] font-semibold text-fg whitespace-pre-wrap break-words"
            style={READABLE_TEXT_STYLE}
          >
            {question.text}
          </div>
        </div>
        <button
          type="button"
          onClick={() => setCollapsed((value) => !value)}
          className="mt-0.5 w-7 h-7 shrink-0 rounded-md flex items-center justify-center text-fg-2 hover:bg-surface-hi transition"
          title={collapsed ? '展开' : '折叠'}
        >
          <VsIcon name={collapsed ? 'expandRight' : 'expandDown'} size={14} />
        </button>
        <button
          type="button"
          onClick={cancel}
          className="mt-0.5 w-7 h-7 shrink-0 rounded-md flex items-center justify-center text-fg-2 hover:bg-danger-bg hover:text-danger transition"
          title="取消回答"
        >
          <VsIcon name="close" size={14} />
        </button>
      </div>

      {collapsed ? (
        <div className="shrink-0 px-3 py-2 text-[12px] text-fg-2 flex items-center justify-between gap-3">
          <span className="truncate">已折叠,继续等待回答。</span>
          <span className="shrink-0 text-fg-mute">{nav.current}/{nav.total}</span>
        </div>
      ) : (
        <>
          <div className="p-2.5 min-h-0 flex-1 overflow-y-auto flex flex-col gap-1.5">
            {question.options.map((opt, index) => {
              const selected = answer.selected?.includes(opt.value);
              const focused = focusIndex === index;
              return (
                <button
                  key={`${opt.value}-${index}`}
                  type="button"
                  onClick={(event) => {
                    if (event.detail > 0 && hasSelectedTextWithin(event.currentTarget, window.getSelection())) return;
                    selectOption(index);
                  }}
                  onFocus={() => setFocusIndex(index)}
                  aria-pressed={selected}
                  style={SELECTABLE_OPTION_STYLE}
                  className={clsx(
                    'w-full text-left rounded-lg border px-2.5 py-2 flex items-start gap-2 transition outline-none',
                    selected
                      ? 'bg-accent-bg border-accent text-fg'
                      : 'bg-surface-alt border-border hover:bg-surface-hi',
                    focused && 'ring-2 ring-accent/20 border-accent',
                    // 独占激活时预设选项整体弱化(所见即所得:已清空)
                    exclusiveSelected && 'opacity-50',
                  )}
                >
                  <span className="w-5 shrink-0 pt-0.5 text-[12px] font-semibold text-fg-mute tabular-nums">
                    {index + 1}
                  </span>
                  <span className="min-w-0 flex-1">
                    <span
                      className="block text-[12px] font-medium text-fg whitespace-pre-wrap break-words"
                      style={READABLE_TEXT_STYLE}
                    >
                      {opt.label}
                    </span>
                    {opt.description && (
                      <span
                        className="block mt-0.5 text-[11px] leading-[15px] text-fg-mute whitespace-pre-wrap break-words"
                        style={READABLE_TEXT_STYLE}
                      >
                        {opt.description}
                      </span>
                    )}
                  </span>
                  <span className="w-5 h-5 shrink-0 mt-0.5 flex items-center justify-center">
                    {selected ? (
                      <VsIcon name="ok" size={14} mono={false} />
                    ) : (
                      <span className="w-3.5 h-3.5 rounded-full border border-border" />
                    )}
                  </span>
                </button>
              );
            })}

            {/* 「以上都不是」独占行(方案 C:列表末行,勾选 + 行内输入,暖色) */}
            <div
              role="checkbox"
              aria-checked={exclusiveSelected}
              tabIndex={0}
              onClick={onToggleExclusive}
              onKeyDown={(event) => {
                if (event.key === 'Enter' || event.key === ' ') {
                  event.preventDefault();
                  onToggleExclusive();
                }
              }}
              onFocus={() => setFocusIndex(exclusiveRowIndex)}
              className={clsx(
                'w-full rounded-lg border px-2.5 py-2 flex items-start gap-2 transition cursor-pointer outline-none',
                exclusiveSelected
                  ? 'bg-danger-bg border-danger'
                  : 'bg-surface-alt border-border hover:bg-surface-hi',
                focusIndex === exclusiveRowIndex && !exclusiveSelected && 'ring-2 ring-danger/20 border-danger',
                showExclusiveError && 'border-danger ring-2 ring-danger/25',
              )}
            >
              <span className="w-5 shrink-0 pt-0.5 mt-1 flex items-center justify-center">
                {exclusiveSelected ? (
                  <VsIcon name="ok" size={14} mono={false} />
                ) : (
                  <span className="w-3.5 h-3.5 rounded-full border border-border" />
                )}
              </span>
              <span className="min-w-0 flex-1 flex flex-col gap-1">
                <span className={clsx('text-[12px] font-medium', exclusiveSelected ? 'text-danger' : 'text-fg')}>
                  以上都不是
                </span>
                <span className="text-[11px] text-fg-mute">选此项将取消以上全部选择</span>
                <input
                  ref={exclusiveInputRef}
                  type="text"
                  value={answer.exclusiveText || ''}
                  onClick={(event) => event.stopPropagation()}
                  onFocus={(event) => {
                    event.stopPropagation();
                    setFocusIndex(exclusiveRowIndex);
                    setValidationError(null);
                  }}
                  onChange={(event) => onExclusiveText(event.target.value)}
                  placeholder={exclusiveSelected ? '填写你的答案(必填)' : '输入自定义答案'}
                  disabled={!exclusiveSelected}
                  className={clsx(
                    'h-7 w-full rounded-md border bg-surface px-2 text-[12px] text-fg outline-none',
                    exclusiveSelected
                      ? showExclusiveError
                        ? 'border-danger text-danger placeholder-danger/60'
                        : 'border-danger/50 focus:border-danger'
                      : 'border-border opacity-60',
                    'disabled:cursor-not-allowed',
                  )}
                />
              </span>
              <span className="w-5 h-5 shrink-0 mt-0.5 flex items-center justify-center">
                {exclusiveSelected ? (
                  <span className="text-[11px] text-danger font-medium">独占</span>
                ) : (
                  <span className="w-3.5 h-3.5 rounded-full border border-border" />
                )}
              </span>
            </div>

            {/* 分隔线 + 「补充说明」常驻输入区(方案 C:打字即生效,无勾选框) */}
            <div className="border-t border-border my-1" />
            <div className="flex flex-col gap-1 px-0.5">
              <div className="flex items-baseline gap-2">
                <span className="text-[12px] font-medium text-fg">补充说明</span>
                <span className="text-[11px] text-fg-mute">可选,随答案一起提交</span>
                {supplementDisabled && supplementHasText && (
                  <span className="ml-auto text-[10px] text-fg-mute shrink-0">
                    已停用 · 内容保留,不提交
                  </span>
                )}
              </div>
              <textarea
                ref={supplementRef}
                rows={2}
                value={answer.supplement || ''}
                disabled={supplementDisabled}
                onChange={(event) => onSupplement(event.target.value)}
                placeholder={supplementDisabled ? '已停用(勾选「以上都不是」时补充不随答案提交)' : '补充预设之外的说明(可选)'}
                className={clsx(
                  'w-full resize-none rounded-lg border bg-surface px-2.5 py-1.5 text-[12px] leading-[16px] text-fg outline-none focus:border-accent placeholder:text-fg-mute',
                  supplementHasText && !supplementDisabled
                    ? 'border-accent/60'
                    : 'border-border',
                  supplementDisabled && 'opacity-50 disabled:cursor-not-allowed',
                )}
              />
            </div>

            {validationError && (
              <div className="px-1 text-[11px] text-danger" role="alert">
                {VALIDATION_MESSAGES[validationError] || '请完善当前问题的回答'}
              </div>
            )}
          </div>

          <div className="shrink-0 px-3 py-2 border-t border-border bg-surface-alt flex items-center gap-2">
            <button
              type="button"
              onClick={goPrev}
              disabled={!nav.canGoPrev}
              className="w-7 h-7 rounded-md border border-border bg-surface text-[13px] text-fg-2 disabled:opacity-40 disabled:cursor-not-allowed hover:bg-surface-hi transition"
              title="上一题"
            >
              &lt;
            </button>
            <div className="text-[12px] text-fg-mute tabular-nums min-w-10 text-center">
              {nav.current}/{nav.total}
            </div>
            <button
              type="button"
              onClick={goNext}
              disabled={!nav.canGoNext}
              className="w-7 h-7 rounded-md border border-border bg-surface text-[13px] text-fg-2 disabled:opacity-40 disabled:cursor-not-allowed hover:bg-surface-hi transition"
              title="下一题"
            >
              &gt;
            </button>
            <div className="flex-1" />
            <span className="hidden sm:inline text-[11px] text-fg-mute">
              数字选择 · Enter 确认 · Esc 取消
            </span>
            <button
              type="button"
              onClick={primaryAction}
              disabled={nav.isLast ? !nav.canSubmit : !nav.canGoNext}
              className="px-3 h-7 rounded-md bg-accent text-white text-[12px] font-medium disabled:opacity-50 disabled:cursor-not-allowed hover:opacity-90 transition"
            >
              {nav.isLast ? '提交' : '下一题'}
            </button>
          </div>
        </>
      )}
    </section>
  );
}
