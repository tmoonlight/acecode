import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import hljs from 'highlight.js/lib/core';
import { renderAsync } from 'docx-preview';
import 'x-data-spreadsheet/dist/xspreadsheet.css';
import 'x-data-spreadsheet/dist/xspreadsheet.js';
import { ApiError } from '../lib/api.js';
import { isDesktopShell } from '../lib/desktopShellMode.js';
import { langForFile } from '../lib/lang.js';
import { renderMarkdown } from '../lib/markdown.js';
import { parseWorkbookArrayBuffer } from '../lib/officePreview.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import { resolveSelectionSourcePath } from '../lib/selectionChatContext.js';
import {
  normalizeSelectionSourceText,
  selectionSourceContentRevision,
} from '../lib/selectionSourceDecorations.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { clsx, formatBytes } from '../lib/format.js';
import { filePreviewKind, isBlobFilePreview } from '../lib/filePreviewKind.js';
import {
  captureFilePreviewScroll,
  restoredFilePreviewScroll,
} from '../lib/filePreviewScroll.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import MarkdownWysiwygEditor from './MarkdownWysiwygEditor.jsx';
import { SelectionAnnotationOverlay } from './SelectionAnnotationOverlay.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

async function copyWithToast(text, okText) {
  const result = await copyTextToSystemClipboard(text);
  if (result.ok) toast({ kind: 'ok', text: okText });
  else toast({ kind: 'err', text: '复制失败:' + (result.error || '') });
}

function editableFileError(error, fallback = '无法编辑此文件') {
  if (!(error instanceof ApiError)) return error?.message || fallback;
  const body = error.body;
  if (body && typeof body === 'object') {
    if (body.error === 'file changed') return '磁盘内容已变化，未覆盖文件；请保留当前草稿并重新载入后再合并';
    if (body.error === 'file too large') return '文件过大，无法在详情中编辑';
    if (body.error === 'binary' || body.error === 'unsupported encoding') return '文件编码不受支持，只能只读预览';
    if (body.error === 'unknown workspace') return '该文件不在当前工作区或 worktree 中，只能只读预览';
    if (body.error === 'not found') return '文件不存在';
    if (body.error) return String(body.error);
  }
  return `${fallback} (HTTP ${error.status})`;
}

function editableStatePatch(result, { editing = true } = {}) {
  const text = String(result?.text ?? '');
  return {
    editing,
    loading: false,
    saving: false,
    baselineText: text,
    text,
    readId: String(result?.read_id || ''),
    encoding: String(result?.encoding || ''),
    lineEnding: String(result?.line_ending || ''),
    hasBom: result?.has_bom === true,
    size: Number(result?.size || 0),
    externalChanged: false,
    error: '',
  };
}

