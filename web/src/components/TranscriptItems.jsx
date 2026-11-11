import {
  Fragment,
  useCallback,
  useEffect,
  useMemo,
  useState,
} from 'react';
import { renderMarkdown } from '../lib/markdown.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { buildAssistantRunDirectives } from '../lib/assistantRunDirectives.js';
import {
  EMPTY_BACKGROUND_TASK_INDEX,
  MOVE_TO_BACKGROUND_DELAY_MS,
  backgroundableToolStartedAt,
  itemHasRunningBackgroundTask,
  startedBackgroundTaskId,
} from '../lib/backgroundTasks.js';
import { CONVERSATION_ACTIVITY_KIND } from '../lib/conversationActivity.js';
import { hasNextStepQuestion } from '../lib/nextStepBox.js';
import { clsx, relativeTime } from '../lib/format.js';
import {
  DEFAULT_TRANSCRIPT_CAPABILITIES,
  completionSummaryText,
  normalizeTranscriptCapabilities,
  transcriptRenderKind,
  transcriptRowAttrs,
  transcriptRowClassName,
} from '../lib/transcriptItemPresentation.js';
import { ActivityLine } from './ActivityLine.jsx';
import { AttachmentStrip } from './AttachmentStrip.jsx';
import { VsIcon } from './Icon.jsx';
import { Message, MessageActions } from './Message.jsx';
import { SubagentGroupBlock } from './SubagentGroupBlock.jsx';
import { ToolBlock } from './ToolBlock.jsx';
import { toast } from './Toast.jsx';

const EMPTY_ITEMS = Object.freeze([]);
const EMPTY_SET = new Set();
const EMPTY_MAP = new Map();

function activitySummaryDetails(item) {
  return item?.detailItems || item?.collapsedItems || EMPTY_ITEMS;
}

export function ActivityDetailsReveal({ children }) {
  const [settled, setSettled] = useState(false);

  useEffect(() => {
    const fallback = window.setTimeout(() => setSettled(true), 220);
    return () => window.clearTimeout(fallback);
  }, []);

  return (
    <div
      className={clsx('ace-activity-details-reveal', settled && 'is-settled')}
      data-activity-details-reveal="true"
      onAnimationEnd={(event) => {
        if (event.target === event.currentTarget) setSettled(true);
      }}
    >
      <div className="ace-activity-details-reveal-inner mt-1 flex min-h-0 flex-col gap-0.5">
        {children}
      </div>
    </div>
  );
}

// 运行中的前台命令 / 子代理等待跑满 2 秒后,在活动行尾部给出「转到后台」。
function useMoveToBackgroundVisible(startedAtMs, enabled) {
  const [nowMs, setNowMs] = useState(() => Date.now());
  const remaining = enabled && startedAtMs > 0
    ? startedAtMs + MOVE_TO_BACKGROUND_DELAY_MS - nowMs
    : Infinity;
  useEffect(() => {
    if (!Number.isFinite(remaining) || remaining <= 0) return undefined;
    const timer = setTimeout(() => setNowMs(Date.now()), remaining + 50);
    return () => clearTimeout(timer);
  }, [remaining]);
  return Number.isFinite(remaining) && remaining <= 0;
}

