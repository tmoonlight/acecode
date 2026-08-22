import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import {
  createEditor,
  Editor,
  Element as SlateElement,
  Transforms,
} from 'slate';
import {
  Editable,
  ReactEditor,
  Slate,
  useSlate,
  withReact,
} from 'slate-react';
import {
  HistoryEditor,
  withHistory,
} from 'slate-history';
import { markdownToSlate, slateToMarkdown } from '../lib/markdownWysiwyg.js';

const EDITABLE_BLOCK_TYPES = new Set([
  'paragraph',
  'heading',
  'blockquote',
  'code-block',
]);

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

function isBlockActive(editor, type, attributes = {}) {
  if (!editor.selection) return false;
  const [match] = Editor.nodes(editor, {
    at: editor.selection,
    match: (node) => (
      !Editor.isEditor(node)
      && SlateElement.isElement(node)
      && node.type === type
      && Object.entries(attributes).every(([key, value]) => node[key] === value)
    ),
  });
  return !!match;
}

function isMarkActive(editor, mark) {
  return Editor.marks(editor)?.[mark] === true;
}

function toggleMark(editor, mark) {
  if (isMarkActive(editor, mark)) Editor.removeMark(editor, mark);
  else Editor.addMark(editor, mark, true);
}

function setTextBlock(editor, type, attributes = {}) {
  if (!editor.selection) return;
  Transforms.setNodes(editor, { type, ...attributes }, {
    match: (node) => (
      !Editor.isEditor(node)
      && SlateElement.isElement(node)
      && ['paragraph', 'heading', 'code-block'].includes(node.type)
    ),
  });
}

function toggleQuote(editor) {
  if (!editor.selection) return;
  if (isBlockActive(editor, 'blockquote')) {
    Transforms.unwrapNodes(editor, {
      match: (node) => SlateElement.isElement(node) && node.type === 'blockquote',
      split: true,
    });
    return;
  }
  Transforms.wrapNodes(editor, { type: 'blockquote', children: [] }, {
    match: (node) => (
      SlateElement.isElement(node)
      && ['paragraph', 'heading', 'code-block'].includes(node.type)
    ),
    split: true,
  });
}

function toggleList(editor, { ordered = false, task = false } = {}) {
  if (!editor.selection) return;
  const active = isBlockActive(editor, 'list', { ordered });
  if (active) {
    Transforms.unwrapNodes(editor, {
      match: (node) => SlateElement.isElement(node) && node.type === 'list',
      split: true,
    });
    Transforms.unwrapNodes(editor, {
      match: (node) => SlateElement.isElement(node) && node.type === 'list-item',
      split: true,
    });
    return;
  }

  Transforms.unwrapNodes(editor, {
    match: (node) => SlateElement.isElement(node) && node.type === 'list',
    split: true,
  });
  Transforms.wrapNodes(editor, {
    type: 'list-item',
    checked: task ? false : null,
    children: [],
  }, {
    match: (node) => (
      SlateElement.isElement(node)
      && ['paragraph', 'heading', 'code-block'].includes(node.type)
    ),
    split: true,
  });
  Transforms.wrapNodes(editor, {
    type: 'list',
    ordered,
    start: null,
    spread: false,
    children: [],
  }, {
    match: (node) => SlateElement.isElement(node) && node.type === 'list-item',
    split: true,
  });
}

function ToolbarButton({ active = false, disabled = false, label, title, onPress }) {
  return (
    <button
      type="button"
      className={`ace-markdown-toolbar-button${active ? ' is-active' : ''}`}
      aria-label={title || label}
      aria-pressed={active ? 'true' : undefined}
      disabled={disabled}
      title={title || label}
      onMouseDown={(event) => {
        event.preventDefault();
        if (!disabled) onPress?.();
      }}
    >
      {label}
    </button>
  );
}

