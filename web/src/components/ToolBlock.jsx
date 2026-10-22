// 工具行只展示本地化动作与精简对象。参数、统计和结果位于展开详情，
// 新文件源码、diff、问答和 Shell 保留各自内容渲染器；复制使用完整原文。

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { clsx } from '../lib/format.js';
import { hunksToUnifiedDiff } from '../lib/diff.js';
import { compactOneLinePreview } from '../lib/compactMessagePreview.js';
import { createdFileSource } from '../lib/createdFileSource.js';
import { normalizeAttachmentList } from '../lib/messageAttachments.js';
import { renderMarkdown } from '../lib/markdown.js';
import { highlightSourceForFile } from '../lib/sourceCodeHighlight.js';
import { fallbackToolSummary } from '../lib/toolSummaryFallback.js';
import { canonicalTranscriptTool, presentTool } from '../lib/toolPresentation.js';
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
import { ToolDetailMetadata } from './ToolDetailMetadata.jsx';
import { ToolSummaryIcon, VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';
import * as Diff2Html from 'diff2html';

function ToolTextFrame({ text = '', children = null, metadata = null }) {
  const copyText = String(text ?? '');
  if (!copyText && !children && !metadata) return null;
  if (!copyText && !children) return (
    <div className="ace-tool-output-frame overflow-hidden rounded-xl border border-border bg-surface" data-tool-output-frame="true">
      {metadata}
    </div>
  );
  return (
    <CopyableCodeFrame
      text={copyText}
      className="ace-tool-output-frame overflow-hidden rounded-xl border border-border bg-surface"
      data-tool-output-frame="true"
    >
      {metadata}
      {children || (copyText && (
        <pre
          className="m-0 max-h-[280px] overflow-auto whitespace-pre-wrap break-words px-5 py-3 pr-12 font-mono text-fg-2"
          style={{ fontSize: 'var(--ace-font-size-code)', lineHeight: 1.55 }}
          data-code-copy-source="true"
        >
          {copyText}
        </pre>
      ))}
    </CopyableCodeFrame>
  );
}

function CreatedFileFrame({ source, metadata }) {
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
      {metadata}
      <pre data-code-copy-source="true">
        <code
          className={codeClassName}
          dangerouslySetInnerHTML={{ __html: highlighted.html }}
        />
      </pre>
    </CopyableCodeFrame>
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
  const { t } = useTranslation();
  return (
    <div className="ace-qa-inline-list w-full min-w-0 max-w-[88%] pb-2 pl-5 pt-0.5">
      {items.map((item, index) => (
        <div key={index} className="ace-qa-inline-item py-1">
          <div className="ace-qa-question text-[12px] leading-snug text-fg-mute break-words">
            {index + 1}. {item.question}
          </div>
          <div className="ace-qa-answer mt-0.5 text-[13px] leading-snug break-words">
            {item.unanswered ? <span className="text-fg-mute">{t('toolTranscript.unanswered')}</span> : item.answer}
          </div>
        </div>
      ))}
    </div>
  );
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
  const [detailClipped, setDetailClipped] = useState(false);
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
  const presentation = presentTool(entry, translate, { elapsed: liveElapsed });
  const shellPresentation = useMemo(() => shellCommandPresentation({ ...entry, tool: canonicalTranscriptTool(entry) }), [entry]);
  const bashCommand = shellPresentation?.command || '';
  const outputPreview = useMemo(
    () => compactOneLinePreview(output || askUserQuestionOutput || currentPartial || tailLines.join('\n') || bashCommand || title || displayOverride || tool),
    [askUserQuestionOutput, bashCommand, currentPartial, displayOverride, output, tailLines, title, tool],
  );

  const createdFile = useMemo(() => createdFileSource({
    success,
    tool: presentation.tool,
    args,
    summary,
    hunks,
    displayOverride,
  }), [args, displayOverride, hunks, success, summary, presentation.tool]);

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

  const fullOutput = presentation.output || askUserQuestionOutput || diffText || tailLines.join('\n') || currentPartial || '';
  const fullToolOutput = shellPresentation?.copyText ?? fullOutput;
  const visibleOutput = expanded ? fullToolOutput : (shellPresentation?.command ?? (outputPreview || currentPartial || tailLines.join('\n') || ''));
  const taskCompleteText = isTaskComplete
    ? normalizeTaskCompleteMarkdown(taskCompleteDisplayText(summary, output), '完成')
    : '';
  const taskCompleteHtml = useMemo(
    () => ({ __html: renderMarkdown(taskCompleteText) }),
    [taskCompleteText],
  );
  const toolName = presentation.label + (presentation.detail ? presentation.separator + presentation.detail : '');
  const buttonTooltip = presentation.fullObject || toolName;
  const toggleLabel = translate(expanded ? 'toolTranscript.collapse' : 'toolTranscript.expand');
  // Diff headers already own the complete file path and addition/deletion counts.
  const detailFields = diffHtml ? presentation.fields.filter(field => !['path', '+', '-'].includes(field.key)) : presentation.fields;
  const detailMetadata = detailFields.length ? <ToolDetailMetadata fields={detailFields} /> : null;
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
        title={presentation.label}
        {...toolContextAttrs}
      >
        <ActivityLine
          icon={<VsIcon name="ok" size={13} mono={false} className="text-ok" />}
          label={translate('toolTranscript.names.task_complete')}
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
      transcript
      showCommand={detailClipped || bashCommand !== presentation.detail}
      status={detailMetadata}
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
            label={presentation.label}
            detail={presentation.detail}
            detailSeparator={presentation.separator}
            preserveLabel
            expandable={!askCancelled && askItems.length > 0}
            expanded={askExpanded}
            onToggle={!askCancelled && askItems.length > 0 ? toggleExpanded : undefined}
            title={askCancelled ? undefined : toggleLabel}
            ariaLabel={askCancelled ? undefined : toggleLabel}
          />
          {askCancelled ? (
            <div className="max-w-[88%] pb-1 pl-5 text-[12px] text-fg-mute">{translate('用户已取消回答')}</div>
          ) : askExpanded ? <>{detailMetadata}<AskUserQuestionDetails items={askItems} /></> : null}
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
          label={presentation.label}
          detail={presentation.detail}
          detailSeparator={presentation.separator}
          onDetailOverflowChange={shellPresentation ? setDetailClipped : undefined}
          preserveLabel
          trailing={!ok ? <span className="text-danger">{translate('toolTranscript.failed')}</span> : null}
          expandable
          expanded={expanded}
          onToggle={toggleExpanded}
          title={buttonTooltip || toggleLabel}
          ariaLabel={toggleLabel}
        />
        {expanded && (shellDetails || createdFile || diffHtml || fullToolOutput || detailMetadata) && (
          <div className={clsx('w-full min-w-0 pb-2 pt-1', !shellDetails && 'max-w-[88%]')}>
            {shellDetails || (createdFile ? (
              <CreatedFileFrame source={createdFile} metadata={detailMetadata} />
            ) : diffHtml ? (
              <div className={clsx('ace-diff ace-tool-diff', detailMetadata && 'ace-tool-diff-with-metadata')}>
                {detailMetadata}
                <div dangerouslySetInnerHTML={{ __html: diffHtml }} />
              </div>
            ) : (
              <ToolTextFrame text={fullToolOutput} metadata={detailMetadata} />
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
  // 参数、统计或输出至少有一项时才提供展开，避免出现空框。
  const progressFrameText = [
    hidden > 0 ? translate('toolTranscript.foldedLines', { count: hidden }) : '',
    tailLines.join('\n'),
    currentPartial,
  ].filter(Boolean).join('\n');
  const hasExpandableContent = !!(shellDetails || progressFrameText || detailMetadata);
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
        label={presentation.label}
        detail={presentation.detail}
        detailSeparator={presentation.separator}
        onDetailOverflowChange={shellPresentation ? setDetailClipped : undefined}
        preserveLabel
        expandable={hasExpandableContent}
        expanded={expanded}
        onToggle={hasExpandableContent ? toggleExpanded : undefined}
        title={buttonTooltip || (hasExpandableContent ? toggleLabel : undefined)}
        ariaLabel={hasExpandableContent ? toggleLabel : undefined}
      />
      {expanded && hasExpandableContent && (
        <div className={clsx('w-full min-w-0 pb-1.5 pt-1', !shellDetails && 'max-w-[88%]')}>
          {shellDetails || <ToolTextFrame text={progressFrameText} metadata={detailMetadata} />}
        </div>
      )}
    </div>
  );
});
