import { useEffect, useRef, useState } from 'react';
import { filterShortcuts, shortcutCatalog, shortcutLabel } from '../lib/appShortcuts.js';
import { bindingLabel, captureShortcut, DEFAULT_SHORTCUTS, effectiveShortcut, getShortcutOverrides, shortcutStore, validateShortcut } from '../lib/shortcutBindings.js';
import { useShortcutPreferences } from '../lib/useShortcutPreferences.js';
import { VsIcon } from './Icon.jsx';
import { Modal } from './Modal.jsx';

function ShortcutKeys({ label }) {
  return label.split('+').map((key, index) => (
    <kbd key={`${index}-${key}`} className="inline-flex h-7 min-w-7 items-center justify-center rounded border border-border bg-surface-alt px-1.5 font-sans text-[12px] text-fg-2">{key}</kbd>
  ));
}

function shortcutError(issue) {
  if (!issue) return '';
  if (issue.reason === 'conflict') {
    const action = shortcutCatalog().find((row) => row.bindings.includes(issue.conflict));
    return `此快捷键已用于“${action?.title || issue.conflict}”，请换一组按键。`;
  }
  if (issue.reason === 'storage') return '无法保存快捷键，请检查本地存储后重试。';
  if (issue.reason === 'reserved') return '此组合键由系统、浏览器或文本编辑保留，请换一组按键。';
  if (issue.reason === 'modifier') return '请搭配 Ctrl、Cmd 或 Alt，或使用功能键，避免影响正常输入。';
  return '请按下完整的快捷键组合。';
}

function ShortcutEditor({ editing, onClose }) {
  const [candidate, setCandidate] = useState(() => effectiveShortcut(editing.id));
  const [error, setError] = useState('');
  const recordRef = useRef(null);
  const issue = validateShortcut(editing.id, candidate, getShortcutOverrides());
  const save = (reset = false) => {
    const result = shortcutStore().save(editing.id, reset ? null : candidate);
    if (!result.ok) { setError(shortcutError(result)); return; }
    onClose();
  };
  return (
    <Modal onClose={onClose} width={440} layerClassName="z-[400]" labelledBy="shortcut-editor-title" dismissOnBackdrop={false}>
      <div className="p-5">
        <div className="mb-4 flex items-center justify-between gap-3">
          <h2 id="shortcut-editor-title" className="text-[15px] font-semibold">修改快捷键</h2>
          <button type="button" aria-label="关闭" onClick={onClose}
            className="inline-flex h-7 w-7 items-center justify-center rounded text-fg-mute hover:bg-surface-hi hover:text-fg focus-visible:outline focus-visible:outline-2 focus-visible:outline-accent">
            <VsIcon name="close" size={14} />
          </button>
        </div>
        <p className="mb-3 text-[13px] text-fg">{editing.title}</p>
        <label className="block text-[12px] text-fg-mute">
          <span>点击下方输入框，然后按下新的组合键</span>
          <input ref={recordRef} data-shortcut-recorder autoFocus readOnly value={bindingLabel(candidate)}
            aria-label="录入快捷键" aria-describedby="shortcut-record-hint shortcut-record-error" aria-invalid={!!(error || issue)}
            className="mt-2 h-11 w-full rounded-md border border-border bg-surface-alt px-3 text-center text-[14px] text-fg outline-none focus:border-accent"
            onKeyDown={(event) => {
              if (event.key === 'Tab') return;
              event.preventDefault();
              event.stopPropagation();
              const captured = captureShortcut(event);
              if (captured) { setCandidate(captured); setError(''); }
            }} />
        </label>
        <p id="shortcut-record-hint" className="mt-2 text-[11px] text-fg-mute">录入期间不会执行快捷键，点击保存后立即生效。</p>
        <div id="shortcut-record-error" role="status" className="mt-2 min-h-5 text-[12px] text-danger">{error || shortcutError(issue)}</div>
        <div className="mt-4 flex flex-wrap items-center justify-end gap-2">
          <button type="button" onClick={() => save(true)} title={bindingLabel(DEFAULT_SHORTCUTS[editing.id])}
            className="mr-auto rounded px-2 py-1 text-[12px] text-fg-mute hover:bg-surface-hi hover:text-fg">恢复默认</button>
          <button type="button" onClick={onClose} className="rounded border border-border bg-surface px-3 py-1 text-[12px] hover:bg-surface-hi">取消</button>
          <button type="button" disabled={!!issue} onClick={() => save()} data-ace-dialog-primary="true"
            className="rounded bg-accent px-3 py-1 text-[12px] text-white disabled:opacity-50">保存</button>
        </div>
      </div>
    </Modal>
  );
}

