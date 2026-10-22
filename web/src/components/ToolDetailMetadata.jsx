import '../styles/tool-details.css';

export function ToolDetailMetadata({ fields = [] }) {
  if (!fields.length) return null;
  return (
    <div className="flex min-w-0 flex-wrap gap-x-3 gap-y-1 px-4 py-2 pr-12 text-fg-mute" data-tool-detail-metadata="true"
      style={{ fontSize: 'var(--ace-font-size-code-compact)', overflowWrap: 'anywhere' }}>
      {fields.map(({ key, label, value, standalone }) => (
        <span key={key} className={key === 'path' ? 'w-full min-w-0' : 'min-w-0'}>
          {key === '+' || key === '-' ? (
            <span className={key === '+' ? 'ace-change-add' : 'ace-change-del'}>{key}{value}</span>
          ) : standalone ? value : <>{label}: {value}</>}
        </span>
      ))}
    </div>
  );
}