function MarkdownToolbar({ disabled }) {
  const editor = useSlate();
  const heading = [1, 2, 3].find((level) => isBlockActive(editor, 'heading', { level }));
  const blockValue = heading ? `h${heading}` : (
    isBlockActive(editor, 'code-block') ? 'code' : 'paragraph'
  );

  return (
    <div className="ace-markdown-toolbar" role="toolbar" aria-label="Markdown 格式">
      <select
        className="ace-markdown-toolbar-select"
        aria-label="段落样式"
        value={blockValue}
        disabled={disabled}
        onChange={(event) => {
          const next = event.target.value;
          if (next === 'paragraph') setTextBlock(editor, 'paragraph');
          else if (next === 'code') setTextBlock(editor, 'code-block', { lang: '', meta: '' });
          else setTextBlock(editor, 'heading', { level: Number(next.slice(1)) || 1 });
          try { ReactEditor.focus(editor); } catch { /* Selection remains usable without focus. */ }
        }}
      >
        <option value="paragraph">正文</option>
        <option value="h1">一级标题</option>
        <option value="h2">二级标题</option>
        <option value="h3">三级标题</option>
        <option value="code">代码块</option>
      </select>
      <span className="ace-markdown-toolbar-separator" aria-hidden="true" />
      <ToolbarButton
        label="B"
        title="粗体"
        active={isMarkActive(editor, 'bold')}
        disabled={disabled}
        onPress={() => toggleMark(editor, 'bold')}
      />
      <ToolbarButton
        label="I"
        title="斜体"
        active={isMarkActive(editor, 'italic')}
        disabled={disabled}
        onPress={() => toggleMark(editor, 'italic')}
      />
      <ToolbarButton
        label="S"
        title="删除线"
        active={isMarkActive(editor, 'strike')}
        disabled={disabled}
        onPress={() => toggleMark(editor, 'strike')}
      />
      <ToolbarButton
        label="&lt;/&gt;"
        title="行内代码"
        active={isMarkActive(editor, 'code')}
        disabled={disabled}
        onPress={() => toggleMark(editor, 'code')}
      />
      <span className="ace-markdown-toolbar-separator" aria-hidden="true" />
      <ToolbarButton
        label="引用"
        active={isBlockActive(editor, 'blockquote')}
        disabled={disabled}
        onPress={() => toggleQuote(editor)}
      />
      <ToolbarButton
        label="• 列表"
        active={isBlockActive(editor, 'list', { ordered: false })}
        disabled={disabled}
        onPress={() => toggleList(editor)}
      />
      <ToolbarButton
        label="1. 列表"
        active={isBlockActive(editor, 'list', { ordered: true })}
        disabled={disabled}
        onPress={() => toggleList(editor, { ordered: true })}
      />
      <ToolbarButton
        label="任务"
        title="任务列表"
        active={isBlockActive(editor, 'list-item', { checked: true })}
        disabled={disabled}
        onPress={() => toggleList(editor, { task: true })}
      />
      <span className="ace-markdown-toolbar-spacer" />
      <ToolbarButton
        label="撤销"
        disabled={disabled || editor.history.undos.length === 0}
        onPress={() => HistoryEditor.undo(editor)}
      />
      <ToolbarButton
        label="重做"
        disabled={disabled || editor.history.redos.length === 0}
        onPress={() => HistoryEditor.redo(editor)}
      />
    </div>
  );
}

function OpaqueNode({ attributes, children, element, inline = false }) {
  const Tag = inline ? 'span' : 'div';
  return (
    <Tag
      {...attributes}
      contentEditable={false}
      className={`ace-markdown-opaque${inline ? ' is-inline' : ' is-block'}`}
      title="此 Markdown 结构按原文保留"
    >
      <span className="ace-markdown-opaque-children">{children}</span>
      <code>{element.raw || ''}</code>
    </Tag>
  );
}

function MarkdownElement({ attributes, children, element }) {
  const editor = useSlate();
  switch (element.type) {
    case 'heading': {
      const Tag = `h${Math.min(6, Math.max(1, Number(element.level) || 1))}`;
      return <Tag {...attributes}>{children}</Tag>;
    }
    case 'blockquote':
      return <blockquote {...attributes}>{children}</blockquote>;
    case 'code-block':
      return <pre {...attributes}><code>{children}</code></pre>;
    case 'list': {
      const Tag = element.ordered ? 'ol' : 'ul';
      return <Tag {...attributes} start={element.ordered ? element.start || undefined : undefined}>{children}</Tag>;
    }
    case 'list-item':
      return (
        <li {...attributes} className={element.checked == null ? undefined : 'is-task'}>
          {element.checked == null ? null : (
            <input
              type="checkbox"
              contentEditable={false}
              checked={element.checked === true}
              aria-label="任务完成状态"
              onChange={() => {
                const path = ReactEditor.findPath(editor, element);
                Transforms.setNodes(editor, { checked: element.checked !== true }, { at: path });
              }}
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
      return <td {...attributes}>{children}</td>;
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
  return <span {...attributes}>{content}</span>;
}

export default function MarkdownWysiwygEditor({
  value,
  onChange,
  onSave,
  disabled = false,
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

  useEffect(() => () => {
    if (compositionTimerRef.current) window.clearTimeout(compositionTimerRef.current);
  }, []);

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

  const handleCompositionStart = useCallback(() => {
    if (compositionTimerRef.current) window.clearTimeout(compositionTimerRef.current);
    compositionTimerRef.current = 0;
    compositionRef.current.active = true;
    compositionRef.current.settling = false;
  }, []);

  const handleCompositionEnd = useCallback(() => {
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

  const renderElement = useCallback((props) => <MarkdownElement {...props} />, []);
  const renderLeaf = useCallback((props) => <MarkdownLeaf {...props} />, []);

  return (
    <div className="ace-markdown-editor">
      <Slate
        editor={editor}
        initialValue={initialValueRef.current}
        onValueChange={handleValueChange}
      >
        <MarkdownToolbar disabled={disabled} />
        <Editable
          className="ace-markdown-editable"
          aria-label="Markdown 所见即所得编辑器"
          readOnly={disabled}
          renderElement={renderElement}
          renderLeaf={renderLeaf}
          onKeyDown={handleKeyDown}
          onCompositionStart={handleCompositionStart}
          onCompositionEnd={handleCompositionEnd}
          spellCheck
        />
      </Slate>
    </div>
  );
}

export { EDITABLE_BLOCK_TYPES };