export function ShortcutsSettings() {
  useShortcutPreferences();
  const [query, setQuery] = useState('');
  const [editing, setEditing] = useState(null);
  const searchRef = useRef(null);
  const rows = filterShortcuts(shortcutCatalog(), query);
  const groups = [...new Set(rows.map((row) => row.group))];
  useEffect(() => { searchRef.current?.focus({ preventScroll: true }); }, []);
  return (
    <section data-shortcut-settings="true">
      <h2 className="text-xl font-bold mb-5">键盘快捷键</h2>
      <div data-shortcut-filters className="sticky -top-3 z-10 -mx-4 -mt-3 mb-5 bg-surface px-4 py-3 sm:-top-5 sm:-mx-6 sm:px-6">
        <label className="flex min-w-0 items-center gap-2 rounded-md border border-border bg-surface px-3 focus-within:border-accent transition">
          <VsIcon name="search" size={16} className="shrink-0 text-fg-mute" />
          <input ref={searchRef} data-shortcut-search type="search" value={query}
            onChange={(event) => setQuery(event.target.value)} placeholder="搜索快捷键" aria-label="搜索快捷键"
            className="h-9 w-full min-w-0 bg-transparent text-[13px] text-fg placeholder:text-fg-mute outline-none" />
        </label>
      </div>
      {groups.map((group) => (
        <section key={group} className="mb-6" aria-label={group}>
          <h3 className="text-[14px] font-semibold mb-3">{group}</h3>
          <dl className="divide-y divide-border">
            {rows.filter((row) => row.group === group).map((row) => (
              <div key={row.id} data-shortcut-id={row.id} className="flex flex-wrap items-center justify-between gap-x-4 gap-y-2 py-3">
                <dt className="min-w-0 flex-1 basis-48">
                  <div className="text-[13px] text-fg">{row.title}</div>
                  <div className="mt-0.5 text-[11px] text-fg-mute">{row.description}</div>
                </dt>
                <dd className="flex max-w-full flex-wrap items-center justify-end gap-2">
                  {row.bindings.map((id) => (
                    <span key={id} className="inline-flex max-w-full flex-wrap items-center gap-1">
                      <ShortcutKeys label={shortcutLabel(id)} />
                      <button type="button" data-edit-shortcut={id} title="修改快捷键" aria-label={`修改快捷键：${row.title}${id === 'historyPrevious' ? '（上一条）' : id === 'historyNext' ? '（下一条）' : ''}`}
                        onClick={() => setEditing({ id, title: row.title })}
                        className="ml-1 inline-flex h-7 w-7 shrink-0 items-center justify-center rounded text-fg-mute hover:bg-surface-hi hover:text-fg focus-visible:outline focus-visible:outline-2 focus-visible:outline-accent">
                        <VsIcon name="edit" size={14} />
                      </button>
                    </span>
                  ))}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ))}
      {rows.length === 0 && <p role="status" className="py-8 text-center text-[13px] text-fg-mute">没有找到匹配的快捷键</p>}
      {editing && <ShortcutEditor editing={editing} onClose={() => setEditing(null)} />}
    </section>
  );
}
