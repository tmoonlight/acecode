import {
  forwardRef,
  useCallback,
  useEffect,
  useImperativeHandle,
  useMemo,
  useRef,
} from 'react';
import {
  createEditor,
  Editor,
  Range,
  Transforms,
} from 'slate';
import {
  Editable,
  ReactEditor,
  Slate,
  withReact,
} from 'slate-react';
import {
  HistoryEditor,
  withHistory,
} from 'slate-history';
import { clsx } from '../lib/format.js';
import {
  clipboardHasRichText,
  composerAdjacentAttachmentKey,
  composerAdjacentTagDeletionRange,
  composerAttachmentItemsSignature,
  composerAttachmentTagsSignature,
  composerDocumentFromText,
  composerDocumentWithSynchronizedAttachments,
  composerDocumentWithSynchronizedLeadingCommand,
  composerLeadingCommandSignature,
  composerPlainTextRangeFromSelection,
  composerSelectionFromPlainTextRange,
  composerTextFromDocument,
  isComposerAttachmentTag,
  isComposerCommandTag,
  isComposerInlineTag,
  isComposerPathTag,
  isComposerSessionTag,
  normalizeComposerPlainText,
  plainTextFromClipboardData,
} from '../lib/richComposerModel.js';
import { filesFromClipboardEvent, filesFromTransfer } from '../lib/composerFileTransfer.js';
import {
  RICH_COMPOSER_CONTEXT_PASTE_ACTIONS,
  RICH_COMPOSER_CONTEXT_PASTE_EVENT,
} from '../lib/richComposerContextPaste.js';
import { slashCommandKindPresentation } from '../lib/slashCommands.js';
import { CommandGlyph, FileTypeIcon, VsIcon } from './Icon.jsx';

function withComposerInlineTags(editor) {
  const { isInline, isVoid, markableVoid } = editor;
  editor.isInline = (element) => (
    isComposerInlineTag(element) ? true : isInline(element)
  );
  editor.isVoid = (element) => (
    isComposerInlineTag(element) ? true : isVoid(element)
  );
  editor.markableVoid = (element) => (
    isComposerInlineTag(element) ? false : markableVoid?.(element) || false
  );
  return editor;
}

function commandTagTitle(command) {
  const presentation = slashCommandKindPresentation(command);
  return command?.description || presentation.label || command?.name || command?.token || '';
}

function CommandTagElement({ attributes, children, element }) {
  const displayName = String(element?.name || element?.token || '').replace(/^\/+/, '');
  return (
    <span
      {...attributes}
      contentEditable={false}
      draggable={false}
      data-composer-inline-tag="command"
      data-slash-chip-kind={element?.kind || 'skill'}
      className="ace-cmd-token ace-slate-inline-tag"
      title={commandTagTitle(element)}
      onDragStart={(event) => event.preventDefault()}
    >
      {children}
      <CommandGlyph kind={element?.kind || 'skill'} size={12} className="ace-cmd-token-glyph" />
      <span className="ace-cmd-token-name">{displayName}</span>
    </span>
  );
}

function PathTagElement({ attributes, children, element }) {
  const path = String(element?.path || element?.token || '').replace(/^@(?:"(.*)"|(.*))$/, '$1$2');
  return (
    <span
      {...attributes}
      contentEditable={false}
      draggable={false}
      data-composer-inline-tag="path"
      className="ace-cmd-token ace-slate-inline-tag ace-slate-path-tag"
      title={element?.token || path}
      onDragStart={(event) => event.preventDefault()}
    >
      {children}
      {element?.directory
        ? <VsIcon name="folder" size={12} className="ace-cmd-token-glyph" />
        : <FileTypeIcon path={path} size={12} className="ace-cmd-token-glyph" />}
      <span className="ace-cmd-token-name">{path}</span>
    </span>
  );
}

function SessionTagElement({ attributes, children, element }) {
  const title = String(element?.title || element?.sessionId || '');
  const workspaceName = String(element?.workspaceName || '');
  return (
    <span
      {...attributes}
      contentEditable={false}
      draggable={false}
      data-composer-inline-tag="session"
      className="ace-cmd-token ace-slate-inline-tag ace-slate-session-tag"
      title={workspaceName ? `${title} · ${workspaceName}` : title}
      onDragStart={(event) => event.preventDefault()}
    >
      {children}
      <VsIcon name="newSession" size={12} className="ace-cmd-token-glyph" />
      <span className="ace-cmd-token-name">{title}</span>
    </span>
  );
}