// backgroundRunning:这条折叠里包着还没结束的后台任务。回合已经结束,所以只让文字
// 带炫光,图标和「已处理」文案不变;转圈留给还在跑的回合。
export function ActivitySummaryBlock({
  item,
  expanded,
  onToggle,
  activity = null,
  onMoveToBackground,
  backgroundRunning = false,
}) {
  const details = activitySummaryDetails(item);
  const backgroundableStartedAt = item?.live === true && onMoveToBackground
    ? backgroundableToolStartedAt(item?.collapsedItems)
    : 0;
  const showMoveToBackground = useMoveToBackgroundVisible(backgroundableStartedAt, !!onMoveToBackground);
  const hasDetails = details.length > 0;
  const live = item?.live === true;
  const activityKind = activity?.kind || CONVERSATION_ACTIVITY_KIND.IDLE;
  if (
    live
    && !hasDetails
    && (activityKind === CONVERSATION_ACTIVITY_KIND.PERMISSION
      || activityKind === CONVERSATION_ACTIVITY_KIND.QUESTION)
  ) {
    return null;
  }

  const parallelCount = Number(item?.runningToolCount) || 0;
  // 具体进度提示(add-tool-preamble,「适合日常工作」):正在运行的工具带着 daemon
  // 生成的文案(「正在读取 3 个文件」,已含数量、不带参数)时,它优先于实时阶段文案与
  // 并行计数;没有时保持原有文案。
  const preambleTitle = String(item?.preamble?.title || '').trim();
  const label = preambleTitle
    || (live && parallelCount > 1
      ? `正在运行 ${parallelCount} 个工具`
      : (live ? (activity?.label || item?.title || '正在处理请求') : (item?.title || '已处理')));
  const detail = live
    ? [
        preambleTitle ? '' : (activity?.detail || ''),
        activity?.backgroundCount > 0 ? activity.backgroundLabel : '',
      ].filter(Boolean).join(' · ')
    : '';

  const trailing = showMoveToBackground ? (
    <button
      type="button"
      className="rounded px-1.5 py-0.5 text-[11.5px] text-fg-2 transition hover:bg-surface-hi hover:text-fg"
      data-move-to-background="true"
      onClick={(event) => {
        event.stopPropagation();
        onMoveToBackground();
      }}
      onKeyDown={(event) => event.stopPropagation()}
    >
      转到后台
    </button>
  ) : null;

  return (
    <ActivityLine
      icon={live ? null : <VsIcon name="edit" size={13} className="opacity-80" />}
      running={live}
      label={label}
      detail={detail}
      trailing={trailing}
      expandable={hasDetails}
      expanded={expanded}
      onToggle={hasDetails ? onToggle : undefined}
      title={hasDetails ? (expanded ? '收起详情' : '展开详情') : label}
      ariaLabel={hasDetails ? (expanded ? '收起详情' : '展开详情') : undefined}
      live={live || backgroundRunning}
      className={item?.mode === 'processed' ? 'ace-activity-line-processed' : ''}
    />
  );
}

export function MediaGroupBlock({ item, collapsed, onToggle, messageAutoCollapse = true }) {
  const attachments = Array.isArray(item?.attachments) ? item.attachments : EMPTY_ITEMS;
  if (attachments.length === 0) return null;
  const label = item?.title || `已查看 ${attachments.length} 张图像`;
  const toggleHint = collapsed ? '展开图像' : '收起图像';
  return (
    <div className="flex flex-col">
      <ActivityLine
        icon={<VsIcon name="eye" size={13} className="opacity-80" />}
        label={label}
        expandable={messageAutoCollapse}
        expanded={!collapsed}
        onToggle={messageAutoCollapse ? onToggle : undefined}
        title={messageAutoCollapse ? toggleHint : label}
        ariaLabel={messageAutoCollapse ? toggleHint : undefined}
      />
      {!collapsed && (
        <div className="mt-1 max-w-[88%]">
          <AttachmentStrip attachments={attachments} align="left" compact />
        </div>
      )}
    </div>
  );
}

