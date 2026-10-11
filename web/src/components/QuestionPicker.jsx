import { isDismissShortcut } from '../lib/shortcutBindings.js';
// AskUserQuestion 内联 picker:停靠在输入框位置(替换 composer 输入区)。
// 支持单选 / 多选 / 自定义答案 / 多题导航 / 折叠 / 复制 / Enter 快捷键。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { connection } from '../lib/connection.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import {
  buildQuestionAnswerPayload,
  buildQuestionCancelPayload,
  getNavigationState,
  hasSelectedTextWithin,
  makeInitialAnswers,
  normalizeQuestionRequest,
  recommendedOptionIndex,
  selectAnswerCustom,
  setAnswerCustom,
  toggleAnswerSelection,
  unselectAnswerCustom,
} from '../lib/questionPicker.js';

const READABLE_TEXT_STYLE = { overflowWrap: 'anywhere', wordBreak: 'break-word' };
const SELECTABLE_OPTION_STYLE = {
  WebkitUserSelect: 'text',
  userSelect: 'text',
};
const MAX_CUSTOM_LENGTH = 500;
const COPY_FEEDBACK_MS = 1500;
// Esc 一次取消所有选中;在此窗口内再次按 Esc 才视为拒绝作答(取消整个问答)。
const ESC_ARM_WINDOW_MS = 1200;

function focusSoon(ref) {
  requestAnimationFrame(() => ref.current?.focus?.());
}

async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    try {
      const el = document.createElement('textarea');
      el.value = text;
      el.style.position = 'fixed';
      el.style.opacity = '0';
      document.body.appendChild(el);
      el.select();
      document.execCommand('copy');
      document.body.removeChild(el);
      return true;
    } catch {
      return false;
    }
  }
}

