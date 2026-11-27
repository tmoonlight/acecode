import { useEffect, useState } from 'react';

// The native page-annotation runtime reports these two hint strings as prose,
// not as stable error codes. Map them to product copy so they join the i18n
// catalog instead of leaking English into the UI (mirrors editableFileError).
function localizedTextError(error) {
  if (error === 'Text exceeds the editable limit or is not visible content') return '文本超出可编辑上限，或该内容不可见';
  if (error === 'Text nodes changed; cancel and select the element again') return '文本节点已变化，请取消后重新选择元素';
  return error;
}

export function PageAnnotationTextField({ target, command, working }) {
  const preview = target?.text_preview;
  const [value, setValue] = useState(preview?.value ?? '');
  useEffect(() => { setValue(preview?.value ?? ''); }, [target?.id]);
  if (!preview) return null;
  return (
    <label className="ace-annotation-text-field">
      <span>可见文本</span>
      <textarea
        aria-label="可见文本"
        value={value}
        disabled={working || !preview.editable}
        maxLength={4096}
        rows={3}
        onChange={event => {
          const text = event.target.value;
          setValue(text);
          void command('preview_text', { target_id: target.id, text });
        }}
        className="ace-annotation-text-input"
      />
      {preview.error && <span role="alert" className="ace-annotation-text-error">{localizedTextError(preview.error)}</span>}
    </label>
  );
}
