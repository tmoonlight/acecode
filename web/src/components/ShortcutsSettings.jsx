import { useEffect, useRef, useState } from 'react';
import { filterShortcuts, shortcutCatalog } from '../lib/appShortcuts.js';
import { VsIcon } from './Icon.jsx';

export function ShortcutsSettings() {
  const [query, setQuery] = useState('');
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
                <dd className="flex shrink-0 items-center gap-1">
                  {row.keys.split('+').map((key, index) => (
                    <kbd key={`${index}-${key}`} className="inline-flex h-7 min-w-7 items-center justify-center rounded border border-border bg-surface-alt px-1.5 font-sans text-[12px] text-fg-2">{key}</kbd>
                  ))}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ))}
      {rows.length === 0 && <p role="status" className="py-8 text-center text-[13px] text-fg-mute">没有找到匹配的快捷键</p>}
    </section>
  );
}