export function CompletionSummaryBlock({
  item,
  copyText,
  onFork,
  forkPending = false,
  forkLoading = false,
  showFooter = true,
  showQuestion = false,
}) {
  const summaryText = completionSummaryText(item);
  const html = useMemo(() => ({ __html: renderMarkdown(summaryText) }), [summaryText]);
  const handleMarkdownClick = useCallback(async (event) => {
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

  return (
    <div
      className="group max-w-[88%] px-1 py-0.5 text-fg break-words"
      title={item?.title || `总结：${summaryText}`}
    >
      <div className="text-[12px] font-semibold text-fg-mute mb-0.5">总结</div>
      <div
        className="ace-md ace-completion-summary-md text-[13px] leading-[1.6]"
        onClick={handleMarkdownClick}
        dangerouslySetInnerHTML={html}
      />
      {/* 「下一步」框已结束(历史回合 / 已关闭)时,问题以静态文字留在总结下面。 */}
      {showQuestion && item?.wrapUp?.ask && (
        <div
          className="mt-1 text-[13px] leading-[1.6] text-fg-2 break-words"
          data-next-step-question="settled"
        >
          {item.wrapUp.ask}
        </div>
      )}
      {showFooter && (
        <div className="min-h-6 flex items-center gap-1">
          <MessageActions
            messageId={item?.messageId}
            getCopyText={() => copyText ?? summaryText}
            onFork={onFork}
            forkPending={forkPending}
            forkLoading={forkLoading}
          />
          {item?.ts != null && (
            <span className="text-[10px] text-fg-mute font-normal">{relativeTime(item.ts)}</span>
          )}
        </div>
      )}
    </div>
  );
}

export function TerminationNoticeBlock({
  item,
  onFork,
  forkPending = false,
  forkLoading = false,
  forkMessageId = '',
  showFooter = false,
}) {
  const content = item?.content || '任务已终止';
  return (
    <div className="group max-w-[88%] px-1 py-0.5">
      <div className="text-[12px] leading-5 text-danger whitespace-pre-wrap break-words">
        {content}
      </div>
      {showFooter && (
        <div className="min-h-6 flex items-center gap-1">
          <MessageActions
            messageId={forkMessageId}
            getCopyText={() => content}
            onFork={onFork}
            forkPending={forkPending}
            forkLoading={forkLoading}
          />
          {item?.ts != null && (
            <span className="text-[10px] text-fg-mute font-normal">{relativeTime(item.ts)}</span>
          )}
        </div>
      )}
    </div>
  );
}

function TranscriptItem({
  item,
  nested,
  itemKey,
  capabilities,
  directives,
  expandedActivityKeys,
  collapsedMediaKeys,
  onToggleActivity,
  onToggleMedia,
  conversationActivity,
  onMoveToBackground,
  subagentTasksById,
  backgroundTaskIndex,
  onOpenSubagent,
  sessionRunning,
  onReviewToggle,
  onFork,
  forkingMessageId,
  onOpenFilePreview,
  onLocateInFileTree,
  showAceCodeAvatar,
  messageAutoCollapse,
  annotationPresentations,
  activeNextStepItemId,
}) {
  const renderKind = transcriptRenderKind(item);

  if (renderKind === 'termination_notice') {
    const directive = nested ? undefined : directives.get(item.id);
    const forkMessageId = directive?.forkMessageId || '';
    return (
      <div
        className={transcriptRowClassName(item, { nested })}
        {...transcriptRowAttrs(item, { nested, canFork: capabilities.forkMessages })}
      >
        <TerminationNoticeBlock
          item={item}
          onFork={capabilities.forkMessages ? onFork : undefined}
          forkPending={capabilities.forkMessages && forkingMessageId !== ''}
          forkLoading={capabilities.forkMessages && forkingMessageId !== '' && forkingMessageId === forkMessageId}
          forkMessageId={forkMessageId}
          showFooter={!nested && capabilities.showMessageFooters && directive?.showFooter === true}
        />
      </div>
    );
  }

  if (renderKind === 'completion_summary') {
    const directive = nested ? undefined : directives.get(item.id);
    return (
      <div
        className={transcriptRowClassName(item, { nested })}
        {...transcriptRowAttrs(item, { nested, canFork: capabilities.forkMessages })}
      >
        <CompletionSummaryBlock
          item={item}
          copyText={directive?.copyText}
          onFork={capabilities.forkMessages ? onFork : undefined}
          forkPending={capabilities.forkMessages && forkingMessageId !== ''}
          forkLoading={capabilities.forkMessages
            && forkingMessageId !== ''
            && forkingMessageId === String(item.messageId || '')}
          showFooter={!nested && capabilities.showMessageFooters && directive?.showFooter === true}
          showQuestion={hasNextStepQuestion(item) && item.id !== activeNextStepItemId}
        />
      </div>
    );
  }

  if (renderKind === 'media_group') {
    return (
      <div
        className={transcriptRowClassName(item, { nested })}
        {...transcriptRowAttrs(item, { nested, canFork: capabilities.forkMessages })}
      >
        <MediaGroupBlock
          item={item}
          collapsed={messageAutoCollapse && collapsedMediaKeys.has(item.id)}
          messageAutoCollapse={messageAutoCollapse}
          onToggle={(event) => onToggleMedia?.(item.id, event?.currentTarget)}
        />
      </div>
    );
  }

  if (renderKind === 'subagent_group') {
    return (
      <div
        className={transcriptRowClassName(item, { nested })}
        {...transcriptRowAttrs(item, { nested, canFork: capabilities.forkMessages })}
      >
        <SubagentGroupBlock
          agents={item.agents}
          messageAutoCollapse={messageAutoCollapse}
          tasksById={subagentTasksById}
          onOpen={capabilities.openSubagentTranscripts ? onOpenSubagent : undefined}
        />
      </div>
    );
  }

  if (renderKind === 'activity_summary') {
    const expanded = expandedActivityKeys.has(item.id);
    const detailItems = activitySummaryDetails(item);
    return (
      <div
        className={transcriptRowClassName(item, { nested })}
        {...transcriptRowAttrs(item, { nested, canFork: capabilities.forkMessages })}
      >
        <ActivitySummaryBlock
          item={item}
          expanded={expanded}
          onToggle={(event) => onToggleActivity?.(item.id, event?.currentTarget)}
          activity={!nested && item.live ? conversationActivity : null}
          onMoveToBackground={!nested && item.live ? onMoveToBackground : undefined}
          backgroundRunning={itemHasRunningBackgroundTask(item, backgroundTaskIndex.runningIds)}
        />
        {expanded && (
          <ActivityDetailsReveal>
            <TranscriptItems
              items={detailItems}
              nested
              keyPrefix={`${itemKey}-nested`}
              capabilities={capabilities}
              expandedActivityKeys={expandedActivityKeys}
              collapsedMediaKeys={collapsedMediaKeys}
              onToggleActivity={onToggleActivity}
              onToggleMedia={onToggleMedia}
              subagentTasksById={subagentTasksById}
              backgroundTaskIndex={backgroundTaskIndex}
              onOpenSubagent={onOpenSubagent}
              sessionRunning={sessionRunning}
              onReviewToggle={onReviewToggle}
              onFork={onFork}
              forkingMessageId={forkingMessageId}
              onOpenFilePreview={onOpenFilePreview}
              onLocateInFileTree={onLocateInFileTree}
              showAceCodeAvatar={showAceCodeAvatar}
              messageAutoCollapse={messageAutoCollapse}
              annotationPresentations={annotationPresentations}
            />
          </ActivityDetailsReveal>
        )}
      </div>
    );
  }

  const directive = item?.kind === 'msg'
    && (item.role === 'assistant' || (!nested && item.role === 'error'))
    ? directives.get(item.id)
    : undefined;
  if (directive?.hide) return null;
  const continuation = item?.role === 'assistant' && directive
    ? directive.showHeader === false
    : false;
  const showFooter = !nested
    && capabilities.showMessageFooters
    && (directive ? directive.showFooter === true : true);
  const forkMessageId = directive?.forkMessageId || item?.messageId;

  return (
    <div
      className={transcriptRowClassName(item, { nested })}
      {...transcriptRowAttrs(item, {
        nested,
        continuation,
        canFork: capabilities.forkMessages,
      })}
    >
      {renderKind === 'tool' ? (
        <ToolBlock
          entry={item.tool}
          onReviewToggle={capabilities.pauseTailOnToolReview ? onReviewToggle : undefined}
          sessionRunning={sessionRunning}
          backgroundTask={backgroundTaskIndex.byId.get(startedBackgroundTaskId(item.tool)) || null}
          onOpenBackgroundTask={capabilities.openSubagentTranscripts ? onOpenSubagent : undefined}
        />
      ) : (
        <Message
          role={item?.role}
          content={item?.content}
          contentParts={item?.contentParts}
          ts={item?.ts}
          streaming={item?.streaming}
          messageId={forkMessageId}
          metadata={item?.metadata}
          composerContent={item?.composerContent}
          onFork={capabilities.forkMessages ? onFork : undefined}
          forkPending={capabilities.forkMessages && forkingMessageId !== ''}
          forkLoading={capabilities.forkMessages
            && forkingMessageId !== ''
            && forkingMessageId === String(forkMessageId || '')}
          onOpenFilePreview={capabilities.navigateFiles ? onOpenFilePreview : undefined}
          onLocateInFileTree={capabilities.navigateFiles ? onLocateInFileTree : undefined}
          continuation={continuation}
          showFooter={showFooter}
          showAceCodeAvatar={showAceCodeAvatar}
          messageAutoCollapse={messageAutoCollapse}
          annotationPresentations={capabilities.showSelectionAnnotations
            ? annotationPresentations
            : undefined}
        />
      )}
    </div>
  );
}

export function TranscriptItems({
  items = EMPTY_ITEMS,
  nested = false,
  keyPrefix = 'transcript',
  capabilities = DEFAULT_TRANSCRIPT_CAPABILITIES,
  assistantRunDirectives = null,
  expandedActivityKeys = EMPTY_SET,
  collapsedMediaKeys = EMPTY_SET,
  onToggleActivity,
  onToggleMedia,
  conversationActivity = null,
  onMoveToBackground,
  subagentTasksById = EMPTY_MAP,
  backgroundTaskIndex = EMPTY_BACKGROUND_TASK_INDEX,
  onOpenSubagent,
  sessionRunning = false,
  onReviewToggle,
  onFork,
  forkingMessageId = '',
  onOpenFilePreview,
  onLocateInFileTree,
  showAceCodeAvatar = false,
  messageAutoCollapse = true,
  annotationPresentations,
  renderBeforeItem,
  activeNextStepItemId = '',
}) {
  const list = Array.isArray(items) ? items : EMPTY_ITEMS;
  const generatedDirectives = useMemo(
    () => (assistantRunDirectives instanceof Map
      ? null
      : buildAssistantRunDirectives(list)),
    [assistantRunDirectives, list],
  );
  const directives = assistantRunDirectives instanceof Map
    ? assistantRunDirectives
    : (generatedDirectives || EMPTY_MAP);
  const resolvedCapabilities = normalizeTranscriptCapabilities(capabilities);

  return list.map((item, index) => {
    const identity = item?.id ?? index;
    const itemKey = nested ? `${keyPrefix}-${identity}` : identity;
    const before = !nested ? renderBeforeItem?.(item) : null;
    return (
      <Fragment key={itemKey}>
        {before}
        <TranscriptItem
          item={item}
          nested={nested}
          itemKey={itemKey}
          capabilities={resolvedCapabilities}
          directives={directives}
          expandedActivityKeys={expandedActivityKeys}
          collapsedMediaKeys={collapsedMediaKeys}
          onToggleActivity={onToggleActivity}
          onToggleMedia={onToggleMedia}
          conversationActivity={conversationActivity}
          onMoveToBackground={onMoveToBackground}
          subagentTasksById={subagentTasksById}
          backgroundTaskIndex={backgroundTaskIndex}
          onOpenSubagent={onOpenSubagent}
          sessionRunning={sessionRunning}
          onReviewToggle={onReviewToggle}
          onFork={onFork}
          forkingMessageId={forkingMessageId}
          onOpenFilePreview={onOpenFilePreview}
          onLocateInFileTree={onLocateInFileTree}
          showAceCodeAvatar={showAceCodeAvatar}
          messageAutoCollapse={messageAutoCollapse}
          annotationPresentations={annotationPresentations}
          activeNextStepItemId={activeNextStepItemId}
        />
      </Fragment>
    );
  });
}
