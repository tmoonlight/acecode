import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import hljs from 'highlight.js/lib/core';
import {
  createEditor,
  Editor,
  Node as SlateNode,
  Range as SlateRange,
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
import { slateSelectionDecorationModel } from '../lib/editablePreviewSelection.js';
import { CLEAR_PREVIEW_SELECTION_EVENT } from '../lib/inactiveSelection.js';
import { renderMarkdown, renderMarkdownInline } from '../lib/markdown.js';
import { markdownToSlate, slateToMarkdown } from '../lib/markdownWysiwyg.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';

const EDITABLE_BLOCK_TYPES = new Set([
  'paragraph',
  'heading',
  'blockquote',
  'code-block',
]);

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

function highlightedCodeHtml(value, language) {
  const source = String(value || '');
  if (language && hljs.getLanguage(language)) {
    try {
      return hljs.highlight(source, { language, ignoreIllegals: true }).value;
    } catch { /* Fall back to escaped source. */ }
  }
  return escapeHtml(source);
}

function legalDocument(document) {
  return Array.isArray(document) && document.length > 0
    ? document
    : [{ type: 'paragraph', children: [{ text: '' }] }];
}

function withMarkdownSchema(editor) {
  const { isInline, isVoid, markableVoid } = editor;
  editor.isInline = (element) => (
    ['link', 'opaque-inline', 'hard-break'].includes(element?.type)
      ? true
      : isInline(element)
  );
  editor.isVoid = (element) => (
    ['opaque-inline', 'opaque-block', 'hard-break', 'thematic-break'].includes(element?.type)
      ? true
      : isVoid(element)
  );
  editor.markableVoid = (element) => (
    ['opaque-inline', 'opaque-block', 'hard-break', 'thematic-break'].includes(element?.type)
      ? false
      : markableVoid?.(element) || false
  );
  return editor;
}

function safeDeselect(editor) {
  if (!editor.selection) return;
  try {
    Transforms.deselect(editor);
  } catch {
    editor.selection = null;
  }
}

function replaceDocument(editor, document) {
  const replacement = legalDocument(document);
  try {
    HistoryEditor.withoutSaving(editor, () => {
      Editor.withoutNormalizing(editor, () => {
        safeDeselect(editor);
        const previousCount = Array.isArray(editor.children) ? editor.children.length : 0;
        // Keep at least one root node throughout the operation. WebView2 can
        // otherwise retain a stale IME selection that points into an empty root.
        Transforms.insertNodes(editor, replacement, { at: [previousCount] });
        for (let index = previousCount - 1; index >= 0; index -= 1) {
          Transforms.removeNodes(editor, { at: [index] });
        }
        Transforms.select(editor, Editor.end(editor, []));
      });
    });
    if (HistoryEditor.isHistoryEditor(editor)) {
      editor.history.undos.splice(0);
      editor.history.redos.splice(0);
    }
  } catch {
    editor.children = replacement;
    editor.selection = null;
    try { editor.onChange(); } catch { /* Slate will reconcile on the next render. */ }
  }
}

function OpaqueNode({ attributes, children, element, inline = false }) {
  const Tag = inline ? 'span' : 'div';
  const html = inline
    ? renderMarkdownInline(element.raw || '')
    : renderMarkdown(element.raw || '');
  return (
    <Tag
      {...attributes}
      contentEditable={false}
      className="ace-markdown-opaque"
      title="此 Markdown 结构按原文保留"
    >
      <span className="ace-markdown-opaque-children">{children}</span>
      <Tag
        className="ace-markdown-opaque-rendered"
        dangerouslySetInnerHTML={{ __html: html }}
      />
    </Tag>
  );
}

function MarkdownElement({ attributes, children, element, editor }) {
  switch (element.type) {
    case 'heading': {
      const Tag = `h${Math.min(6, Math.max(1, Number(element.level) || 1))}`;
      return <Tag {...attributes}>{children}</Tag>;
    }
    case 'blockquote':
      return <blockquote {...attributes}>{children}</blockquote>;
    case 'code-block':
      return (
        <CopyableCodeFrame
          {...attributes}
          className="ace-markdown-code-frame"
          text={SlateNode.string(element)}
        >
          <pre className={element.lang ? 'hljs' : undefined}>
            <code
              className="ace-markdown-code-highlight"
              contentEditable={false}
              aria-hidden="true"
              dangerouslySetInnerHTML={{
                __html: highlightedCodeHtml(SlateNode.string(element), element.lang),
              }}
            />
            <code className="ace-markdown-code-input" data-code-copy-source="true">
              {children}
            </code>
          </pre>
        </CopyableCodeFrame>
      );
    case 'list': {
      const Tag = element.ordered ? 'ol' : 'ul';
      return <Tag {...attributes} start={element.ordered ? element.start || undefined : undefined}>{children}</Tag>;
    }
    case 'list-item':
      return (
        <li {...attributes} className={element.checked == null ? undefined : 'task-list-item'}>
          {element.checked == null ? null : (
            <input
              type="checkbox"
              contentEditable={false}
              checked={element.checked === true}
              disabled
              aria-label="任务完成状态"
            />
          )}
          {children}
        </li>
      );
    case 'table':
      return <table {...attributes}><tbody>{children}</tbody></table>;
    case 'table-row':
      return <tr {...attributes}>{children}</tr>;
    case 'table-cell':
      try {
        const path = ReactEditor.findPath(editor, element);
        const Tag = path[path.length - 2] === 0 ? 'th' : 'td';
        return <Tag {...attributes}>{children}</Tag>;
      } catch {
        return <td {...attributes}>{children}</td>;
      }
    case 'thematic-break':
      return <div {...attributes} contentEditable={false}>{children}<hr /></div>;
    case 'link':
      return (
        <a
          {...attributes}
          href={element.url || '#'}
          title={element.title || element.url || ''}
          onClick={(event) => event.preventDefault()}
        >
          {children}
        </a>
      );
    case 'hard-break':
      return <span {...attributes} contentEditable={false}>{children}<br /></span>;
    case 'opaque-inline':
      return <OpaqueNode attributes={attributes} element={element} inline>{children}</OpaqueNode>;
    case 'opaque-block':
      return <OpaqueNode attributes={attributes} element={element}>{children}</OpaqueNode>;
    default:
      return <p {...attributes}>{children}</p>;
  }
}

function MarkdownLeaf({ attributes, children, leaf }) {
  let content = children;
  if (leaf.bold) content = <strong>{content}</strong>;
  if (leaf.italic) content = <em>{content}</em>;
  if (leaf.strike) content = <del>{content}</del>;
  if (leaf.code) content = <code>{content}</code>;
  const decorationId = leaf.selectionDecorationId || '';
  const className = [
    leaf.inactiveSelection ? 'ace-inactive-selection-mark' : '',
    decorationId ? 'ace-selection-reference-mark' : '',
  ].filter(Boolean).join(' ') || undefined;
  const setGroupHovered = (target, hovered) => {
    if (!decorationId) return;
    const root = target?.closest?.('.ace-side-markdown-preview');
    for (const mark of Array.from(root?.querySelectorAll?.('[data-selection-decoration-id]') || [])) {
      if (mark.getAttribute('data-selection-decoration-id') === decorationId) {
        mark.classList.toggle('is-hovered', hovered);
      }
    }
  };
  return (
    <span
      {...attributes}
      className={className}
      data-selection-decoration-id={decorationId || undefined}
      data-selection-annotated={decorationId ? (leaf.selectionAnnotated ? 'true' : 'false') : undefined}
      onMouseEnter={(event) => setGroupHovered(event.currentTarget, true)}
      onMouseLeave={(event) => setGroupHovered(event.currentTarget, false)}
    >
      {content}
    </span>
  );
}

export default function MarkdownWysiwygEditor({
  value,
  onChange,
  onSave,
  disabled = false,
  hostRef = null,
  selectionContexts = [],
  sourcePath = '',
  contentRevision = '',
}) {
  const normalizedValue = String(value ?? '');
  const initialValueRef = useRef(null);
  if (!initialValueRef.current) initialValueRef.current = markdownToSlate(normalizedValue);
  const editor = useMemo(
    () => withMarkdownSchema(withHistory(withReact(createEditor()))),
    [],
  );
  const lastAppliedValueRef = useRef(normalizedValue);
  const lastPublishedValueRef = useRef(null);
  const compositionRef = useRef({ active: false, settling: false });
  const compositionTimerRef = useRef(0);
  const [syncRevision, setSyncRevision] = useState(0);
  const [inactiveSelection, setInactiveSelection] = useState(null);

  useEffect(() => () => {
    if (compositionTimerRef.current) window.clearTimeout(compositionTimerRef.current);
  }, []);

  useEffect(() => {
    const clear = () => {
      setInactiveSelection(null);
      safeDeselect(editor);
    };
    window.addEventListener(CLEAR_PREVIEW_SELECTION_EVENT, clear);
    return () => window.removeEventListener(CLEAR_PREVIEW_SELECTION_EVENT, clear);
  }, [editor]);

  useEffect(() => {
    if (normalizedValue === lastAppliedValueRef.current) return;
    if (normalizedValue === lastPublishedValueRef.current) {
      lastAppliedValueRef.current = normalizedValue;
      return;
    }
    let composing = false;
    try { composing = ReactEditor.isComposing(editor); } catch { /* Not mounted yet. */ }
    if (compositionRef.current.active || compositionRef.current.settling || composing) return;
    replaceDocument(editor, markdownToSlate(normalizedValue));
    lastAppliedValueRef.current = normalizedValue;
    lastPublishedValueRef.current = null;
  }, [editor, normalizedValue, syncRevision]);

  const handleValueChange = useCallback((document) => {
    const contentChanged = editor.operations.some((operation) => operation.type !== 'set_selection');
    if (!contentChanged) return;
    const markdown = slateToMarkdown(legalDocument(document));
    lastPublishedValueRef.current = markdown;
    onChange?.(markdown);
  }, [editor, onChange]);

  const handleCompositionStart = useCallback((event) => {
    event.currentTarget.classList.add('is-composing');
    if (compositionTimerRef.current) window.clearTimeout(compositionTimerRef.current);
    compositionTimerRef.current = 0;
    compositionRef.current.active = true;
    compositionRef.current.settling = false;
  }, []);

  const handleCompositionEnd = useCallback((event) => {
    event.currentTarget.classList.remove('is-composing');
    compositionRef.current.active = false;
    compositionRef.current.settling = true;
    if (compositionTimerRef.current) window.clearTimeout(compositionTimerRef.current);
    compositionTimerRef.current = window.setTimeout(() => {
      compositionRef.current.settling = false;
      compositionTimerRef.current = 0;
      setSyncRevision((revision) => revision + 1);
    }, 0);
  }, []);

  const handleKeyDown = useCallback((event) => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
      event.preventDefault();
      if (
        !disabled
        && !event.isComposing
        && !event.nativeEvent?.isComposing
        && event.keyCode !== 229
        && !ReactEditor.isComposing(editor)
      ) {
        onSave?.();
      }
    }
  }, [disabled, editor, onSave]);

  const decorationModel = useMemo(
    () => slateSelectionDecorationModel(
      Array.isArray(editor.children) && editor.children.length > 0
        ? editor.children
        : initialValueRef.current,
      {
        contexts: selectionContexts,
        sourcePath,
        contentRevision,
        inactiveSelection,
      },
    ),
    [contentRevision, editor, inactiveSelection, normalizedValue, selectionContexts, sourcePath],
  );

  const decorate = useCallback(([, path]) => (
    decorationModel.rangesByPath.get(path.join('.')) || []
  ), [decorationModel]);

  const setEditableRef = useCallback((node) => {
    if (hostRef && typeof hostRef === 'object') hostRef.current = node;
    else if (typeof hostRef === 'function') hostRef(node);
  }, [hostRef]);

  const handleBlur = useCallback(() => {
    if (editor.selection && !SlateRange.isCollapsed(editor.selection)) {
      setInactiveSelection({
        anchor: { ...editor.selection.anchor, path: [...editor.selection.anchor.path] },
        focus: { ...editor.selection.focus, path: [...editor.selection.focus.path] },
      });
    }
  }, [editor]);

  const renderElement = useCallback(
    (props) => <MarkdownElement {...props} editor={editor} />,
    [editor],
  );
  const renderLeaf = useCallback((props) => <MarkdownLeaf {...props} />, []);

  return (
    <Slate
      editor={editor}
      initialValue={initialValueRef.current}
      onValueChange={handleValueChange}
    >
      <Editable
        ref={setEditableRef}
        className="h-full overflow-auto ace-md ace-side-markdown-preview"
        data-ace-managed-inactive-selection="true"
        aria-label="Markdown 所见即所得编辑器"
        aria-busy={disabled ? 'true' : undefined}
        readOnly={disabled}
        decorate={decorate}
        renderElement={renderElement}
        renderLeaf={renderLeaf}
        onFocus={() => setInactiveSelection(null)}
        onBlur={handleBlur}
        onKeyDown={handleKeyDown}
        onCompositionStart={handleCompositionStart}
        onCompositionEnd={handleCompositionEnd}
        spellCheck={false}
      />
    </Slate>
  );
}

export { EDITABLE_BLOCK_TYPES };