function AttachmentTagElement({
  attributes,
  children,
  element,
  onPreviewAttachment,
  onRemoveAttachment,
}) {
  const name = String(element?.name || 'attachment');
  const label = element?.uploading ? `${name} 上传中` : name;
  const previewable = element?.kind === 'image' && !!element?.url;
  const attachmentKey = String(element?.attachmentKey || '');
  return (
    <span
      {...attributes}
      contentEditable={false}
      draggable={false}
      data-composer-inline-tag="attachment"
      data-desktop-attachment-id={`composer:${attachmentKey}`}
      data-desktop-attachment-name={name}
      data-desktop-attachment-url={element?.url || undefined}
      data-desktop-attachment-path={element?.path || undefined}
      data-desktop-attachment-preview-url={element?.url || undefined}
      data-desktop-attachment-mutable="true"
      className={clsx(
        'group ace-cmd-token ace-slate-inline-tag ace-slate-attachment-tag',
        element?.uploading && 'is-uploading',
        previewable && 'is-previewable',
      )}
      title={element?.sourcePath || name}
      onMouseDown={(event) => {
        if (event.button === 0) event.preventDefault();
      }}
      onClick={previewable ? () => onPreviewAttachment?.(element) : undefined}
      onDragStart={(event) => event.preventDefault()}
    >
      {children}
      <FileTypeIcon path={name} size={12} className="ace-cmd-token-glyph" />
      <span className="ace-cmd-token-name ace-slate-attachment-name">{label}</span>
      <button
        type="button"
        contentEditable={false}
        className="ace-slate-attachment-remove"
        aria-label="移除附件"
        onMouseDown={(event) => {
          event.preventDefault();
          event.stopPropagation();
        }}
        onClick={(event) => {
          event.preventDefault();
          event.stopPropagation();
          onRemoveAttachment?.(attachmentKey);
        }}
      >
        <VsIcon name="close" size={9} />
      </button>
    </span>
  );
}

function ComposerElement({ onPreviewAttachment, onRemoveAttachment, ...props }) {
  if (isComposerAttachmentTag(props.element)) {
    return (
      <AttachmentTagElement
        {...props}
        onPreviewAttachment={onPreviewAttachment}
        onRemoveAttachment={onRemoveAttachment}
      />
    );
  }
  if (isComposerCommandTag(props.element)) return <CommandTagElement {...props} />;
  if (isComposerPathTag(props.element)) return <PathTagElement {...props} />;
  if (isComposerSessionTag(props.element)) return <SessionTagElement {...props} />;
  return (
    <div {...props.attributes} className="ace-slate-composer-paragraph">
      {props.children}
    </div>
  );
}

function currentPlainSelection(document, selection) {
  if (!selection) {
    const end = composerTextFromDocument(document).length;
    return { start: end, end, direction: 'none' };
  }
  return composerPlainTextRangeFromSelection(document, selection);
}

function replaceEditorDocument(editor, nextDocument, {
  selection = null,
  selectEnd = true,
  clearHistory = false,
} = {}) {
  const nextText = composerTextFromDocument(nextDocument);
  const plainSelection = selection || {
    start: selectEnd ? nextText.length : 0,
    end: selectEnd ? nextText.length : 0,
    direction: 'none',
  };
  const slateSelection = composerSelectionFromPlainTextRange(
    nextDocument,
    plainSelection.start,
    plainSelection.end,
    plainSelection.direction,
  );

  HistoryEditor.withoutSaving(editor, () => {
    Editor.withoutNormalizing(editor, () => {
      if (editor.selection) Transforms.deselect(editor);
      for (let index = editor.children.length - 1; index >= 0; index -= 1) {
        Transforms.removeNodes(editor, { at: [index] });
      }
      Transforms.insertNodes(editor, nextDocument, { at: [0] });
      Transforms.select(editor, slateSelection);
    });
  });

  if (clearHistory && HistoryEditor.isHistoryEditor(editor)) {
    editor.history.undos.splice(0);
    editor.history.redos.splice(0);
  }
}

function deleteAdjacentTag(editor, direction) {
  const range = composerAdjacentTagDeletionRange(editor.children, editor.selection, direction);
  if (!range) return false;
  Transforms.select(editor, composerSelectionFromPlainTextRange(
    editor.children,
    range.start,
    range.end,
  ));
  Transforms.delete(editor);
  return true;
}