export function QuestionPicker({ request, onResolve, originLabel = '', className = '' }) {

  const normalized = useMemo(() => normalizeQuestionRequest(request), [request]);
  const { questions } = normalized;
  const [answers, setAnswers] = useState(() => makeInitialAnswers(questions));
  const [currentIndex, setCurrentIndex] = useState(0);
  const [focusIndex, setFocusIndex] = useState(-1);
  const [hoverIndex, setHoverIndex] = useState(-1);
  const [collapsed, setCollapsed] = useState(false);
  const [copiedIndex, setCopiedIndex] = useState(-1);
  const [editingCustom, setEditingCustom] = useState(false);
  const rootRef = useRef(null);
  const customRef = useRef(null);
  const copiedTimerRef = useRef(null);
  // 记录 Esc「取消选中」与「拒绝作答」之间的连按窗口。
  const escTimerRef = useRef(null);
  // 记录上一次执行重置的请求 id:只有切换到新请求才重置;
  // 同一请求因重渲染产生新的 questions 引用时,不清空答案、不跳回第一题。
  const lastResetRequestIdRef = useRef(null);

  useEffect(() => {
    // requestId 未变化(首次挂载除外)说明是同一请求的重渲染,保留已答内容。
    if (lastResetRequestIdRef.current === normalized.requestId) return;
    lastResetRequestIdRef.current = normalized.requestId;
    setAnswers(makeInitialAnswers(questions));
    setCurrentIndex(0);
    setFocusIndex(-1);
    setHoverIndex(-1);
    setCollapsed(false);
    setEditingCustom(false);
    if (escTimerRef.current) clearTimeout(escTimerRef.current);
    escTimerRef.current = null;
    focusSoon(rootRef);
  }, [normalized.requestId, questions]);

  useEffect(() => () => {
    if (copiedTimerRef.current) clearTimeout(copiedTimerRef.current);
    if (escTimerRef.current) clearTimeout(escTimerRef.current);
  }, []);

  const question = questions[currentIndex];
  const answer = answers[currentIndex] || { selected: [], custom: '', customSelected: false, skipped: false };
  const optionCount = question?.options?.length || 0;
  const customIndex = optionCount;
  const nav = getNavigationState(currentIndex, questions, answers);
  const isMulti = !!question?.multiSelect;
  const activeOptionIndex = hoverIndex >= 0 ? hoverIndex
    : (focusIndex >= 0 ? focusIndex : recommendedOptionIndex(question));

  const updateAnswer = useCallback((index, updater) => {
    setAnswers((prev) => prev.map((item, i) => i === index ? updater(item) : item));
  }, []);

  const resolve = useCallback(() => {
    onResolve?.();
  }, [onResolve]);

  // Esc 一次:取消所有问题的选中态(清空已选选项与自定义激活态),保留自定义草稿文字。
  const resetAllSelections = useCallback(() => {
    setAnswers((prev) => prev.map((a) => ({ ...a, selected: [], customSelected: false })));
    setEditingCustom(false);
    focusSoon(rootRef);
  }, []);

  const cancel = useCallback(() => {
    connection.sendQuestionAnswer(buildQuestionCancelPayload(normalized));
    resolve();
  }, [normalized, resolve]);

  const submitCurrent = useCallback((i) => {
    setCurrentIndex(Math.min(questions.length - 1, i + 1));
    setFocusIndex(-1);
    setHoverIndex(-1);
    focusSoon(rootRef);
  }, [questions.length]);

  const submitAll = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (!state.canSubmit) return;
    connection.sendQuestionAnswer(buildQuestionAnswerPayload(normalized, questions, answers));
    resolve();
  }, [answers, currentIndex, normalized, questions, resolve]);

  const goPrev = useCallback(() => {
    setCurrentIndex((value) => Math.max(0, value - 1));
    setFocusIndex(-1);
    setHoverIndex(-1);
    focusSoon(rootRef);
  }, []);

  const goNext = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (state.isLast) return;
    // 向后切换 = 进入下一题;当前题未作答时自动记 Not answered(跳过)。
    setAnswers((prev) => prev.map((item, i) =>
      i === currentIndex && !state.currentAnswered ? { ...item, skipped: true } : item));
    setCurrentIndex((value) => Math.min(questions.length - 1, value + 1));
    setFocusIndex(-1);
    setHoverIndex(-1);
    focusSoon(rootRef);
  }, [answers, currentIndex, questions]);

  // 主操作:非末题在本地推进到下一题(daemon 为 first-wins,只允许末题统一收卷,
  // 中途不可发送 question_answer,否则会提前关闭整个请求);末题提交全部。
  const primaryAction = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (state.isLast) submitAll();
    else goNext();
  }, [currentIndex, questions, answers, submitAll, goNext]);

  const skipCurrent = useCallback(() => {
    if (!nav.canSkip) return;
    setAnswers((prev) => prev.map((item, i) => i === currentIndex ? { ...item, skipped: true } : item));
    submitCurrent(currentIndex);
  }, [currentIndex, nav.canSkip, submitCurrent]);

  const selectOption = useCallback((optionIndex) => {
    const opt = question?.options?.[optionIndex];
    if (!opt) return;
    setFocusIndex(optionIndex);
    updateAnswer(currentIndex, (item) => toggleAnswerSelection(item, opt.value, isMulti));
  }, [currentIndex, isMulti, question, updateAnswer]);

  // Enter 一键:单选/多选选中当前焦点项并进入下一题;末题只选中(需 Ctrl+Enter 提交)。
  const commitEnter = useCallback((optionIndex) => {
    const opt = question?.options?.[optionIndex];
    if (!opt) return;
    setFocusIndex(optionIndex);
    // 确认操作保留已选答案;只有单击选项或 Space 才切换多选状态。
    updateAnswer(currentIndex, (item) => isMulti && item.selected.includes(opt.value)
      ? item : toggleAnswerSelection(item, opt.value, isMulti));
    if (currentIndex < questions.length - 1) {
      submitCurrent(currentIndex);
    }
  }, [currentIndex, isMulti, question, questions.length, submitCurrent, updateAnswer]);

  const selectCustom = useCallback(() => {
    setFocusIndex(customIndex);
    setEditingCustom(true);
    updateAnswer(currentIndex, (item) => selectAnswerCustom(item, isMulti));
  }, [customIndex, currentIndex, isMulti, updateAnswer]);

  const setCustom = useCallback((value) => {
    const next = value.slice(0, MAX_CUSTOM_LENGTH);
    updateAnswer(currentIndex, (item) => setAnswerCustom(item, next, isMulti));
  }, [currentIndex, isMulti, updateAnswer]);

  const toggleCustom = useCallback(() => {
    updateAnswer(currentIndex, (item) => item.customSelected
      ? unselectAnswerCustom(item)
      : selectAnswerCustom(item, isMulti));
    setFocusIndex(customIndex);
    setEditingCustom(false);
    focusSoon(rootRef);
  }, [currentIndex, customIndex, isMulti, updateAnswer]);

  const moveFocus = useCallback((delta) => {
    const count = optionCount + 1;
    setFocusIndex(Math.min(count - 1, Math.max(0, activeOptionIndex + delta)));
    setHoverIndex(-1);
  }, [activeOptionIndex, optionCount]);

  const copyOption = useCallback(async (optionIndex, event) => {
    if (event) event.stopPropagation();
    const opt = question?.options?.[optionIndex];
    if (!opt) return;
    const text = opt.description ? `${opt.label} — ${opt.description}` : opt.label;
    await copyText(text);
    setCopiedIndex(optionIndex);
    if (copiedTimerRef.current) clearTimeout(copiedTimerRef.current);
    copiedTimerRef.current = setTimeout(() => setCopiedIndex(-1), COPY_FEEDBACK_MS);
  }, [question]);

  const onKeyDown = useCallback((event) => {
    if (!question) return;
    // 输入法的确认/取消按键只交给输入法,不能收卷或改变题目。
    if (event.isComposing || event.nativeEvent?.isComposing || event.keyCode === 229) return;
    const target = event.target;
    const tag = target?.tagName;
    const inTextInput = tag === 'INPUT' || tag === 'TEXTAREA';
    const withCtrl = event.ctrlKey || event.metaKey;
    const ctrlEnter = withCtrl && event.key === 'Enter';

    if (isDismissShortcut(event)) {
      event.preventDefault();
      if (event.repeat) return;
      if (inTextInput) {
        // 自定义输入框内:先退出编辑态,不参与连按判定。
        setEditingCustom(false);
        focusSoon(rootRef);
        return;
      }
      // 连按窗口内再按 Esc = 拒绝回答问题(取消整个问答)。
      if (escTimerRef.current) {
        clearTimeout(escTimerRef.current);
        escTimerRef.current = null;
        cancel();
        return;
      }
      // 第一次 Esc:取消所有问题选中,并开启连按窗口。
      resetAllSelections();
      escTimerRef.current = setTimeout(() => {
        escTimerRef.current = null;
      }, ESC_ARM_WINDOW_MS);
      return;
    }

    if (collapsed) return;

    if (ctrlEnter) {
      event.preventDefault();
      if (nav.isLast) submitAll();
      return;
    }

    if (inTextInput) {
      if (event.key === 'Enter' && !event.shiftKey) {
        event.preventDefault();
        primaryAction();
      }
      return;
    }

    if (withCtrl || event.altKey) return;
    // 复制/取消/展开等原生按钮保留 Enter 和 Space 的激活行为。
    if ((event.key === 'Enter' || event.key === ' ') && target?.closest?.('button')) return;

    if (event.key === 'Tab') {
      event.preventDefault();
      if (event.shiftKey) goPrev();
      else goNext();
      return;
    }

    if (/^[1-9]$/.test(event.key)) {
      const index = Number(event.key) - 1;
      if (index <= optionCount) {
        event.preventDefault();
        if (index < optionCount) commitEnter(index);
        else {
          setEditingCustom(true);
          customRef.current?.focus();
        }
      }
      return;
    }

    if (event.key === 'ArrowRight') {
      event.preventDefault();
      goNext();
      return;
    }
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      goPrev();
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
      if (activeOptionIndex >= 0 && activeOptionIndex < optionCount) selectOption(activeOptionIndex);
      else {
        setEditingCustom(true);
        customRef.current?.focus();
      }
      return;
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      if (activeOptionIndex >= 0 && activeOptionIndex < optionCount) commitEnter(activeOptionIndex);
      else customRef.current?.focus();
    }
  }, [activeOptionIndex, cancel, collapsed, commitEnter, goNext, goPrev, moveFocus, nav.isLast, optionCount, primaryAction, question, resetAllSelections, selectOption, submitAll]);

  if (!question) return null;

  const countdownLabel = `${nav.current} / ${nav.total}`;
  const collapsedHint = `${nav.total} 个问题待回答`;
  const primaryLabel = nav.isLast ? '提交' : (nav.currentAnswered ? '提交' : '跳过');
  const primaryBtnLabel = primaryLabel === '提交'
    ? (nav.isLast ? '提交' : '提交')
    : '跳过';
  const primaryKeyHint = nav.isLast ? 'Ctrl + Enter' : 'Enter';
  const customActive = !!answer.customSelected && answer.custom.trim().length > 0;
  // 草稿态:输入了自定义内容但未选中自定义项(例如单选时改选预设选项),文字变灰。
  const customDraft = !answer.customSelected && (answer.custom || '').trim().length > 0;

  return (
    <section
      ref={rootRef}
      tabIndex={-1}
      onKeyDown={onKeyDown}
      aria-label="AskUserQuestion"
      className={clsx('mb-2 shrink min-h-0 rounded-[14px] border border-border bg-surface ace-shadow-lg outline-none overflow-hidden flex flex-col', className)}
    >
      <div className="min-h-11 shrink-0 px-3.5 py-2.5 border-b border-border bg-surface flex items-center gap-2">
        <div className="min-w-0 flex-1 overflow-hidden">
          {originLabel && (
            <div className="text-[11px] text-fg-mute mb-0.5 truncate" title={originLabel}>
              {originLabel}
            </div>
          )}
          {!collapsed && (
            <div
              className="text-[14px] font-semibold leading-normal text-fg whitespace-pre-wrap break-words"
              style={READABLE_TEXT_STYLE}
            >
              {question.text}
              {isMulti && <span className="text-fg-mute text-[12px] font-normal ml-1.5">(可多选)</span>}
            </div>
          )}
          {collapsed && (
            <div className="truncate text-[13px] font-medium text-fg-mute">{collapsedHint}</div>
          )}
        </div>

        {!collapsed && (
          <button
            type="button"
            onClick={goPrev}
            disabled={!nav.canGoPrev}
            className="w-8 h-8 shrink-0 rounded-md flex items-center justify-center text-fg-2 hover:bg-surface-hi disabled:opacity-40 disabled:cursor-not-allowed transition"
            title="上一题 (Shift+Tab)"
            aria-label="上一题"
          >
            <VsIcon name="arrowLeft" size={14} />
          </button>
        )}
        <div className={clsx('shrink-0 text-[12px] text-fg-mute tabular-nums', collapsed && 'ml-1')}>
          {collapsed ? countdownLabel : countdownLabel}
        </div>
        {!collapsed && (
          <button
            type="button"
            onClick={goNext}
            disabled={nav.isLast}
            className="w-8 h-8 shrink-0 rounded-md flex items-center justify-center text-fg-2 hover:bg-surface-hi disabled:opacity-40 disabled:cursor-not-allowed transition"
            title="下一题 (Tab)"
            aria-label="下一题"
          >
            <VsIcon name="arrowRight" size={14} />
          </button>
        )}
        <button
          type="button"
          onClick={() => setCollapsed((value) => !value)}
          className="w-8 h-8 shrink-0 rounded-md flex items-center justify-center text-fg-2 hover:bg-surface-hi transition"
          title={collapsed ? '展开' : '折叠'}
          aria-label={collapsed ? '展开' : '折叠'}
        >
          <VsIcon name={collapsed ? 'expandUp' : 'expandDown'} size={14} />
        </button>
      </div>

      {collapsed ? null : (
        <>
          <div className="px-2 py-2 min-h-0 flex-1 overflow-y-auto ace-scrollbar">
            {question.options.map((opt, index) => {
              const selected = answer.selected?.includes(opt.value);
              const focused = focusIndex === index;
              const hovered = hoverIndex === index;
              const copied = copiedIndex === index;
              return (
                <div
                  key={`${opt.value}-${index}`}
                  onMouseEnter={() => setHoverIndex(index)}
                  onMouseLeave={() => setHoverIndex(-1)}
                  onMouseDown={(event) => {
                    if (event.target?.closest?.('button')) return;
                    if (event.detail > 0 && event.detail >= 2) {
                      event.preventDefault();
                      commitEnter(index);
                    }
                  }}
                  onClick={(event) => {
                    if (event.detail > 0 && hasSelectedTextWithin(event.currentTarget, window.getSelection())) return;
                    if (event.detail >= 2) return;
                    selectOption(index);
                  }}
                  style={SELECTABLE_OPTION_STYLE}
                  className={clsx(
                    'group flex items-center gap-3 rounded-md px-3.5 py-3 cursor-pointer transition',
                    selected
                      ? 'bg-accent-bg border border-transparent text-accent'
                      : focused
                        ? 'bg-accent-bg border border-transparent'
                        : hovered
                          ? 'border border-transparent bg-accent-bg'
                          : 'border border-transparent hover:bg-accent-bg',

                  )}
                >
                  <span
                    className={clsx(
                      'w-5 h-5 shrink-0 rounded-full flex items-center justify-center border transition',
                      selected
                        ? 'bg-accent text-white border-accent'
                        : 'border-fg-mute text-fg-mute',
                    )}
                  >
                    {selected ? (
                      <VsIcon name="check" size={13} mono={false} />
                    ) : (
                      <span className="text-[11px] font-medium tabular-nums">{index + 1}</span>
                    )}
                  </span>
                  <span className="min-w-0 flex-1">
                    <span className="block text-[13px] font-medium leading-normal text-fg whitespace-pre-wrap break-words" style={READABLE_TEXT_STYLE}>
                      {opt.label}
                      {opt.recommended && (
                        <span className="ml-1.5 align-middle text-[11px] font-normal text-fg-mute border border-border rounded px-1 py-0.5">
                          [推荐]
                        </span>
                      )}
                    </span>
                    {opt.description && (
                      <span className="block mt-0.5 text-[11px] leading-normal text-fg-mute whitespace-pre-wrap break-words" style={READABLE_TEXT_STYLE}>
                        {opt.description}
                      </span>
                    )}
                  </span>
                  <span className="shrink-0 flex items-center gap-1 opacity-0 group-hover:opacity-100 transition">
                    <button
                      type="button"
                      onClick={(event) => copyOption(index, event)}
                      className={clsx(
                        'h-7 px-2 rounded-md text-[12px] border transition flex items-center gap-1',
                        copied
                          ? 'text-ok border-transparent'
                          : 'text-fg-2 border-border bg-surface hover:bg-surface-hi',
                      )}
                    >
                      {copied ? <VsIcon name="check" size={11} className="text-ok" /> : <>复制</>}
                    </button>
                    <button
                      type="button"
                      onClick={(event) => { event.stopPropagation(); commitEnter(index); }}
                      className="w-7 h-7 shrink-0 rounded-md flex items-center justify-center text-fg-mute hover:bg-surface-hi hover:text-fg-2 transition"
                      title="选择并进入下一题"
                      aria-label="选择并进入下一题"
                    >
                      <VsIcon name="arrowRight" size={13} />
                    </button>
                  </span>
                </div>
              );
            })}

            <div className="mx-3.5 my-1 border-t border-border" />

            <div
              className={clsx(
                'flex items-center gap-3 rounded-md px-3.5 py-3 transition',
                customActive
                  ? 'bg-accent-bg border border-transparent'
                  : focusIndex === customIndex
                    ? 'bg-accent-bg border border-transparent'
                    : 'border border-transparent hover:bg-accent-bg',

              )}
            >
              <button
                type="button"
                onClick={toggleCustom}
                className={clsx(
                  'w-5 h-5 shrink-0 rounded-full flex items-center justify-center border transition',
                  customActive
                    ? 'bg-accent text-white border-accent'
                    : 'border-fg-mute text-fg-mute',
                )}
                aria-label={customActive ? '取消自定义答案' : '选择自定义答案'}
              >
                {customActive ? (
                  <VsIcon name="check" size={13} mono={false} />
                ) : (
                  <span className="text-[11px] font-medium tabular-nums">{customIndex + 1}</span>
                )}
              </button>
              <input
                ref={customRef}
                type="text"
                value={answer.custom || ''}
                onFocus={selectCustom}
                onChange={(event) => setCustom(event.target.value)}
                onBlur={() => setEditingCustom(false)}
                placeholder="输入你的答案"
                maxLength={MAX_CUSTOM_LENGTH}
                className={clsx(
                  'min-w-0 flex-1 h-7 bg-transparent text-[13px] leading-normal outline-none placeholder:text-fg-mute',
                  customDraft ? 'text-fg-mute font-normal' : 'text-fg',
                )}

              />
              <span className="shrink-0 text-[11px] text-fg-mute tabular-nums">
                {(answer.custom || '').length}/{MAX_CUSTOM_LENGTH}
              </span>
            </div>
          </div>
          <div className="shrink-0 flex items-center justify-end gap-2 border-t border-border px-3.5 py-2.5">

              <button
                type="button"
                onClick={cancel}
                className="h-8 px-3 rounded-md border border-border text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt transition whitespace-nowrap"
              >
                取消
              </button>
              {nav.currentAnswered || nav.isLast ? (
                <button
                  type="button"
                  onClick={nav.isLast ? submitAll : (nav.currentAnswered ? () => submitCurrent(currentIndex) : undefined)}
                  className="h-8 px-3 rounded-md text-[12px] bg-accent text-white hover:opacity-90 transition flex items-center gap-1.5 whitespace-nowrap"
                >
                  {primaryBtnLabel}
                  <span className="ace-action-shortcut-hint">
                    {primaryKeyHint}
                  </span>
                </button>
              ) : (
                <button
                  type="button"
                  onClick={skipCurrent}
                  className="h-8 px-3 rounded-md border border-border text-[12px] text-fg-2 bg-surface-hi hover:bg-surface-alt transition whitespace-nowrap"
                >
                  跳过
                </button>
              )}
          </div>
        </>
      )}
    </section>
  );
}
