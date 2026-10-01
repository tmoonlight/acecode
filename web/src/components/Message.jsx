// 单条消息渲染:user 气泡(右)/ assistant 正文(左)/ system 信息活动行。
// assistant 走 markdown-it 渲染(见 lib/markdown.js)。
//
// hover actions(codex 风格):user 消息和 assistant run 最后一条消息悬停时浮出
// 消息底部同侧的复制 + 分叉按钮(左消息在左下角,右消息在右下角)。复制走 navigator.clipboard.writeText;分叉
// 调上层 onFork(messageId) — disabled 当 messageId 缺失。

import { memo, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { useTranslation } from 'react-i18next';
import { renderMarkdownBlocks } from '../lib/markdown.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { clsx, relativeTime } from '../lib/format.js';
import { presentSystemNotice } from '../lib/systemNotice.js';
import { assistantChromeState } from '../lib/assistantAvatarDisplay.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { ActivityLine } from './ActivityLine.jsx';
import { VsIcon, CommandGlyph, FileTypeIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';
import { resolveLeadingSlashCommand } from '../lib/slashCommands.js';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { AttachmentStrip } from './AttachmentStrip.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import { attachmentsFromContentParts, isImageAttachment } from '../lib/messageAttachments.js';
import {
  composerContentAttachments,
  composerContentClipboardText,
  composerContentText,
  isPasteBlockPart,
  normalizeComposerContent,
} from '../lib/composerContent.js';
import { isComposerThumbnailAttachment } from '../lib/composerImagePresentation.js';
import { pasteBlockTextSource, pasteBlocksOf, pastedTextTitle } from '../lib/pastedText.js';
import { composerContentMessagePreview, userMessageTextPreview } from '../lib/userMessagePreview.js';
import { AttachmentTextLoaderContext } from './AttachmentTextLoaderContext.jsx';
import { PastedTextCard } from './PastedTextCard.jsx';
import { PastedTextDialog } from './PastedTextDialog.jsx';
import { extractSessionReferences } from '../lib/sessionReference.js';
import { DESKTOP_CONTEXT_ACTION_EVENT, DESKTOP_CONTEXT_ACTIONS } from '../lib/desktopContextMenu.js';

export function MessageActions({ messageId, getCopyText, onFork, forkPending = false, forkLoading = false }) {
  const handleCopy = async (event) => {
    event.stopPropagation();
    try {
      const text = getCopyText();
      if (!navigator.clipboard) throw new Error('clipboard unavailable');
      await navigator.clipboard.writeText(text);
      toast({ kind: 'ok', text: '已复制' });
    } catch (e) {
      toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
    }
  };
  const handleFork = (event) => {
    event.stopPropagation();
    if (!messageId || forkPending) return;
    onFork?.(messageId);
  };
  return (
    <div
      className="ace-msg-actions flex gap-0.5"
      data-fork-loading={forkLoading ? 'true' : undefined}
    >
      <button type="button" onClick={handleCopy} title="复制">
        <VsIcon name="copy" size={14} />
      </button>
      <button
        type="button"
        onClick={handleFork}
        disabled={!messageId || forkPending}
        data-fork-loading={forkLoading ? 'true' : undefined}
        aria-busy={forkLoading ? 'true' : undefined}
        aria-label={forkLoading ? '正在分叉到新会话' : '分叉到新会话'}
        title={forkLoading
          ? '正在分叉到新会话…'
          : (messageId ? '分叉到新会话' : '此消息不可分叉(无 ID)')}
      >
        {forkLoading
          ? <span className="ace-spinner w-3.5 h-3.5" aria-hidden="true" />
          : <VsIcon name="fork" size={14} />}
      </button>
    </div>
  );
}

// 斜杠命令徽标:skill 图标 + 蓝色命令名,hover 浮出描述。
// 放在 whitespace-pre-wrap 容器里随正文内联排版。
//
// 描述气泡走 portal 挂到 document.body + position:fixed:聊天区是 overflow 滚动
// 容器、顶部还有 sticky header,CSS 绝对定位的浮层会被裁剪 / 盖住。portal 到顶层
// 才能稳定盖在最前。顶部空间不足时(被 header 压住)自动翻到徽标下方。
function CommandToken({ token, name, kind, description }) {
  const anchorRef = useRef(null);
  const [tip, setTip] = useState(null);
  const displayName = String(name || token || '').replace(/^\/+/, '');

  const showTip = useCallback(() => {
    if (!description) return;
    const el = anchorRef.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const right = Math.max(8, window.innerWidth - r.right);
    if (r.top < 96) {
      // 顶部空间不足 → 翻到徽标下方,避免被 header 遮住
      setTip({ placement: 'below', right, top: Math.round(r.bottom + 6) });
    } else {
      setTip({ placement: 'above', right, bottom: Math.round(window.innerHeight - r.top + 6) });
    }
  }, [description]);
  const hideTip = useCallback(() => setTip(null), []);

  return (
    <span
      ref={anchorRef}
      className="ace-cmd-token"
      tabIndex={description ? 0 : undefined}
      onMouseEnter={showTip}
      onMouseLeave={hideTip}
      onFocus={showTip}
      onBlur={hideTip}
    >
      <CommandGlyph kind={kind} command={name} size={12} className="ace-cmd-token-glyph" />
      <span className="ace-cmd-token-name">{displayName}</span>
      {tip
        ? createPortal(
            <span
              className="ace-cmd-token-tip"
              data-ace-native-overlay="overlap"
              role="tooltip"
              data-placement={tip.placement}
              style={{
                right: tip.right,
                top: tip.placement === 'below' ? tip.top : undefined,
                bottom: tip.placement === 'above' ? tip.bottom : undefined,
              }}
            >
              <span className="ace-cmd-token-tip-name">{displayName}</span>
              <span className="ace-cmd-token-tip-desc">{description}</span>
            </span>,
            document.body,
          )
        : null}
    </span>
  );
}

// 用户消息正文:首段命中已知 skill / builtin 命令时把 "/name" 渲染成徽标,
// 其余原文照常;未命中(普通消息或未知命令)→ 纯文本回退。
function UserMessageBody({ content }) {
  const { commands } = useSlashCommands();
  const cmd = useMemo(() => resolveLeadingSlashCommand(content, commands), [content, commands]);
  if (!cmd) return content;
  return (
    <>
      <CommandToken token={cmd.token} name={cmd.name} kind={cmd.kind} description={cmd.description} />
      {cmd.rest}
    </>
  );
}

// 对话记录里的粘贴块卡片:内联块的正文就在 composer_content 里;文件块的来源取
// content_parts 合成的附件记录(带 blob_url 与 size_bytes),经 context 的 loader 读取。
function pasteBlockCard(block, attachments) {
  const { part } = block;
  if (block.kind === 'inline') {
    return { id: block.id, title: pastedTextTitle(part.text), source: { text: part.text } };
  }
  const attachment = attachments.find((item) => (part.id && item.id === part.id) || item.local_id === part.key) || null;
  return {
    id: block.id,
    title: part.paste?.title || part.name,
    sizeBytes: attachment?.size_bytes,
    uploading: !!attachment?.uploading,
    source: pasteBlockTextSource({ part, resource: attachment }),
  };
}

// 图片附件与输入框一致:显示在气泡上方的缩略图条(AttachmentStrip),不作为正文里的
// 文件名按钮。曾经所有 attachment 部件都进正文按钮分支,同时又被从缩略图条剔除,
// 用户消息里的图片于是只剩一个文件名。
function isThumbnailPart(part) {
  return part?.type === 'attachment' && isComposerThumbnailAttachment(part);
}

function OrderedUserMessageBody({ composerContent, contentParts, onOpenFilePreview, onLocateInFileTree }) {
  const { commands } = useSlashCommands();
  const loadAttachmentText = useContext(AttachmentTextLoaderContext);
  const [preview, setPreview] = useState(null);
  const [openPasteId, setOpenPasteId] = useState('');
  // 缩略图由 AttachmentStrip 渲染并处理预览;这里只留正文内联的附件,避免桌面右键
  // 「预览」被两处同时接住。
  const attachments = useMemo(() => composerContentAttachments(
    composerContent, attachmentsFromContentParts(contentParts),
  ).filter((attachment) => !isComposerThumbnailAttachment(attachment)), [composerContent, contentParts]);
  const pasteCards = useMemo(
    () => pasteBlocksOf(composerContent).map((block) => pasteBlockCard(block, attachments)),
    [composerContent, attachments],
  );
  const openPaste = openPasteId ? pasteCards.find((card) => card.id === openPasteId) : null;
  // 两种粘贴块都渲染成上方的卡片;留在正文里会落进 attachment 的内联按钮分支。
  // 图片同理,由气泡上方的缩略图条渲染。
  const bodyParts = composerContent.parts.filter((part) => !isPasteBlockPart(part) && !isThumbnailPart(part));
  const previewAttachment = useCallback((attachment) => {
    const url = attachment.blob_url || attachment.preview_url || attachment.url || '';
    if (isImageAttachment(attachment) && url) {
      setPreview({ src: url, alt: attachment.name || 'attachment' });
    } else if (attachment.path) onOpenFilePreview?.(attachment.path);
  }, [onOpenFilePreview]);
  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      if (detail.action !== DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT || detail.target?.type !== 'attachment') return;
      const attachment = attachments.find((item) => (item.id || item.local_id) === detail.target.id);
      if (!attachment) return;
      detail.handled = true;
      previewAttachment(attachment);
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [attachments, previewAttachment]);
  return (
    <>
      {pasteCards.length > 0 ? (
        <div
          className={clsx('flex flex-wrap gap-1.5 whitespace-normal', bodyParts.length > 0 && 'mb-1.5')}
          data-user-message-pasted-text="true"
        >
          {pasteCards.map((card) => (
            <PastedTextCard
              key={card.id}
              title={card.title}
              sizeBytes={card.sizeBytes}
              status={card.uploading ? 'uploading' : 'ready'}
              onOpen={() => setOpenPasteId(card.id)}
            />
          ))}
        </div>
      ) : null}
      {bodyParts.map((part, index) => {
        if (part.type === 'text') {
          const displayText = extractSessionReferences(part.text).displayText;
          return index === 0 ? <UserMessageBody key={index} content={displayText} /> : displayText;
        }
        if (part.type === 'skill') {
          const command = commands.find((item) => part.path
            ? (item.path || item.skill_path) === part.path
            : item.name === part.name);
          return <CommandToken key={index} token={part.token} name={part.name} kind="skill" description={command?.description || part.path} />;
        }
        if (part.type === 'path') {
          return (
            <button key={index} type="button" className="ace-cmd-token" title={part.path}
              data-file-path={part.path} data-file-kind={part.directory ? 'directory' : 'file'}
              onClick={() => part.directory ? onLocateInFileTree?.(part.path) : onOpenFilePreview?.(part.path)}>
              {part.directory ? <VsIcon name="folder" size={12} className="ace-cmd-token-glyph" />
                : <FileTypeIcon path={part.path} size={12} className="ace-cmd-token-glyph" />}
              <span className="ace-cmd-token-name">{part.path}</span>
            </button>
          );
        }
        const attachment = attachments.find((item) => (part.id && item.id === part.id) || item.local_id === part.key) || part;
        const url = attachment.blob_url || attachment.preview_url || attachment.url || '';
        const imageUrl = isImageAttachment(attachment) ? url : '';
        return (
          <button key={index} type="button" className="ace-cmd-token" title={attachment.source_path || attachment.name}
            data-desktop-attachment-id={attachment.id || part.key}
            data-desktop-attachment-name={attachment.name}
            data-desktop-attachment-url={url || undefined}
            data-desktop-attachment-path={attachment.path || undefined}
            data-desktop-attachment-preview-url={imageUrl || undefined}
            data-desktop-attachment-copy-image-url={imageUrl || undefined}
            data-desktop-attachment-mime-type={attachment.mime_type || undefined}
            data-desktop-attachment-kind={attachment.kind}
            data-desktop-attachment-mutable="false"
            onClick={() => previewAttachment(attachment)}>
            <FileTypeIcon path={attachment.name} size={12} className="ace-cmd-token-glyph" />
            <span className="ace-cmd-token-name">{attachment.name}</span>
          </button>
        );
      })}
      <ImageLightbox preview={preview} onClose={() => setPreview(null)} />
      {openPaste ? (
        <PastedTextDialog
          title={openPaste.title}
          source={openPaste.source}
          loader={loadAttachmentText}
          readOnly
          onClose={() => setOpenPasteId('')}
        />
      ) : null}
    </>
  );
}

function UserBubble({
  content,
  contentParts,
  composerContent,
  onOpenFilePreview,
  onLocateInFileTree,
  ts,
  messageId,
  onFork,
  forkPending,
  forkLoading,
  showFooter,
  annotationPresentations,
}) {
  // 正文内联渲染的附件不再进缩略图条;图片部件不在正文里,照常留在缩略图条。
  const inlineReferences = (composerContent?.parts || [])
    .filter((part) => part.type === 'attachment' && !isThumbnailPart(part));
  const inlineIds = new Set(inlineReferences.map((part) => part.id).filter(Boolean));
  const inlineKeys = new Set(inlineReferences.map((part) => part.key).filter(Boolean));
  const remainingParts = composerContent
    ? (contentParts || []).filter((part) => !part.attachment || !(
      inlineIds.has(part.attachment.id) || inlineKeys.has(part.attachment.local_id)
    ))
    : contentParts;
  // 只有图片的消息不画空气泡。
  const hasBubbleBody = composerContent
    ? composerContent.parts.some((part) => !isThumbnailPart(part))
    : !!content;
  // f300:2400 多万字符的旧消息整段进 pre-wrap 气泡会卡死页面。气泡只渲染有界预览,
  // 截断时显示 … 与「查看全文」(只读对话框);复制仍取全文。
  const messagePreview = useMemo(() => {
    if (composerContent) {
      const result = composerContentMessagePreview(composerContent);
      return { composerContent: result.content || composerContent, text: '', truncated: result.truncated };
    }
    const result = userMessageTextPreview(content);
    return { composerContent: null, text: result.preview, truncated: result.truncated };
  }, [composerContent, content]);
  const [fullTextOpen, setFullTextOpen] = useState(false);
  const fullText = useMemo(() => {
    if (!fullTextOpen) return '';
    return composerContent
      ? extractSessionReferences(composerContentText(composerContent)).displayText
      : String(content ?? '');
  }, [fullTextOpen, composerContent, content]);
  return (
    <div className="self-end min-w-0 max-w-[70%] flex flex-col items-end gap-0.5 group">
      <AttachmentStrip
        contentParts={remainingParts}
        annotationPresentations={annotationPresentations}
        align="right"
      />
      {hasBubbleBody ? (
        <div className="ace-user-message-bubble ace-chat-message-content px-3.5 py-2 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words">
          {messagePreview.composerContent ? (
            <OrderedUserMessageBody composerContent={messagePreview.composerContent} contentParts={contentParts}
              onOpenFilePreview={onOpenFilePreview} onLocateInFileTree={onLocateInFileTree} />
          ) : <UserMessageBody content={messagePreview.text} />}
          {messagePreview.truncated ? (
            <>
              {'…'}
              <button
                type="button"
                className="ml-1 align-baseline text-[12px] text-accent hover:underline"
                data-user-message-view-full="true"
                onClick={() => setFullTextOpen(true)}
              >
                查看全文
              </button>
            </>
          ) : null}
        </div>
      ) : null}
      {fullTextOpen ? (
        <PastedTextDialog
          title={pastedTextTitle(fullText)}
          source={{ text: fullText }}
          readOnly
          onClose={() => setFullTextOpen(false)}
        />
      ) : null}
      {showFooter && (
        <div className="min-h-6 flex items-center justify-end gap-1 mr-1">
          {ts != null && <span className="text-[10px] text-fg-mute">{relativeTime(ts)}</span>}
          <MessageActions
            messageId={messageId}
            getCopyText={() => composerContent
              ? extractSessionReferences(composerContentClipboardText(composerContent)).displayText
              : content}
            onFork={onFork}
            forkPending={forkPending}
            forkLoading={forkLoading}
          />
        </div>
      )}
    </div>
  );
}

function AssistantBubble({
  content,
  contentParts,
  ts,
  streaming,
  messageId,
  onFork,
  forkPending,
  forkLoading,
  onOpenFilePreview,
  onLocateInFileTree,
  continuation,
  showFooter,
  showAceCodeAvatar,
  annotationPresentations,
}) {
  // 按块渲染(而非全文一次 dangerouslySetInnerHTML):流式追加时只有尾部
  // 块的 HTML 字符串变化,前缀块被 React 的字符串比较跳过,DOM 保持不动。
  // 整树替换会销毁浏览器滚动锚点并造成高度瞬时振荡 —— 那是 desktop
  // (WebView2)上流式出字时消息区上下跳动的主要来源。
  const blocks = useMemo(() => renderMarkdownBlocks(content || ''), [content]);
  const chrome = assistantChromeState({ showAceCodeAvatar, continuation });
  const handleMarkdownClick = useCallback(async (event) => {
    // 1) 本地文件链接 → 在中间详情页开预览。必须拦下默认导航,否则相对 href 会跳到
    //    http://<host>/<path> 命中 SPA 兜底(白屏/错误页),外链形态的还会开新标签页。
    const fileAnchor = event.target?.closest?.('a[data-file-path]');
    if (fileAnchor) {
      event.preventDefault();
      event.stopPropagation();
      const path = fileAnchor.getAttribute('data-file-path') || '';
      const kind = fileAnchor.getAttribute('data-file-kind') || 'file';
      const lineAttr = fileAnchor.getAttribute('data-file-line');
      const line = lineAttr ? Number(lineAttr) : null;
      if (!path) return;
      if (kind === 'directory') onLocateInFileTree?.(path);
      else onOpenFilePreview?.(path, line);
      return;
    }
    // 2) 代码块复制按钮(原逻辑)。
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
  }, [onLocateInFileTree, onOpenFilePreview]);
  // ACECode 头像永久隐藏;不再保留空白占位,让左右外边距保持一致。
  return (
    <div className={`flex min-w-0 ${chrome.gapClass} max-w-[88%] group relative`}>
      {chrome.showAvatarPlaceholder ? (
        <div className="w-6 shrink-0" aria-hidden="true" />
      ) : chrome.showAvatar ? (
        <div className="w-6 h-6 rounded-full bg-ok text-white text-[11px] font-bold flex items-center justify-center shrink-0 mt-[2px]">A</div>
      ) : (
        null
      )}
      <div className="flex-1 min-w-0 flex flex-col gap-1">
        {chrome.showName && (
          <div className="text-[12px] font-semibold text-fg flex items-center gap-1.5">
            ACECode
          </div>
        )}
        <div
          className="ace-md ace-chat-message-content text-[13px] text-fg leading-[1.6] py-0.5"
          onClick={handleMarkdownClick}
        >
          {blocks.map((block) => (
            <div
              key={block.key}
              className="ace-md-block"
              dangerouslySetInnerHTML={{ __html: block.html }}
            />
          ))}
        </div>
        <AttachmentStrip
          contentParts={contentParts}
          annotationPresentations={annotationPresentations}
          align="left"
        />
        {showFooter && (
          <div className="min-h-6 flex items-center gap-1">
            {!streaming && (
              <MessageActions
                messageId={messageId}
                getCopyText={() => content || ''}
                onFork={onFork}
                forkPending={forkPending}
                forkLoading={forkLoading}
              />
            )}
            {ts != null && <span className="text-[10px] text-fg-mute font-normal">{relativeTime(ts)}</span>}
          </div>
        )}
      </div>
    </div>
  );
}

function SystemRow({ role, content, metadata }) {
  const { t } = useTranslation();
  const [manuallyExpanded, setExpanded] = useState(false);
  const { title: label, text } = presentSystemNotice({ role, content, metadata }, t);
  const hasContent = text.trim().length > 0;
  const expandable = hasContent;
  const expanded = hasContent && manuallyExpanded;

  return (
    <div className="self-stretch min-w-0" data-system-notice="true">
      <ActivityLine
        className="ace-system-activity-line"
        icon={<VsIcon name="info" size={16} />}
        label={label}
        expandable={expandable}
        expanded={expanded}
        onToggle={() => setExpanded((v) => !v)}
        ariaLabel={t(expanded ? 'systemNotice.collapse' : 'systemNotice.expand', { title: label })}
      />
      {expanded && (
        <div className="w-full min-w-0 max-w-[88%] pb-1.5 pt-1">
          <CopyableCodeFrame
            text={text}
            className="ace-system-copy-frame overflow-hidden rounded-xl border border-border bg-surface text-fg-2"
          >
            <div
              className="whitespace-pre-wrap break-words px-5 py-3 text-[13px] leading-[1.55]"
              data-code-copy-source="true"
            >
              {text}
            </div>
          </CopyableCodeFrame>
        </div>
      )}
    </div>
  );
}

function ErrorRow({
  content,
  ts,
  messageId,
  onFork,
  forkPending,
  forkLoading,
  showFooter,
}) {
  return (
    <div className="group self-stretch max-w-[88%] flex flex-col gap-0.5">
      <div className="ace-chat-message-content rounded-md border border-danger/30 bg-danger-bg px-3 py-2 text-[12px] leading-5 text-danger whitespace-pre-wrap break-words">
        {content || '[Error]'}
      </div>
      {showFooter && (
        <div className="min-h-6 flex items-center gap-1">
          <MessageActions
            messageId={messageId}
            getCopyText={() => content || '[Error]'}
            onFork={onFork}
            forkPending={forkPending}
            forkLoading={forkLoading}
          />
          {ts != null && <span className="text-[10px] text-fg-mute font-normal">{relativeTime(ts)}</span>}
        </div>
      )}
    </div>
  );
}

export const Message = memo(function Message({
  role,
  content,
  contentParts,
  composerContent,
  ts,
  streaming,
  messageId,
  metadata,
  onFork,
  forkPending = false,
  forkLoading = false,
  onOpenFilePreview,
  onLocateInFileTree,
  continuation,
  showFooter = true,
  showAceCodeAvatar = false,
  messageAutoCollapse = true,
  annotationPresentations = null,
}) {
  useTranslation();
  // normalize 对 text 部件逐字符归一换行;超长旧消息不能每次渲染都重跑一遍。
  const orderedSource = role === 'user' ? (composerContent || metadata?.composer_content) : null;
  const orderedContent = useMemo(
    () => (orderedSource ? normalizeComposerContent(orderedSource) : null),
    [orderedSource],
  );
  if (role === 'user') {
    // expand-webui-skill-commands:daemon 把 /<skill> args 在送给 LLM 前展开为
    // 轻量提示;原文存到 metadata.display_text,UI 优先显示原文,不让用户看到
    // 内部展开。
    const hasDisplayText = metadata && typeof metadata.display_text === 'string'
      && (metadata.display_text.length > 0 || metadata.selection_context_expanded);
    const displayContent = hasDisplayText
      ? metadata.display_text
      : content;
    return <UserBubble content={displayContent} contentParts={contentParts} ts={ts}
                        composerContent={orderedContent}
                        onOpenFilePreview={onOpenFilePreview}
                        onLocateInFileTree={onLocateInFileTree}
                        messageId={messageId}
                        onFork={onFork}
                        forkPending={forkPending}
                        forkLoading={forkLoading}
                        showFooter={showFooter}
                        annotationPresentations={annotationPresentations} />;
  }
  if (role === 'assistant') {
    return <AssistantBubble content={content} contentParts={contentParts}
                             ts={ts} streaming={streaming}
                             messageId={messageId} onFork={onFork}
                             forkPending={forkPending} forkLoading={forkLoading}
                             onOpenFilePreview={onOpenFilePreview}
                             onLocateInFileTree={onLocateInFileTree}
                             continuation={continuation}
                             showFooter={showFooter}
                             showAceCodeAvatar={showAceCodeAvatar}
                             annotationPresentations={annotationPresentations} />;
  }
  if (role === 'error') {
    return <ErrorRow
      content={content}
      ts={ts}
      messageId={messageId}
      onFork={onFork}
      forkPending={forkPending}
      forkLoading={forkLoading}
      showFooter={showFooter}
    />;
  }
  return <SystemRow role={role} content={content} metadata={metadata} />;
});