export function FilePreviewContent({
  api,
  cwd,
  path,
  focusLine = null,
  focusLineRevision = 0,
  reloadRevision = 0,
  wrapPreview,
  selectionContexts = [],
  editState = null,
  onEditStateChange,
  onToggleWrapPreview,
  onRefresh,
}) {
  const [state, setState] = useState({
    status: 'idle',
    kind: 'text',
    text: '',
    error: null,
    lang: '',
    size: 0,
    previewUrl: '',
    contentType: '',
    blob: null,
  });
  const [markdownSource, setMarkdownSource] = useState(false);
  const [imagePreview, setImagePreview] = useState(null);
  const previewScrollRef = useRef(null);
  const loadedIdentityRef = useRef('');
  const pendingScrollSnapshotRef = useRef(null);
  const handledFocusRequestRef = useRef('');
  const editStateRef = useRef(editState);
  const onEditStateChangeRef = useRef(onEditStateChange);
  editStateRef.current = editState;
  onEditStateChangeRef.current = onEditStateChange;
  const sourceContentRevision = useMemo(
    () => (state.status === 'ok' && (state.kind === 'text' || state.kind === 'markdown')
      ? selectionSourceContentRevision(state.text)
      : ''),
    [state.kind, state.status, state.text],
  );

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'preview' || target.path !== path) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.REFRESH_DETAILS) {
        detail.handled = true;
        onRefresh?.();
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_TEXT) {
        detail.handled = true;
        if (!state.text) {
          toast({ kind: 'info', text: '没有可复制的预览文本' });
          return;
        }
        copyWithToast(state.text, '已复制预览内容');
      } else if (action === DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_METADATA) {
        detail.handled = true;
        const metadata = [
          `path: ${path}`,
          `kind: ${state.kind}`,
          `size: ${state.size}`,
          state.contentType ? `content-type: ${state.contentType}` : '',
          state.lang ? `language: ${state.lang}` : '',
        ].filter(Boolean).join('\n');
        copyWithToast(metadata, '已复制预览信息');
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [onRefresh, path, state.contentType, state.kind, state.lang, state.size, state.text]);

  useEffect(() => {
    const identity = `${cwd || ''}\u0000${path || ''}`;
    if (!cwd || !path) {
      loadedIdentityRef.current = identity;
      pendingScrollSnapshotRef.current = null;
      setState({ status: 'idle', kind: 'text', text: '', error: null, lang: '', size: 0, previewUrl: '', contentType: '', blob: null });
      return undefined;
    }
    const reloadingSameFile = loadedIdentityRef.current === identity;
    if (reloadingSameFile) {
      const currentScroll = previewScrollRef.current;
      if (currentScroll) pendingScrollSnapshotRef.current = captureFilePreviewScroll(currentScroll);
    } else {
      pendingScrollSnapshotRef.current = null;
    }
    loadedIdentityRef.current = identity;
    let cancelled = false;
    let objectUrl = '';
    const nextKind = filePreviewKind(path);
    if (!reloadingSameFile) setMarkdownSource(false);
    setImagePreview(null);
    setState({ status: 'loading', kind: nextKind, text: '', error: null, lang: '', size: 0, previewUrl: '', contentType: '', blob: null });

    if (nextKind === 'unsupported') {
      pendingScrollSnapshotRef.current = null;
      setState({
        status: 'error',
        kind: nextKind,
        text: '',
        error: '该文件格式暂不支持预览',
        lang: '',
        size: 0,
        previewUrl: '',
        contentType: '',
        blob: null,
      });
      return () => {
        cancelled = true;
      };
    }

    if (isBlobFilePreview(path)) {
      api.readFileBlob(cwd, path).then((blob) => {
        if (cancelled) return;
        if (nextKind === 'image' || nextKind === 'pdf') {
          objectUrl = URL.createObjectURL(blob);
        }
        setState({
          status: 'ok',
          kind: nextKind,
          text: '',
          error: null,
          lang: '',
          size: blob.size || 0,
          previewUrl: objectUrl,
          contentType: blob.type || '',
          blob,
        });
      }).catch((err) => {
        if (cancelled) return;
        pendingScrollSnapshotRef.current = null;
        let msg = '读取失败';
        let extraSize = 0;
        if (err instanceof ApiError) {
          const body = err.body;
          if (body && typeof body === 'object') {
            if (body.error === 'file too large') {
              extraSize = Number(body.size || 0);
              msg = `文件过大 (${formatBytes(extraSize)}),无法在浏览器内预览`;
            } else if (body.error === 'unsupported file type') msg = '该文件格式暂不支持预览';
            else if (body.error === 'not found') msg = '文件不存在';
            else if (body.error) msg = body.error;
          } else {
            msg = `读取失败 (HTTP ${err.status})`;
          }
        }
        setState({ status: 'error', kind: nextKind, text: '', error: msg, lang: '', size: extraSize, previewUrl: '', contentType: '', blob: null });
      });
      return () => {
        cancelled = true;
        if (objectUrl) URL.revokeObjectURL(objectUrl);
      };
    }

    const currentEdit = editStateRef.current;
    if ((nextKind === 'text' || nextKind === 'markdown') && currentEdit?.editing) {
      if (currentEdit.dirty) {
        const baselineText = String(currentEdit.baselineText ?? '');
        setState({
          status: 'ok',
          kind: nextKind,
          text: baselineText,
          error: null,
          lang: langForFile(path),
          size: Number(currentEdit.size || baselineText.length),
          previewUrl: '',
          contentType: '',
          blob: null,
        });
        return () => { cancelled = true; };
      }
      api.readEditableFile(cwd, path).then((result) => {
        if (cancelled) return;
        const text = String(result?.text ?? '');
        setState({
          status: 'ok',
          kind: nextKind,
          text,
          error: null,
          lang: langForFile(path),
          size: Number(result?.size || text.length),
          previewUrl: '',
          contentType: '',
          blob: null,
        });
        onEditStateChangeRef.current?.(editableStatePatch(result));
      }).catch((err) => {
        if (cancelled) return;
        const baselineText = String(currentEdit.baselineText ?? '');
        setState({
          status: 'ok',
          kind: nextKind,
          text: baselineText,
          error: null,
          lang: langForFile(path),
          size: Number(currentEdit.size || baselineText.length),
          previewUrl: '',
          contentType: '',
          blob: null,
        });
        onEditStateChangeRef.current?.({
          loading: false,
          error: editableFileError(err, '重新读取失败'),
        });
      });
      return () => { cancelled = true; };
    }

    api.readFile(cwd, path).then((text) => {
      if (cancelled) return;
      setState({
        status: 'ok',
        kind: nextKind,
        text,
        error: null,
        lang: langForFile(path),
        size: text.length,
        previewUrl: '',
        contentType: '',
        blob: null,
      });
    }).catch((err) => {
      if (cancelled) return;
      pendingScrollSnapshotRef.current = null;
      let msg = '读取失败';
      let extraSize = 0;
      if (err instanceof ApiError) {
        const body = err.body;
        if (body && typeof body === 'object') {
          if (body.error === 'binary') msg = '二进制文件,无法预览';
          else if (body.error === 'file too large') {
            extraSize = Number(body.size || 0);
            msg = `文件过大 (${formatBytes(extraSize)}),无法在浏览器内预览`;
          } else if (body.error === 'not found') msg = '文件不存在';
          else if (body.error) msg = body.error;
        } else {
          msg = `读取失败 (HTTP ${err.status})`;
        }
      }
      setState({ status: 'error', kind: nextKind, text: '', error: msg, lang: '', size: extraSize, previewUrl: '', contentType: '', blob: null });
    });
    return () => { cancelled = true; };
  }, [api, cwd, path, reloadRevision]);

  const beginEditing = useCallback(async () => {
    if (!cwd || !path || editStateRef.current?.loading) return;
    onEditStateChangeRef.current?.({ loading: true, error: '' });
    try {
      const result = await api.readEditableFile(cwd, path);
      const text = String(result?.text ?? '');
      setState((previous) => ({
        ...previous,
        status: 'ok',
        text,
        size: Number(result?.size || text.length),
      }));
      onEditStateChangeRef.current?.(editableStatePatch(result));
    } catch (error) {
      const message = editableFileError(error);
      onEditStateChangeRef.current?.({ loading: false, error: message });
      toast({ kind: 'err', text: message });
    }
  }, [api, cwd, path]);

  const saveEditing = useCallback(async () => {
    const current = editStateRef.current;
    if (!current?.editing || current.saving || !current.dirty || !current.readId) return;
    const text = String(current.text ?? '');
    onEditStateChangeRef.current?.({ saving: true, error: '' });
    try {
      const result = await api.saveEditableFile(cwd, path, text, current.readId);
      setState((previous) => ({
        ...previous,
        text,
        size: Number(result?.size || text.length),
      }));
      onEditStateChangeRef.current?.({
        ...editableStatePatch({ ...result, text }),
        editing: true,
      });
      toast({ kind: 'ok', text: '文件已保存' });
    } catch (error) {
      const message = editableFileError(error, '保存失败');
      onEditStateChangeRef.current?.({
        saving: false,
        externalChanged: error instanceof ApiError && error.status === 409,
        error: message,
      });
      toast({ kind: 'err', text: message });
    }
  }, [api, cwd, path]);

  const cancelEditing = useCallback(() => {
    const current = editStateRef.current;
    const baselineText = String(current?.baselineText ?? state.text ?? '');
    setState((previous) => ({ ...previous, text: baselineText, size: baselineText.length }));
    onEditStateChangeRef.current?.({
      editing: false,
      loading: false,
      saving: false,
      text: baselineText,
      externalChanged: false,
      error: '',
    });
  }, [state.text]);

  // 同一文件重新读取时，loading 会临时卸载滚动容器。新 DOM 落地后在布局
  // 阶段恢复位置，并按新内容高度/宽度钳制，避免空文件或大幅删减后越界。
  useLayoutEffect(() => {
    if (state.status !== 'ok') return;
    const snapshot = pendingScrollSnapshotRef.current;
    if (!snapshot) return;
    pendingScrollSnapshotRef.current = null;
    const host = previewScrollRef.current;
    if (!host) return;
    const restored = restoredFilePreviewScroll(snapshot, host);
    host.scrollTop = restored.top;
    host.scrollLeft = restored.left;
  }, [cwd, markdownSource, path, reloadRevision, state.blob, state.kind, state.previewUrl, state.status, state.text]);

  // 聊天正文 foo.md:42 链接定位 markdown 文件时切到源码视图 —— 渲染视图没有行的
  // 概念,滚不到指定行。必须声明在上方加载 effect 之后:同一 commit 内 effect 按
  // 声明序执行,加载 effect 会把 markdownSource 重置为 false,这里要排在它后面
  // set true 才能赢。(移到加载 effect 之前会被重置覆盖,勿动。)
  useEffect(() => {
    if (focusLine != null && filePreviewKind(path) === 'markdown') {
      setMarkdownSource(true);
    }
  }, [focusLine, focusLineRevision, path]);

  // 滚动到 focusLine(高亮由渲染时的 .ace-line-focus class 承担)。依赖说明:
  //   - focusLineRevision:重复点击同一链接 revision 递增,line 相同也重新滚动;
  //   - state.status:首次打开时 effect 先跑在 loading 视图(ref 为 null),内容
  //     加载完成转 ok 后必须重跑一次才真正滚到;
  //   - markdownSource:markdown 文件先切源码视图、行号表挂载后重跑才滚。
  useEffect(() => {
    if (focusLine == null || state.status !== 'ok') return;
    const requestKey = `${cwd || ''}\u0000${path || ''}\u0000${focusLine}\u0000${focusLineRevision}`;
    if (handledFocusRequestRef.current === requestKey) return;
    const host = previewScrollRef.current;
    if (!host) return;
    const row = host.querySelector(`.ace-line-table tbody tr:nth-child(${focusLine})`);
    if (row) {
      handledFocusRequestRef.current = requestKey;
      row.scrollIntoView({ block: 'center' });
    }
  }, [cwd, focusLine, focusLineRevision, markdownSource, path, state.status]);

  if (!path) {
    return <div className="ace-empty-state">未选中文件,请在「文件」中点击一个文件</div>;
  }
  const sourcePath = resolveSelectionSourcePath({ cwd, path });
  const previewAttrs = {
    'data-desktop-preview-path': path || undefined,
    'data-desktop-preview-cwd': cwd || undefined,
    'data-desktop-preview-source-path': sourcePath || path || undefined,
    'data-desktop-preview-kind': state.kind || undefined,
    'data-desktop-preview-size': Number.isFinite(state.size) ? String(state.size) : undefined,
    'data-desktop-preview-content-type': state.contentType || undefined,
    'data-desktop-preview-copy-image-url': state.kind === 'image' ? state.previewUrl || undefined : undefined,
    'data-selection-source-revision': sourceContentRevision || undefined,
  };
  if (state.status === 'loading') {
    return <div className="ace-empty-state" {...previewAttrs}>加载中...</div>;
  }
  if (state.status === 'error') {
    return (
      <div className="ace-empty-state" {...previewAttrs}>
        <div className="text-danger text-[12px] mb-1">{state.error}</div>
        <div className="text-fg-mute text-[10px] opacity-70 break-all">{path}</div>
      </div>
    );
  }
  if (state.status !== 'ok') return null;
  if (state.kind === 'image') {
    return (
      <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
        <button
          type="button"
          className="ace-side-image-preview"
          title="预览图片"
          aria-label="预览图片"
          onClick={() => setImagePreview({ src: state.previewUrl, alt: path })}
        >
          <img src={state.previewUrl} alt={path} draggable="false" />
        </button>
        <ImageLightbox
          preview={imagePreview}
          contextMenuAttrs={previewAttrs}
          onClose={() => setImagePreview(null)}
        />
      </div>
    );
  }
  if (state.kind === 'pdf') {
    return (
      <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
        <object
          className="ace-side-pdf-preview"
          data={state.previewUrl}
          type="application/pdf"
          title={path}
        >
          <div className="ace-empty-state">
            <div className="text-danger text-[12px] mb-1">当前浏览器无法内嵌预览 PDF</div>
            <div className="text-fg-mute text-[10px] opacity-70 break-all">{path}</div>
          </div>
        </object>
      </div>
    );
  }
  if (state.kind === 'word') {
    return (
      <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
        <WordPreview blob={state.blob} path={path} />
      </div>
    );
  }
  if (state.kind === 'spreadsheet') {
    return (
      <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
        <SpreadsheetPreview blob={state.blob} path={path} />
      </div>
    );
  }

  const isMarkdown = state.kind === 'markdown';
  const activeEdit = editState && typeof editState === 'object' ? editState : null;
  if ((state.kind === 'text' || isMarkdown) && activeEdit?.editing) {
    const draftText = String(activeEdit.text ?? activeEdit.baselineText ?? '');
    const statusText = activeEdit.error
      || (activeEdit.externalChanged ? '磁盘内容可能已变化；保存时会再次检查，绝不会静默覆盖' : '')
      || (activeEdit.dirty ? '有未保存的更改' : '所有更改已保存');
    return (
      <div className="ace-file-editor-shell" {...previewAttrs}>
        <div className="ace-file-editor-statusbar">
          <div
            className={clsx(
              'ace-file-editor-status',
              activeEdit.error && 'is-error',
              !activeEdit.error && activeEdit.externalChanged && 'is-warning',
              activeEdit.dirty && 'is-dirty',
            )}
            role={activeEdit.error ? 'alert' : 'status'}
          >
            {statusText}
          </div>
          <div className="ace-file-editor-actions">
            <button
              type="button"
              className="ace-file-editor-button is-primary"
              disabled={!activeEdit.dirty || activeEdit.saving}
              onClick={saveEditing}
            >
              <VsIcon name="save" size={13} />
              {activeEdit.saving ? '保存中...' : '保存'}
            </button>
            <button
              type="button"
              className="ace-file-editor-button"
              disabled={activeEdit.saving}
              onClick={cancelEditing}
            >
              取消
            </button>
          </div>
        </div>
        {isMarkdown ? (
          <MarkdownWysiwygEditor
            value={draftText}
            disabled={activeEdit.saving}
            onChange={(text) => onEditStateChangeRef.current?.({ text, error: '' })}
            onSave={saveEditing}
          />
        ) : (
          <textarea
            className="ace-file-text-editor"
            aria-label={`编辑 ${path}`}
            value={draftText}
            wrap={wrapPreview ? 'soft' : 'off'}
            disabled={activeEdit.saving}
            spellCheck="false"
            onChange={(event) => onEditStateChangeRef.current?.({
              text: event.target.value,
              error: '',
            })}
            onKeyDown={(event) => {
              if (
                (event.ctrlKey || event.metaKey)
                && event.key.toLowerCase() === 's'
              ) {
                event.preventDefault();
                if (!event.isComposing && !event.nativeEvent?.isComposing && event.keyCode !== 229) {
                  saveEditing();
                }
              }
            }}
          />
        )}
      </div>
    );
  }

  const lang = state.lang;
  const normalizedSourceText = normalizeSelectionSourceText(state.text);
  let codeHtml;
  if (lang && hljs.getLanguage(lang)) {
    try {
      codeHtml = hljs.highlight(normalizedSourceText, { language: lang, ignoreIllegals: true }).value;
    } catch {
      codeHtml = escapeHtml(normalizedSourceText);
    }
  } else {
    codeHtml = escapeHtml(normalizedSourceText);
  }
  const lines = codeHtml.split('\n');
  const sourceLines = normalizedSourceText.split('\n');
  const sourceLineStarts = [];
  let sourceOffset = 0;
  for (const line of sourceLines) {
    sourceLineStarts.push(sourceOffset);
    sourceOffset += line.length + 1;
  }
  const gutterW = String(lines.length).length;
  const html = `<table class="ace-line-table"><tbody>${
    lines.map((ln, i) =>
      `<tr${i + 1 === focusLine ? ' class="ace-line-focus"' : ''}><td class="ace-line-no" style="width:${gutterW + 1}ch">${i + 1}</td><td class="ace-line-code" data-source-line="${i + 1}" data-source-start="${sourceLineStarts[i] ?? 0}" data-source-length="${sourceLines[i]?.length ?? 0}">${ln || ' '}</td></tr>`
    ).join('')
  }</tbody></table>`;
  const wrapTitle = wrapPreview ? '关闭自动换行' : '开启自动换行';
  const showMarkdownRendered = isMarkdown && !markdownSource;
  const markdownToggleTitle = markdownSource ? '渲染 Markdown' : '查看 Markdown 原文';
  return (
    <div className="flex-1 flex flex-col overflow-hidden" {...previewAttrs}>
      <CopyableCodeFrame
        text={state.text}
        className="flex-1 min-h-0 ace-side-preview-code"
        data-wrap={wrapPreview ? 'true' : 'false'}
        actions={(
          <>
            {isDesktopShell() && (state.kind === 'text' || isMarkdown) && (
              <button
                type="button"
                className="ace-code-action-btn ace-code-edit-btn"
                title={activeEdit?.error || '编辑文件'}
                aria-label="编辑文件"
                disabled={activeEdit?.loading === true}
                onClick={(event) => { event.stopPropagation(); beginEditing(); }}
              >
                <VsIcon name="edit" size={14} />
              </button>
            )}
            {isMarkdown && (
              <button
                type="button"
                className={clsx('ace-code-action-btn ace-code-markdown-btn', showMarkdownRendered && 'is-active')}
                title={markdownToggleTitle}
                aria-label={markdownToggleTitle}
                aria-pressed={showMarkdownRendered}
                onClick={(event) => { event.stopPropagation(); setMarkdownSource((prev) => !prev); }}
              >
                <VsIcon name={markdownSource ? 'document' : 'code'} size={14} />
              </button>
            )}
            {!showMarkdownRendered && (
              <button
                type="button"
                className={clsx('ace-code-action-btn ace-code-wrap-btn', wrapPreview && 'is-active')}
                title={wrapTitle}
                aria-label={wrapTitle}
                aria-pressed={wrapPreview}
                onClick={(event) => { event.stopPropagation(); onToggleWrapPreview?.(); }}
              >
                <VsIcon name="wordWrap" size={14} />
              </button>
            )}
          </>
        )}
      >
        {showMarkdownRendered ? (
          <>
            <div
              ref={previewScrollRef}
              className="h-full overflow-auto ace-md ace-side-markdown-preview"
              dangerouslySetInnerHTML={{ __html: renderMarkdown(state.text) }}
            />
            <SelectionAnnotationOverlay
              hostRef={previewScrollRef}
              contexts={selectionContexts}
              sourcePath={sourcePath}
              sourceText={normalizedSourceText}
              contentRevision={sourceContentRevision}
              rendered
            />
          </>
        ) : (
          <>
            <div
              ref={previewScrollRef}
              className="h-full overflow-auto text-[11px] ace-preview"
              dangerouslySetInnerHTML={{ __html: html }}
            />
            <SelectionAnnotationOverlay
              hostRef={previewScrollRef}
              contexts={selectionContexts}
              sourcePath={sourcePath}
              sourceText={normalizedSourceText}
              contentRevision={sourceContentRevision}
            />
          </>
        )}
      </CopyableCodeFrame>
    </div>
  );
}

