import { isDismissShortcut } from '../lib/shortcutBindings.js';
import { useTranslation } from 'react-i18next';
import { VsIcon } from './Icon.jsx';

// 设置导航顶部的搜索框。它不在导航的滚动区里:滚动时搜索框固定在顶部,
// 只有下面的分组导航 / 搜索结果(SettingsSearchResults)滚动。
export function SettingsSearch({ query, onQuery, results, selected, onSelect, onComposing }) {
  useTranslation();
  return (
    <div className="ace-settings-search">
      <div className="ace-settings-search-input">
        <VsIcon name="search" size={18} className="text-fg-mute shrink-0" />
        <input type="search" value={query} placeholder="搜索设置" aria-label="搜索设置"
          aria-controls={query.trim() ? 'settings-search-results' : undefined}
          onChange={(event) => onQuery(event.target.value)}
          onCompositionStart={() => onComposing(true)} onCompositionEnd={() => onComposing(false)}
          onKeyDown={(event) => {
            if (event.nativeEvent.isComposing) return;
            // 有内容时 Esc 只清空搜索;已空时放行,让它冒泡去关闭设置窗口。
            if (isDismissShortcut(event) && query) { event.stopPropagation(); onQuery(''); }
            if (results.length && ['ArrowDown', 'ArrowUp', 'Enter'].includes(event.key)) {
              event.preventDefault();
              onSelect(event.key === 'Enter' ? selected : (selected + (event.key === 'ArrowDown' ? 1 : -1) + results.length) % results.length);
            }
          }} />
      </div>
    </div>
  );
}

// 搜索结果列表,放在导航的滚动区里(与分组导航互斥显示)。
export function SettingsSearchResults({ results, selected, onSelect }) {
  useTranslation();
  return (
    <div className="ace-settings-search-results" id="settings-search-results">
      <div className="text-[11px] text-fg-mute px-2 py-2" role="status" aria-live="polite">
        {results.length ? `找到 ${results.length} 项设置` : '未找到相关设置'}
      </div>
      {results.map((result, index) => <button key={result.id} type="button"
        aria-current={index === selected ? 'true' : undefined} className="ace-settings-search-result"
        onClick={() => onSelect(index)}><span>{result.label}</span><small>{result.sectionLabel}</small></button>)}
    </div>
  );
}
