// 工具调用块:统一保留工具执行行，Shell 仅在展开详情时使用命令/输出卡片。
//   - 进度模式: tool_start 之后,显示 5-line tail + 状态行(行数/字节/已耗时)
//   - summary 模式: tool_end 后,绿/红 chip(icon · verb · object · metrics)
//   - 失败折叠: success=false 时 summary 行下显示前 3 行 stderr,可展开看完整 output
// 用户点 chip 可切换"展开/收起"。task_complete 用 Done: <summary> 渲染。
//
// hunks 字段(file_edit / file_write):新建文件直接展示可复制源码，覆盖和编辑
// 继续走 diff2html。其余普通文本展开内容统一套 ToolTextFrame。

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { clsx, formatBytes, formatCount, formatElapsed } from '../lib/format.js';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { compactOneLinePreview } from '../lib/compactMessagePreview.js';
import { createdFileSource } from '../lib/createdFileSource.js';
import { normalizeAttachmentList } from '../lib/messageAttachments.js';
import { renderMarkdown } from '../lib/markdown.js';
import { highlightSourceForFile } from '../lib/sourceCodeHighlight.js';
import { fallbackToolSummary } from '../lib/toolSummaryFallback.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { normalizeTaskCompleteMarkdown } from '../lib/taskCompleteSummary.js';
import { questionFeedbackForTool } from '../lib/questionFeedback.js';
import { shellCommandPresentation } from '../lib/shellCommandPresentation.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { AttachmentStrip } from './AttachmentStrip.jsx';
import { ActivityLine } from './ActivityLine.jsx';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { QuestionFeedbackCard } from './QuestionFeedbackCard.jsx';
import { ShellCommandCard } from './ShellCommandCard.jsx';
import { ToolSummaryIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';
import * as Diff2Html from 'diff2html';

function ToolTextFrame({ text = '', children = null }) {
  const copyText = String(text ?? '');
  if (!copyText && !children) return null;
  return (
    <CopyableCodeFrame
      text={copyText}
      className="ace-tool-output-frame overflow-hidden rounded-xl border border-border bg-surface"
      data-tool-output-frame="true"
    >
      {children || (
        <pre
          className="m-0 max-h-[280px] overflow-auto whitespace-pre-wrap break-words px-5 py-3 pr-12 font-mono text-fg-2"
          style={{ fontSize: 'var(--ace-font-size-code)', lineHeight: 1.55 }}
          data-code-copy-source="true"
        >
          {copyText}
        </pre>
      )}
    </CopyableCodeFrame>
  );
}

function CreatedFileFrame({ source }) {
  const highlighted = useMemo(
    () => highlightSourceForFile(source.path, source.content),
    [source.content, source.path],
  );
  const codeClassName = highlighted.language
    ? `hljs language-${highlighted.language}`
    : undefined;

  return (
    <CopyableCodeFrame
      text={source.content}
      className="ace-tool-created-code"
      data-created-file-source="true"
      data-code-lang={highlighted.language || undefined}
    >
      <pre data-code-copy-source="true">
        <code
          className={codeClassName}
          dangerouslySetInnerHTML={{ __html: highlighted.html }}
        />
      </pre>
    </CopyableCodeFrame>
  );
}

function MetricList({ metrics }) {
  if (!metrics || !metrics.length) return null;
  return (
    <span className="text-fg-mute shrink-0 whitespace-nowrap tabular-nums">
      {metrics.map((m, i) => {
        // file_edit / file_write 的加删行数(label 恒为 "+" / "-",见
        // src/tool/file_edit_tool.cpp)按 diff 惯例渲染成 +12 / -3 并着红绿;
        // 其余 metrics 保持 label=value 的通用格式。
        const label = String(m.label ?? '');
        if (label === '+' || label === '-') {
          return (
            <span key={i}>
              {' '}
              <span className={label === '+' ? 'ace-change-add' : 'ace-change-del'}>
                {label}{m.value}
              </span>
            </span>
          );
        }
        return (
          <span key={i}>
            {' · '}
            <span>{m.label}={m.value}</span>
          </span>
        );
      })}
    </span>
  );
}

function askUserQuestionText(result) {
  const items = Array.isArray(result?.items) ? result.items : [];
  return items
    .filter((item) => item && (item.question || item.answer))
    .map((item) => `Q ${item.question || ''}\nA ${item.answer || ''}`)
    .join('\n\n');
}

function askUserQuestionItems(result) {
  if (!Array.isArray(result?.items)) return [];
  return result.items
    .filter((item) => item && (item.question || item.answer || item.not_answered || item.notAnswered))
    .map((item) => {
      const answer = String(item.answer ?? '');
      const unanswered = item.not_answered === true
        || item.notAnswered === true
        || !answer.trim()
        || answer.trim() === 'Not answered';
      return {
        question: String(item.question ?? ''),
        answer: unanswered ? '' : answer,
        unanswered,
      };
    });
}

function AskUserQuestionDetails({ items }) {
  return (
    <div className="ace-qa-inline-list w-full min-w-0 max-w-[88%] pb-2 pl-5 pt-0.5">
      {items.map((item, index) => (
        <div key={index} className="ace-qa-inline-item py-1">
          <div className="ace-qa-question text-[12px] leading-snug text-fg-mute break-words">
            {index + 1}. {item.question}
          </div>
          <div className="ace-qa-answer mt-0.5 text-[13px] leading-snug break-words">
            {item.unanswered ? <span className="text-fg-mute">未作答</span> : item.answer}
          </div>
        </div>
      ))}
    </div>
  );
}

function joinTooltipParts(...parts) {
  const text = parts
    .map((part) => String(part || '').trim())
    .filter(Boolean)
    .join('\n\n');
  return text || undefined;
}

function metricText(metrics, label) {
  if (!Array.isArray(metrics)) return '';
  const wanted = String(label || '').toLowerCase();
  for (const metric of metrics) {
    if (Array.isArray(metric) && String(metric[0] || '').toLowerCase() === wanted) {
      return String(metric[1] ?? '').trim();
    }
    if (metric && typeof metric === 'object') {
      const key = String(metric.label || metric.key || metric.name || '').toLowerCase();
      if (key === wanted) return String(metric.value ?? '').trim();
    }
  }
  return '';
}

function taskCompleteDisplayText(summary, output) {
  const metricSummary = metricText(summary?.metrics, 'summary');
  if (metricSummary) return metricSummary;
  const object = String(summary?.object ?? '').trim();
  if (object && object.toLowerCase() !== 'task') return object;
  const outputText = String(output ?? '').trim();
  return outputText || '完成';
}

export const ToolBlock = memo(function ToolBlock({ entry, onReviewToggle, sessionRunning = true }) {
  const { t: translate } = useTranslation();
  const contextIdRef = useRef('');
  if (!contextIdRef.current) {
    contextIdRef.current = `tool-${Date.now()}-${Math.random().toString(16).slice(2)}`;
  }

  const {
    isTaskComplete = false,
    isDone = false,
    success = null,
    title = '',
    tool = '',
    displayOverride = '',
    args = null,
    tailLines = [],
    currentPartial = '',
    totalLines = 0,
    totalBytes = 0,
    elapsed = 0,
    startedAtMs = 0,
    summary = null,
    output = '',
    hunks = [],
    attachments = [],
    askUserQuestionResult = null,
  } = entry || {};
  const shouldExpandAskResult = isDone
    && success !== false
    && askUserQuestionItems(askUserQuestionResult).length > 0;
  const [expanded, setExpanded] = useState(shouldExpandAskResult);
  useEffect(() => {
    if (shouldExpandAskResult) setExpanded(true);
  }, [shouldExpandAskResult]);
  const attachmentItems = useMemo(() => normalizeAttachmentList(attachments), [attachments]);
  const genericSummary = useMemo(
    () => fallbackToolSummary(tool || 'tool', args),
    [args, tool],
  );
  const completedSummary = summary || genericSummary;
  const askUserQuestionOutput = useMemo(() => {
    return askUserQuestionText(askUserQuestionResult);
  }, [askUserQuestionResult]);

  const [nowMs, setNowMs] = useState(() => Date.now());
  const liveProgress = !isDone && !!sessionRunning;
  const wasLiveProgressRef = useRef(false);
  if (liveProgress) wasLiveProgressRef.current = true;

  useEffect(() => {
    if (!liveProgress || !startedAtMs) return undefined;
    setNowMs(Date.now());
    const id = window.setInterval(() => setNowMs(Date.now()), 1000);
    return () => window.clearInterval(id);
  }, [liveProgress, startedAtMs]);

  const elapsedSeconds = Number(elapsed) || 0;
  const computedElapsed = startedAtMs
    ? Math.max(0, (nowMs - startedAtMs) / 1000)
    : 0;
  const liveElapsed = liveProgress && startedAtMs
    ? Math.max(elapsedSeconds, computedElapsed)
    : (elapsedSeconds || (wasLiveProgressRef.current ? computedElapsed : 0));
  const shellPresentation = useMemo(() => shellCommandPresentation(entry), [entry]);
  const bashCommand = shellPresentation?.command || '';
  const outputPreview = useMemo(
    () => compactOneLinePreview(output || askUserQuestionOutput || currentPartial || tailLines.join('\n') || bashCommand || title || displayOverride || tool),
    [askUserQuestionOutput, bashCommand, currentPartial, displayOverride, output, tailLines, title, tool],
  );

  const createdFile = useMemo(() => createdFileSource({
    success,
    tool,
    args,
    summary,
    hunks,
    displayOverride,
  }), [args, displayOverride, hunks, success, summary, tool]);

  // diff2html 渲染:先把 hunks 转 unified diff,再交给 diff2html。空 hunks 时
  // 不构造,避免每次 render 浪费。
  const diffText = useMemo(() => {
    if (!Array.isArray(hunks) || hunks.length === 0) return '';
    const file = (summary && summary.object) || displayOverride || 'change';
    return hunksToUnifiedDiff(hunks, file) || '';
  }, [hunks, summary, displayOverride]);

  const diffHtml = useMemo(() => {
    if (createdFile || !diffText) return '';
    try {
      return Diff2Html.html(diffText, {
        drawFileList: false,
        outputFormat: 'line-by-line',
        matching: 'lines',
      });
    } catch {
      return '';
    }
  }, [createdFile, diffText]);

  const fullOutput = output || askUserQuestionOutput || diffText || tailLines.join('\n') || currentPartial || '';
  const fullToolOutput = shellPresentation?.copyText ?? fullOutput;
  const visibleOutput = expanded ? fullToolOutput : (shellPresentation?.command ?? (outputPreview || currentPartial || tailLines.join('\n') || ''));
  const taskCompleteText = isTaskComplete
    ? normalizeTaskCompleteMarkdown(taskCompleteDisplayText(summary, output), '完成')
    : '';
  const taskCompleteHtml = useMemo(
    () => ({ __html: renderMarkdown(taskCompleteText) }),
    [taskCompleteText],
  );
  const toolName = bashCommand
    ? [summary?.verb || title || tool || 'bash', bashCommand].filter(Boolean).join(' · ')
    : (title || displayOverride || tool || summary?.object || 'tool');
  const summaryLabel = summary
    ? [summary.verb, bashCommand || summary.object].filter(Boolean).join(' · ')
    : '';
  const buttonTooltip = joinTooltipParts(summaryLabel || toolName, fullToolOutput);
  const toolContextAttrs = {
    'data-desktop-tool-id': contextIdRef.current,
    'data-desktop-tool-name': toolName,
    'data-desktop-tool-visible-output': visibleOutput || undefined,
    'data-desktop-tool-full-output': fullToolOutput || undefined,
    'data-desktop-tool-expanded': expanded ? 'true' : 'false',
    'data-desktop-tool-toggle': isTaskComplete ? 'false' : 'true',
  };

  const toggleExpanded = useCallback(() => {
    onReviewToggle?.();
    setExpanded((v) => !v);
  }, [onReviewToggle]);

  const handleMarkdownCodeCopy = useCallback(async (event) => {
    const text = codeTextFromCopyButtonTarget(event.target);
    if (text == null) return;
    event.preventDefault();
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      toast({ kind: 'ok', text: '已复制代码' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  }, []);

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'tool' || target.id !== contextIdRef.current) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.EXPAND_TOOL) {
        detail.handled = true;
        onReviewToggle?.();
        setExpanded(true);
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COLLAPSE_TOOL) {
        detail.handled = true;
        onReviewToggle?.();
        setExpanded(false);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onReviewToggle]);

  if (isTaskComplete) {
    return (
      <div
        className="ace-tool-activity min-w-0"
        title={joinTooltipParts('Done', taskCompleteText)}
        {...toolContextAttrs}
      >
        <ActivityLine
          icon={<VsIcon name="ok" size={13} mono={false} className="text-ok" />}
          label="Done"
          detail={compactOneLinePreview(taskCompleteText)}
          preserveLabel
        />
        <div
          className="ace-md ace-task-complete-md min-w-0 max-w-[88%] pb-1"
          onClick={handleMarkdownCodeCopy}
          dangerouslySetInnerHTML={taskCompleteHtml}
        />
      </div>
    );
  }

  const shellDetails = shellPresentation && (
    <ShellCommandCard
      presentation={shellPresentation}
      expanded={expanded}
      onToggle={toggleExpanded}
      running={liveProgress}
      failed={isDone && success === false}
      status={isDone
        ? <MetricList metrics={completedSummary.metrics} />
        : <span className="tabular-nums">{formatElapsed(liveElapsed)}</span>}
    />
  );

  // 所有工具的完成态与运行态共用 ActivityLine；详情仍由 ToolBlock 自己负责。
  if (isDone) {
    const isAskUserQuestionResult = askUserQuestionResult
      && typeof askUserQuestionResult === 'object'
      && !Array.isArray(askUserQuestionResult);
    const askItems = isAskUserQuestionResult ? askUserQuestionItems(askUserQuestionResult) : [];
    const askCancelled = isAskUserQuestionResult
      && (askUserQuestionResult.cancelled === true || askUserQuestionResult.interjected === true);
    const askSubmitted = isAskUserQuestionResult && success !== false && askItems.length > 0;
    if (askCancelled || askSubmitted) {
      const askExpanded = expanded;
      return (
        <div
          {...toolContextAttrs}
          className="ace-tool-activity min-w-0"
          data-ask-user-question-result="true"
          data-question-feedback={askCancelled
            ? (askUserQuestionResult.interjected === true ? 'interject' : 'cancel')
            : 'submit'}
        >
          <ActivityLine
            icon={<ToolSummaryIcon tool={tool} icon={completedSummary.icon} ok={!askCancelled} className={askCancelled ? 'text-fg-mute' : 'text-ok'} />}
            label={completedSummary.verb || title || tool || 'AskUserQuestion'}
            detail={completedSummary.object || ''}
            trailing={liveElapsed > 0 ? <span className="tabular-nums">{formatElapsed(liveElapsed)}</span> : null}
            preserveLabel
            expandable={!askCancelled && askItems.length > 0}
            expanded={askExpanded}
            onToggle={!askCancelled && askItems.length > 0 ? toggleExpanded : undefined}
            title={askCancelled ? undefined : (askExpanded ? '收起详情' : '展开详情')}
            ariaLabel={askCancelled ? undefined : (askExpanded ? '收起详情' : '展开详情')}
          />
          {askCancelled ? (
            <div className="max-w-[88%] pb-1 pl-5 text-[12px] text-fg-mute">{translate('用户已取消回答')}</div>
          ) : askExpanded ? <AskUserQuestionDetails items={askItems} /> : null}
        </div>
      );
    }
    const ok = !!success;
    const questionFeedback = questionFeedbackForTool(entry);
    if (questionFeedback) {
      return (
        <div {...toolContextAttrs} className="ace-tool-activity min-w-0">
          <QuestionFeedbackCard feedback={questionFeedback} />
        </div>
      );
    }
    return (
      <div
        {...toolContextAttrs}
        className="ace-tool-activity min-w-0"
      >
        <ActivityLine
          icon={<ToolSummaryIcon tool={tool} icon={completedSummary.icon} ok={ok} className={ok ? 'text-ok' : 'text-danger'} />}
          label={completedSummary.verb || title || tool || '工具完成'}
          detail={bashCommand || completedSummary.object || ''}
          trailing={(
            <>
              <MetricList metrics={completedSummary.metrics} />
              {!ok && output && <span className="max-w-48 truncate" title={output}>· {outputPreview}</span>}
              {liveElapsed > 0 && <span className="tabular-nums">{formatElapsed(liveElapsed)}</span>}
            </>
          )}
          preserveLabel
          expandable
          expanded={expanded}
          onToggle={toggleExpanded}
          title={buttonTooltip || (expanded ? '收起' : '展开')}
          ariaLabel={expanded ? '收起' : '展开'}
        />
        {expanded && (shellDetails || createdFile || diffHtml || fullToolOutput) && (
          <div className={clsx('w-full min-w-0 pb-2 pt-1', !shellDetails && 'max-w-[88%]')}>
            {shellDetails || (createdFile ? (
              <CreatedFileFrame source={createdFile} />
            ) : diffHtml ? (
              <div
                className="ace-diff ace-tool-diff"
                dangerouslySetInnerHTML={{ __html: diffHtml }}
              />
            ) : (
              <ToolTextFrame text={fullToolOutput} />
            ))}
          </div>
        )}
        {attachmentItems.length > 0 && (
          <div className="w-full min-w-0 max-w-[88%] pb-2 pt-1">
            <AttachmentStrip attachments={attachmentItems} align="left" compact />
          </div>
        )}

      </div>
    );
  }

  // 进度模式
  const hidden = Math.max(0, totalLines - tailLines.length);
  // 工具刚启动、还没有任何可展示内容(无 bash 命令 / tail 输出 / partial)时,
  // 展开区渲染出来只有一圈 padding 的空框(高度就几个像素),观感像 bug。
  // 此时干脆不提供展开:箭头不显示、点击不响应,等有内容后恢复展开能力。
  const progressFrameText = [
    hidden > 0 ? `... +${hidden} 行已折叠` : '',
    tailLines.join('\n'),
    currentPartial,
  ].filter(Boolean).join('\n');
  const hasExpandableContent = !!(shellDetails || progressFrameText);
  return (
    <div
      className="ace-tool-activity min-w-0"
      {...toolContextAttrs}
      data-desktop-tool-toggle={hasExpandableContent ? 'true' : 'false'}
    >
      <ActivityLine
        running
        preferIconWhileRunning
        icon={<ToolSummaryIcon tool={tool} icon={genericSummary.icon} ok />}
        spinnerStatic={!liveProgress}
        label={genericSummary.verb || title || displayOverride || tool || '正在执行工具'}
        detail={bashCommand || genericSummary.object || ''}
        trailing={(
          <>
            <span>{totalLines} 行</span>
            <span>{formatBytes(totalBytes)}</span>
            <span>{formatElapsed(liveElapsed)}</span>
          </>
        )}
        preserveLabel
        expandable={hasExpandableContent}
        expanded={expanded}
        onToggle={hasExpandableContent ? toggleExpanded : undefined}
        title={buttonTooltip || outputPreview || (hasExpandableContent ? (expanded ? '收起' : '展开') : undefined)}
        ariaLabel={hasExpandableContent ? (expanded ? '收起' : '展开') : undefined}
      />
      {expanded && hasExpandableContent && (
        <div className={clsx('w-full min-w-0 pb-1.5 pt-1', !shellDetails && 'max-w-[88%]')}>
          {shellDetails || <ToolTextFrame text={progressFrameText} />}
        </div>
      )}
    </div>
  );
});
