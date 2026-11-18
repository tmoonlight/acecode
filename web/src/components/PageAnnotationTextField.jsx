import { useEffect, useState } from 'react';

export function PageAnnotationTextField({ target, command, working }) {
  const preview = target?.text_preview;
  const [value, setValue] = useState(preview?.value ?? '');
  useEffect(() => { setValue(preview?.value ?? ''); }, [target?.id]);
  if (!preview) return null;
  return (
    <label className="flex flex-col gap-1 text-[12px] text-fg">
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
        className="resize-y rounded-md border border-border bg-surface-alt px-2 py-1.5 text-fg outline-none focus:border-accent disabled:opacity-50"
      />
      {preview.error && <span role="alert" className="text-danger">{preview.error}</span>}
    </label>
  );
}
