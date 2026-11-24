import { useEffect, useRef, useState } from 'react';
import { Modal } from './Modal.jsx';

// 页面已关闭时的编辑弹窗（PRD §6.5）：只有描述框、删除、取消/保存。
// 没有「调整」——页面没了，没有样式可比对；定位信息只读展示。
export function PageAnnotationEditDialog({ record, onCancel, onSave, onDelete }) {
  const [description, setDescription] = useState(record?.description || '');
  const [editing, setEditing] = useState(false);
  const [saving, setSaving] = useState(false);
  const inputRef = useRef(null);
  useEffect(() => {
    const input = inputRef.current;
    if (!input) return;
    input.focus();
    const end = input.value.length;
    input.setSelectionRange?.(end, end);
  }, []);
  if (!record) return null;
  const locator = Array.isArray(record.targets) && record.targets[0]?.locator
    ? String(record.targets[0].locator)
    : record.element;
  const pageLabel = record.pageTitle || record.pageUrl || '页面';
  const subtitle = record.state === 'orphan'
    ? `${pageLabel} · 页面已关闭，只能修改文字`
    : `${pageLabel} · 目标已失效，只能修改文字`;
  const save = async () => {
    setSaving(true);
    try {
      const saved = await onSave?.(record, description.trim());
      if (saved !== false) setEditing(false);
    } catch {
      setEditing(true);
    } finally {
      setSaving(false);
    }
  };
  return (
    <Modal width={420} onClose={onCancel}>
      <div className="p-5">
        <h2 className="text-[15px] font-semibold text-fg">
          {`编辑第 ${record.number} 条批注`}
        </h2>
        <p className="mt-1 text-[12px] leading-5 text-fg-2">
          {subtitle}
        </p>
        <textarea
          ref={inputRef}
          value={description}
          rows={4}
          maxLength={4096}
          aria-label="批注描述"
          className="mt-3 w-full resize-y rounded-lg border border-border bg-surface px-3 py-2 text-[13px] leading-6 text-fg outline-none focus:border-accent"
          onChange={(event) => setDescription(event.target.value)}
        />
        <p className="mt-2 truncate text-[11.5px] text-fg-mute" title={locator}>
          {`目标：${locator}`}
        </p>
        <div className="mt-5 flex items-center justify-between gap-2">
          <button type="button"
            className="h-8 rounded-lg px-2 text-[13px] text-err hover:bg-surface-hi"
            onClick={() => { onDelete?.(record); }}>
            删除这条
          </button>
          <div className="flex gap-2">
            <button type="button"
              className="h-8 rounded-lg border border-border px-3 text-[13px] text-fg-2 hover:bg-surface-hi"
              onClick={onCancel}>
              取消
            </button>
            {editing ? (
              <button type="button"
                className="h-8 rounded-lg bg-accent px-3 text-[13px] font-medium text-white disabled:opacity-50"
                disabled={saving || !description.trim()}
                onClick={save}>
                保存
              </button>
            ) : (
              <button type="button"
                className="h-8 rounded-lg bg-accent px-3 text-[13px] font-medium text-white"
                onClick={() => {
                  setEditing(true);
                  inputRef.current?.focus?.();
                }}>
                编辑
              </button>
            )}
          </div>
        </div>
      </div>
    </Modal>
  );
}