function insertPlainText(editor, text) {
  const parts = normalizeComposerPlainText(text).split('\n');
  HistoryEditor.withNewBatch(editor, () => {
    parts.forEach((part, index) => {
      if (index > 0) editor.insertBreak();
      if (part) Transforms.insertText(editor, part);
    });
  });
}

function deleteSelectedPlainText(editor) {
  if (!editor.selection || Range.isCollapsed(editor.selection)) return false;
  const selection = composerPlainTextRangeFromSelection(editor.children, editor.selection);
  if (selection.start === selection.end) return false;
  Transforms.select(editor, composerSelectionFromPlainTextRange(
    editor.children,
    selection.start,
    selection.end,
    selection.direction,
  ));
  Transforms.delete(editor);
  return true;
}

function writeSelectedPlainText(event, editor) {
  if (!editor.selection || Range.isCollapsed(editor.selection)) return false;
  const text = composerTextFromDocument(editor.children);
  const selection = composerPlainTextRangeFromSelection(editor.children, editor.selection);
  try {
    event.clipboardData?.setData('text/plain', text.slice(selection.start, selection.end));
  } catch {
    return false;
  }
  event.preventDefault();
  return true;
}

function RichComposerShell({
  value,
  commands,
  attachments = [],
  disabled,
  placeholder,
  className,
  placeholderClassName,
  style,
  onChange,
  onKeyDown,
  onCompositionStart,
  onCompositionEnd,
  onSubmit,
  onPasteFiles,
  onPasteFilesystemItems,
  onPreviewAttachment,
  onRemoveAttachment,
  allowNativeFilesystemDrop = false,
  isComposingKeyEvent,
  onSelectionChange,
}, ref) {
  const commandsRef = useRef(commands);
  commandsRef.current = commands;
  const attachmentsRef = useRef(attachments);
  attachmentsRef.current = attachments;
  const initialValueRef = useRef(null);
  if (!initialValueRef.current) {
    initialValueRef.current = composerDocumentFromText(value, commands, attachments);
  }
  const editor = useMemo(
    () => withComposerInlineTags(withHistory(withReact(createEditor()))),
    [],
  );
  const editableRef = useRef(null);
  const latestTextRef = useRef(composerTextFromDocument(initialValueRef.current));
  const selectionRef = useRef({
    start: latestTextRef.current.length,
    end: latestTextRef.current.length,
    direction: 'none',
  });

  const commandSignature = useMemo(
    () => (Array.isArray(commands) ? commands : [])
      .map((command) => [
        command?.token || '',
        command?.name || '',
        command?.kind || '',
        command?.description || '',
      ].join(':'))
      .join('\n'),
    [commands],
  );
  const attachmentSignature = useMemo(
    () => composerAttachmentItemsSignature(attachments),
    [attachments],
  );

  const publishSelection = useCallback((selection = editor.selection) => {
    const next = currentPlainSelection(editor.children, selection);
    selectionRef.current = next;
    onSelectionChange?.(next);
  }, [editor, onSelectionChange]);

  const handleContextPasteAction = useCallback((event) => {
    const detail = event?.detail;
    if (detail?.action === RICH_COMPOSER_CONTEXT_PASTE_ACTIONS.CAPTURE_SELECTION) {
      detail.selection = currentPlainSelection(editor.children, editor.selection);
      detail.handled = true;
      return;
    }
    if (detail?.action !== RICH_COMPOSER_CONTEXT_PASTE_ACTIONS.INSERT_TEXT) return;

    // RichComposer owns this contenteditable. Consume the action even while
    // disabled so the generic context-menu fallback never mutates Slate's DOM.
    detail.handled = true;
    if (disabled) return;

    if (detail.selection) {
      Transforms.select(editor, composerSelectionFromPlainTextRange(
        editor.children,
        detail.selection.start,
        detail.selection.end,
        detail.selection.direction,
      ));
    }

    let focused = false;
    try {
      ReactEditor.focus(editor);
      focused = true;
    } catch {
      // The menu can close in the same frame as a Slate render. Retry after
      // React has reconciled the editable DOM rather than writing into it.
    }
    if (detail.text) insertPlainText(editor, detail.text);
    if (!focused) {
      window.requestAnimationFrame(() => {
        try { ReactEditor.focus(editor); } catch {}
      });
    }
  }, [disabled, editor]);

  useEffect(() => {
    const editable = editableRef.current;
    if (!editable) return undefined;
    editable.addEventListener(RICH_COMPOSER_CONTEXT_PASTE_EVENT, handleContextPasteAction);
    return () => {
      editable.removeEventListener(RICH_COMPOSER_CONTEXT_PASTE_EVENT, handleContextPasteAction);
    };
  }, [handleContextPasteAction]);

  useEffect(() => {
    const nextText = normalizeComposerPlainText(value);
    const currentDocument = editor.children;
    const currentText = composerTextFromDocument(currentDocument);
    const textChanged = currentText !== nextText;
    const attachmentsChanged = composerAttachmentTagsSignature(currentDocument)
      !== attachmentSignature;
    let nextDocument = null;

    if (textChanged) {
      nextDocument = composerDocumentFromText(nextText, commands, attachments);
    } else {
      const withAttachments = attachmentsChanged
        ? composerDocumentWithSynchronizedAttachments(currentDocument, attachments)
        : currentDocument;
      const synchronized = composerDocumentWithSynchronizedLeadingCommand(
        withAttachments,
        nextText,
        commands,
      );
      if (
        attachmentsChanged
        || composerAttachmentTagsSignature(synchronized)
          !== composerAttachmentTagsSignature(currentDocument)
        || composerLeadingCommandSignature(synchronized)
          !== composerLeadingCommandSignature(currentDocument)
      ) {
        nextDocument = synchronized;
      }
    }

    if (!nextDocument) return;
    const preservedSelection = textChanged
      ? null
      : currentPlainSelection(currentDocument, editor.selection);
    replaceEditorDocument(editor, nextDocument, {
      selection: preservedSelection,
      selectEnd: true,
      clearHistory: textChanged,
    });
    latestTextRef.current = nextText;
    publishSelection(editor.selection);
  }, [attachmentSignature, attachments, commandSignature, commands, editor, publishSelection, value]);

  const handleValueChange = useCallback((nextDocument) => {
    const text = composerTextFromDocument(nextDocument);
    latestTextRef.current = text;
    publishSelection(editor.selection);
    onChange?.(text);
  }, [editor, onChange, publishSelection]);

  const handleSlateSelectionChange = useCallback((selection) => {
    publishSelection(selection);
  }, [publishSelection]);

  useImperativeHandle(ref, () => ({
    focus() {
      const focusEditor = () => {
        if (!editor.selection) {
          const end = latestTextRef.current.length;
          Transforms.select(editor, composerSelectionFromPlainTextRange(editor.children, end, end));
        }
        ReactEditor.focus(editor);
      };
      try {
        focusEditor();
      } catch {
        // 外部草稿刚替换 Slate 文档时，React 树和 Slate DOM 映射可能相差一帧。
        // 延迟重试避免开场白回填成功却留下 Cannot resolve a DOM node 错误。
        window.requestAnimationFrame(() => {
          try { focusEditor(); } catch {}
        });
      }
    },
    setSelectionRange(start, end, direction) {
      const selection = composerSelectionFromPlainTextRange(
        editor.children,
        start,
        Number.isFinite(end) ? end : start,
        direction,
      );
      Transforms.select(editor, selection);
      publishSelection(selection);
    },
    get value() {
      return latestTextRef.current;
    },
    get selectionStart() {
      return selectionRef.current.start;
    },
    get selectionEnd() {
      return selectionRef.current.end;
    },
    get selectionDirection() {
      return selectionRef.current.direction || 'none';
    },
    getEditorStateText() {
      return latestTextRef.current;
    },
    replaceText(next, { selectEnd = true } = {}) {
      const nextDocument = composerDocumentFromText(
        next,
        commandsRef.current,
        attachmentsRef.current,
      );
      replaceEditorDocument(editor, nextDocument, {
        selectEnd,
        clearHistory: true,
      });
      latestTextRef.current = composerTextFromDocument(editor.children);
      publishSelection(editor.selection);
    },
  }), [editor, publishSelection]);

  const renderElement = useCallback((props) => (
    <ComposerElement
      {...props}
      onPreviewAttachment={onPreviewAttachment}
      onRemoveAttachment={onRemoveAttachment}
    />
  ), [onPreviewAttachment, onRemoveAttachment]);
  const renderPlaceholder = useCallback(({ attributes, children }) => (
    <span
      {...attributes}
      className={clsx(
        'pointer-events-none absolute inset-0 leading-[20px] font-sans text-fg-mute',
        placeholderClassName,
      )}
    >
      {children}
    </span>
  ), [placeholderClassName]);

  const handleKeyDown = useCallback((event) => {
    onKeyDown?.(event);
    if (event.defaultPrevented) return;
    if (disabled) {
      event.preventDefault();
      return;
    }

    if (event.key === 'Enter' && !event.shiftKey) {
      if (
        isComposingKeyEvent?.(event)
        || event.isComposing
        || event.nativeEvent?.isComposing
        || event.keyCode === 229
        || ReactEditor.isComposing(editor)
      ) {
        return;
      }
      event.preventDefault();
      onSubmit?.();
      return;
    }

    if (
      (event.key === 'Backspace' || event.key === 'Delete')
      && editor.selection
      && !Range.isCollapsed(editor.selection)
      && deleteSelectedPlainText(editor)
    ) {
      event.preventDefault();
      return;
    }

    if (event.key === 'Backspace') {
      const attachmentKey = composerAdjacentAttachmentKey(
        editor.children,
        editor.selection,
        'backward',
      );
      if (attachmentKey && onRemoveAttachment) {
        event.preventDefault();
        onRemoveAttachment(attachmentKey);
        return;
      }
    }
    if (event.key === 'Delete') {
      const attachmentKey = composerAdjacentAttachmentKey(
        editor.children,
        editor.selection,
        'forward',
      );
      if (attachmentKey && onRemoveAttachment) {
        event.preventDefault();
        onRemoveAttachment(attachmentKey);
        return;
      }
    }

    if (event.key === 'Backspace' && deleteAdjacentTag(editor, 'backward')) {
      event.preventDefault();
      return;
    }
    if (event.key === 'Delete' && deleteAdjacentTag(editor, 'forward')) {
      event.preventDefault();
    }
  }, [disabled, editor, isComposingKeyEvent, onKeyDown, onRemoveAttachment, onSubmit]);

  const handlePaste = useCallback((event) => {
    if (disabled) {
      event.preventDefault();
      return;
    }
    const clipboardData = event.clipboardData || event.nativeEvent?.clipboardData;
    const files = filesFromClipboardEvent(event);
    const text = plainTextFromClipboardData(clipboardData);
    const hasRichText = clipboardHasRichText(clipboardData);
    const handlesFilesystemItems = typeof onPasteFilesystemItems === 'function';
    if (files.length === 0 && !text && !hasRichText && !handlesFilesystemItems) return;
    event.preventDefault();
    event.stopPropagation();
    if (handlesFilesystemItems) {
      let uriList = '';
      try { uriList = clipboardData?.getData?.('text/uri-list') || ''; } catch { /* ignored */ }
      onPasteFilesystemItems({ files, uriList });
    } else if (files.length > 0) {
      onPasteFiles?.(files);
    }
    if (text) insertPlainText(editor, text);
  }, [disabled, editor, onPasteFiles, onPasteFilesystemItems]);

  const handleCopy = useCallback((event) => {
    writeSelectedPlainText(event, editor);
  }, [editor]);

  const handleCut = useCallback((event) => {
    if (disabled || !writeSelectedPlainText(event, editor)) return;
    deleteSelectedPlainText(editor);
  }, [disabled, editor]);

  const handleDrop = useCallback((event) => {
    const files = filesFromTransfer(event.dataTransfer);
    const types = Array.from(event.dataTransfer?.types || []);
    if (
      types.includes('application/x-slate-fragment')
      || (files.length > 0 && !allowNativeFilesystemDrop)
    ) {
      event.preventDefault();
    }
  }, [allowNativeFilesystemDrop]);

  return (
    <Slate
      editor={editor}
      initialValue={initialValueRef.current}
      onValueChange={handleValueChange}
      onSelectionChange={handleSlateSelectionChange}
    >
      <Editable
        ref={editableRef}
        aria-label={placeholder}
        aria-disabled={disabled ? 'true' : undefined}
        readOnly={disabled}
        placeholder={placeholder}
        className={className}
        style={style}
        renderElement={renderElement}
        renderPlaceholder={renderPlaceholder}
        onKeyDown={handleKeyDown}
        onPaste={handlePaste}
        onCopy={handleCopy}
        onCut={handleCut}
        onDrop={handleDrop}
        onCompositionStart={onCompositionStart}
        onCompositionEnd={onCompositionEnd}
        spellCheck
      />
    </Slate>
  );
}

export const RichComposer = forwardRef(RichComposerShell);
