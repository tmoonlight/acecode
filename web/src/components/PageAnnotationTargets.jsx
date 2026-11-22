export function PageAnnotationTargets({ snapshot, command, working }) {
  return (
    <div className="ace-annotation-chips" aria-label="批注目标">
      {(snapshot.targets || []).map(target => (
        <button
          key={target.id}
          type="button"
          disabled={working}
          aria-pressed={snapshot.target?.id === target.id}
        className="ace-annotation-chip"
        onClick={() => void command('select_target', { target_id: target.id })}
      >
        <span className="ace-annotation-chip-text">{target.text || target.tag}</span>
      </button>
      ))}
      {snapshot.region && (
        <button type="button" className="ace-annotation-chip" disabled={working} aria-pressed={!snapshot.target} onClick={() => void command('select_region')}>
          <span className="ace-annotation-chip-region">区域 1</span>
        </button>
      )}
    </div>
  );
}