function WordPreview({ blob, path }) {
  const hostRef = useRef(null);
  const [error, setError] = useState('');

  useEffect(() => {
    const host = hostRef.current;
    if (!host || !blob) return undefined;
    let cancelled = false;
    setError('');
    host.replaceChildren();
    renderAsync(blob, host, host, {
      className: 'ace-docx-preview-doc',
      inWrapper: true,
      ignoreWidth: false,
      ignoreHeight: false,
      breakPages: true,
      renderHeaders: true,
      renderFooters: true,
      renderFootnotes: true,
      renderEndnotes: true,
    }).catch((err) => {
      if (cancelled) return;
      setError(err?.message || 'Word 文件预览失败');
    });
    return () => {
      cancelled = true;
      host.replaceChildren();
    };
  }, [blob]);

  if (error) {
    return (
      <div className="ace-empty-state">
        <div className="text-danger text-[12px] mb-1">{error}</div>
        <div className="text-fg-mute text-[10px] opacity-70 break-all">{path}</div>
      </div>
    );
  }

  return <div ref={hostRef} className="ace-side-docx-preview" />;
}

function SpreadsheetPreview({ blob, path }) {
  const hostRef = useRef(null);
  const [error, setError] = useState('');

  useEffect(() => {
    const host = hostRef.current;
    if (!host || !blob) return undefined;
    let cancelled = false;
    let resizeObserver = null;
    let spreadsheet = null;
    setError('');
    host.replaceChildren();

    const render = async () => {
      try {
        const buffer = await blob.arrayBuffer();
        if (cancelled) return;
        const data = parseWorkbookArrayBuffer(buffer);
        const factory = window.x_spreadsheet;
        if (typeof factory !== 'function') {
          throw new Error('x-spreadsheet renderer unavailable');
        }
        spreadsheet = factory(host, {
          mode: 'read',
          showToolbar: false,
          showGrid: true,
          showContextmenu: false,
          showBottomBar: data.length > 1,
          view: {
            height: () => Math.max(240, host.clientHeight || 0),
            width: () => Math.max(320, host.clientWidth || 0),
          },
          row: {
            len: 100,
            height: 24,
          },
          col: {
            len: 26,
            width: 100,
            indexWidth: 46,
            minWidth: 60,
          },
        });
        spreadsheet.loadData(data);
        if (typeof ResizeObserver !== 'undefined') {
          resizeObserver = new ResizeObserver(() => spreadsheet?.reRender?.());
          resizeObserver.observe(host);
        }
      } catch (err) {
        if (cancelled) return;
        setError(err?.message || 'Excel 文件预览失败');
      }
    };
    render();

    return () => {
      cancelled = true;
      resizeObserver?.disconnect();
      host.replaceChildren();
      spreadsheet = null;
    };
  }, [blob]);

  if (error) {
    return (
      <div className="ace-empty-state">
        <div className="text-danger text-[12px] mb-1">{error}</div>
        <div className="text-fg-mute text-[10px] opacity-70 break-all">{path}</div>
      </div>
    );
  }

  return <div ref={hostRef} className="ace-side-spreadsheet-preview" />;
}
