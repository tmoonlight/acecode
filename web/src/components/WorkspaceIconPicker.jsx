import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  WORKSPACE_ICON_COLORS,
  filterWorkspaceIcons,
} from '../lib/workspaceIcons.js';
import { WorkspaceIcon } from './WorkspaceIcon.jsx';

const PICKER_WIDTH = 292;
const VIEWPORT_MARGIN = 8;

// 「编辑项目」左上角图标按钮弹出的选择层:搜索框 + 色板 + 图标网格。
// 放在对话框 DOM 内、用 fixed 定位:对话框本身 overflow:auto,绝对定位会被裁掉;
// 留在对话框里则 Tab 循环与 Enter 约定(lib/dialogKeyboard.js)照常生效。
// 点击选择层外部只收起选择层,保留编辑项目表单;触发按钮和 Esc 也可收起。
// 网格里选中的那一格显示展开态,与侧栏项目展开时一致。
export function WorkspaceIconPicker({ anchorRef, value, onChange, onClose }) {
  const pickerRef = useRef(null);
  const searchRef = useRef(null);
  const [query, setQuery] = useState('');
  const [position, setPosition] = useState(null);
  const icons = useMemo(() => filterWorkspaceIcons(query), [query]);

  useLayoutEffect(() => {
    const place = () => {
      const anchor = anchorRef?.current;
      if (!anchor) return;
      const rect = anchor.getBoundingClientRect();
      const maxLeft = window.innerWidth - PICKER_WIDTH - VIEWPORT_MARGIN;
      setPosition({
        left: Math.max(VIEWPORT_MARGIN, Math.min(rect.left, maxLeft)),
        top: rect.bottom + 6,
      });
    };
    place();
    window.addEventListener('resize', place);
    return () => window.removeEventListener('resize', place);
  }, [anchorRef]);

  useEffect(() => {
    searchRef.current?.focus();
  }, []);

  useEffect(() => {
    const onPointerDown = (event) => {
      if (pickerRef.current?.contains(event.target)
        || anchorRef?.current?.contains(event.target)) return;
      onClose?.();
    };
    document.addEventListener('pointerdown', onPointerDown, true);
    return () => document.removeEventListener('pointerdown', onPointerDown, true);
  }, [anchorRef, onClose]);

  useEffect(() => {
    // Esc 只收起选择层:捕获阶段先拦下,对话框(document 冒泡阶段监听)就不会一起关掉。
    const onKey = (event) => {
      if (event.key !== 'Escape' || event.isComposing) return;
      event.preventDefault();
      event.stopPropagation();
      onClose?.();
      anchorRef?.current?.focus?.();
    };
    document.addEventListener('keydown', onKey, true);
    return () => document.removeEventListener('keydown', onKey, true);
  }, [anchorRef, onClose]);

  return (
    <div
      ref={pickerRef}
      className="ace-workspace-icon-picker fixed z-[210] rounded-xl border border-border bg-surface ace-shadow-lg p-2.5"
      style={{
        width: PICKER_WIDTH,
        left: position?.left ?? 0,
        top: position?.top ?? 0,
        visibility: position ? 'visible' : 'hidden',
      }}
    >
      <input
        ref={searchRef}
        value={query}
        onChange={(event) => setQuery(event.target.value)}
        onKeyDown={(event) => {
          // 选择层在「编辑项目」的 <form> 里:Enter 不能触发表单提交(= 保存)。
          if (event.key === 'Enter') event.preventDefault();
        }}
        placeholder="搜索图标"
        aria-label="搜索图标"
        autoComplete="off"
        spellCheck={false}
        className="w-full h-8 px-2.5 rounded-md border border-border bg-surface text-[12px] text-fg outline-none focus:border-fg-mute placeholder:text-fg-mute/70"
      />
      <div className="mt-2.5 grid grid-cols-8 gap-1 justify-items-center">
        {WORKSPACE_ICON_COLORS.map((color) => {
          const selected = value?.color === color.id;
          return (
            <button
              key={color.id}
              type="button"
              aria-label={color.label}
              aria-pressed={selected}
              onClick={() => onChange?.({ ...value, color: color.id })}
              className={clsx(
                'w-[30px] h-[30px] rounded-full flex items-center justify-center hover:bg-surface-hi',
                selected && 'bg-surface-hi',
              )}
            >
              <span
                className="block w-[18px] h-[18px] rounded-full"
                style={{ backgroundColor: color.value || 'var(--ace-fg)' }}
              />
            </button>
          );
        })}
      </div>
      <div className="mt-1.5 grid grid-cols-8 gap-1 justify-items-center">
        {icons.map((icon) => {
          const selected = value?.id === icon.id;
          return (
            <button
              key={icon.id}
              type="button"
              aria-label={icon.label}
              aria-pressed={selected}
              onClick={() => onChange?.({ ...value, id: icon.id })}
              className={clsx(
                'w-[30px] h-[30px] rounded-md flex items-center justify-center text-fg hover:bg-surface-hi',
                selected && 'bg-surface-hi',
              )}
            >
              <WorkspaceIcon id={icon.id} color={value?.color} open={selected} size={16} />
            </button>
          );
        })}
      </div>
    </div>
  );
}
