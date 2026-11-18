export function PageAnnotationTargets({ snapshot, command, working }) {
  return (
    <div className="flex flex-wrap gap-1" aria-label="批注目标">
      {(snapshot.targets || []).map(target => (
        <button
          key={target.id}
          type="button"
          disabled={working}
          aria-pressed={snapshot.target?.id === target.id}
          className="max-w-full truncate rounded border border-border px-2 py-1 aria-pressed:border-accent"
          onClick={() => void command('select_target', { target_id: target.id })}
        >{target.tag}{target.locator !== target.tag ? ` (${target.locator})` : ''}</button>
      ))}
      {snapshot.region && (
        <button type="button" disabled={working} aria-pressed={!snapshot.target}
          className="rounded border border-border px-2 py-1 aria-pressed:border-accent"
          onClick={() => void command('select_region')}>区域</button>
      )}
      <button type="button" disabled={working || snapshot.targets?.length >= 64}
        className="rounded px-2 py-1 hover:bg-surface-hi"
        onClick={() => void command('add_target')}>添加元素</button>
      <button type="button" disabled={working} className="rounded px-2 py-1 hover:bg-surface-hi"
        onClick={() => void command('add_region')}>{snapshot.region ? '替换区域' : '添加区域'}</button>
    </div>
  );
}
