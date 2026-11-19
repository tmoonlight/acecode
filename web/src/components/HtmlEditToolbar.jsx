// HTML 可视化编辑的浮动工具栏(docs/html-preview.md)。画在外层页面,位置跟随隔离框报来的
// 选中元素矩形;按元素种类给不同的控件。所有修改都交给 HtmlPagePreview 写回源码。
// 改字时工具栏照常显示;框里拖选了文字(range)时显示这段文字的当前格式,修改只作用在这段文字上。
// 下拉控件(字号、对齐、颜色、边框、圆角)的按钮都带箭头,弹层贴在按钮正下方,放不下时
// 改为右对齐或向上展开(htmlPageFrame.js::toolbarMenuPlacement)。
import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  HTML_TOOLBAR_COLORS,
  HTML_TOOLBAR_FONT_SIZES,
  HTML_TOOLBAR_RADII,
  HTML_TOOLBAR_BORDER_WIDTHS,
  cssColorToHex,
  cssPixels,
  isBoldWeight,
  toggledDecoration,
  toolbarMenuPlacement,
} from '../lib/htmlPageFrame.js';
import { VsIcon } from './Icon.jsx';

const GAP = 8;
const EDGE = 6;

const ALIGN_OPTIONS = [
  ['left', 'AlignLeft', '左对齐'],
  ['center', 'AlignCenter', '居中对齐'],
  ['right', 'AlignRight', '右对齐'],
  ['justify', 'AlignJustify', '两端对齐'],
];

function normalizeAlign(value) {
  const text = String(value || '').toLowerCase();
  if (text === 'center' || text === 'right' || text === 'justify') return text;
  if (text === 'end') return 'right';
  return 'left';
}

// 一个下拉:按钮(图标 + 箭头)和贴在它正下方的弹层。打开哪个由工具栏统一管,同时只开一个。
function Dropdown({ id, open, label, onToggle, trigger, pressed = false, wide = false, children }) {
  const wrapRef = useRef(null);
  const menuRef = useRef(null);
  const [placement, setPlacement] = useState(null);

  useLayoutEffect(() => {
    if (!open) {
      setPlacement(null);
      return;
    }
    const wrap = wrapRef.current;
    const menu = menuRef.current;
    const host = wrap?.closest('.ace-html-page');
    if (!wrap || !menu || !host) return;
    // 字号是「输入框 + 箭头」一体的组合框,弹层对齐整个框,而不是只对齐箭头。
    const anchor = wrap.closest('.ace-html-toolbar-combo') || wrap;
    setPlacement(toolbarMenuPlacement({
      trigger: anchor.getBoundingClientRect(),
      menu: { width: menu.offsetWidth, height: menu.scrollHeight },
      bounds: host.getBoundingClientRect(),
    }));
  }, [open]);

  return (
    <span ref={wrapRef} className="ace-html-toolbar-dropdown">
      <button type="button" className={clsx('ace-html-toolbar-btn ace-html-toolbar-trigger', wide && 'is-wide')}
        aria-label={label} aria-haspopup="menu" aria-expanded={open} aria-pressed={pressed || undefined}
        onClick={() => onToggle(open ? '' : id)}>
        {trigger}
        <VsIcon name="ExpandDown" size={12} className="ace-html-toolbar-chevron" />
      </button>
      {open && (
        <div ref={menuRef} role="menu" aria-label={label}
          className={clsx('ace-html-toolbar-menu', placement?.alignRight && 'is-align-right', placement?.above && 'is-above')}
          style={{ visibility: placement ? 'visible' : 'hidden', maxHeight: placement?.maxHeight || undefined }}>
          {children}
        </div>
      )}
    </span>
  );
}

function ColorSwatch({ value }) {
  const hex = cssColorToHex(value);
  return <span className={clsx('ace-html-toolbar-swatch', !hex && 'is-empty')} style={hex ? { background: hex } : undefined} />;
}

// 色板:常用色 + 自定义(系统取色器);allowNone 时第一格是「无」(去掉这个属性)。
function ColorMenu({ label, value, allowNone = false, onPick }) {
  const current = cssColorToHex(value);
  return (
    <div className="ace-html-toolbar-palette">
      {allowNone && (
        <button type="button" role="menuitemradio" aria-checked={!current} className="ace-html-toolbar-chip"
          onClick={() => onPick(null)}>无</button>
      )}
      <div className="ace-html-toolbar-swatches">
        {HTML_TOOLBAR_COLORS.map((color) => (
          <button key={color} type="button" role="menuitemradio" aria-label={color}
            aria-checked={current === color} className="ace-html-toolbar-swatch-btn"
            style={{ background: color }} onClick={() => onPick(color)} />
        ))}
      </div>
      <label className="ace-html-toolbar-chip ace-html-toolbar-custom-color">
        自定义颜色
        <input type="color" aria-label={label} value={current || '#000000'}
          onChange={(event) => onPick(event.target.value)} />
      </label>
    </div>
  );
}

function FontSizeCombo({ value, open, onToggle, onPick }) {
  const [draft, setDraft] = useState(String(value));
  useLayoutEffect(() => { setDraft(String(value)); }, [value]);
  const commit = (raw) => {
    const number = Number(raw);
    if (!Number.isFinite(number) || number < 6 || number > 400) {
      setDraft(String(value));
      return;
    }
    if (number !== value) onPick(number);
  };
  return (
    <span className="ace-html-toolbar-combo">
      <input type="number" aria-label="字号" value={draft} min={6} max={400}
        onChange={(event) => {
          setDraft(event.target.value);
          // 键入时等回车 / 失焦再应用;上下箭头与滚轮(没有 inputType)立即应用。
          if (!event.nativeEvent?.inputType) commit(event.target.value);
        }}
        onBlur={(event) => commit(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault();
            commit(event.currentTarget.value);
          }
        }} />
      <Dropdown id="font-size" open={open} label="字号" onToggle={onToggle} trigger={null}>
        <div className="ace-html-toolbar-menu-heading">字号 (px)</div>
        {HTML_TOOLBAR_FONT_SIZES.map((size) => (
          <button key={size} type="button" role="menuitemradio" aria-checked={size === value}
            className="ace-html-toolbar-menu-item" onClick={() => {
              onToggle('');
              onPick(size);
            }}>{size}</button>
        ))}
      </Dropdown>
    </span>
  );
}

function UrlMenu({ label, initial, removable, onApply, onClose }) {
  const [value, setValue] = useState(initial || '');
  const inputRef = useRef(null);
  useLayoutEffect(() => { inputRef.current?.focus(); inputRef.current?.select(); }, []);
  return (
    <form className="ace-html-toolbar-url" onSubmit={(event) => {
      event.preventDefault();
      onApply(value.trim());
    }}>
      <input ref={inputRef} type="text" aria-label={label} value={value} spellCheck="false"
        onChange={(event) => setValue(event.target.value)}
        onKeyDown={(event) => { if (event.key === 'Escape') { event.preventDefault(); onClose(); } }} />
      <button type="submit" className="ace-html-toolbar-text-btn is-primary">确定</button>
      {removable && (
        <button type="button" className="ace-html-toolbar-text-btn" onClick={() => onApply('')}>移除</button>
      )}
    </form>
  );
}

export function HtmlEditToolbar({
  info,
  caps,
  bounds,
  textEditing = false,
  range = null,
  onAiEdit,
  onStyle,
  onEditText,
  onLink,
  onImage,
  onDelete,
}) {
  const ref = useRef(null);
  const [size, setSize] = useState({ width: 0, height: 36 });
  const [menu, setMenu] = useState('');

  useLayoutEffect(() => {
    const element = ref.current;
    if (!element) return;
    const next = { width: element.offsetWidth, height: element.offsetHeight };
    setSize((previous) => (previous.width === next.width && previous.height === next.height ? previous : next));
  });
  useLayoutEffect(() => { setMenu(''); }, [info?.sid, info?.selector]);

  // 点到工具栏外面、焦点进了隔离框(外层窗口失焦)或按 Esc,收起弹层。
  useEffect(() => {
    if (!menu) return undefined;
    const close = (event) => {
      // 窗口失焦事件的 target 是 window,不是节点。
      const target = event?.target;
      if (typeof Node !== 'undefined' && target instanceof Node && ref.current?.contains(target)) return;
      setMenu('');
    };
    const onKey = (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        setMenu('');
      }
    };
    document.addEventListener('pointerdown', close, true);
    window.addEventListener('blur', close);
    document.addEventListener('keydown', onKey, true);
    return () => {
      document.removeEventListener('pointerdown', close, true);
      window.removeEventListener('blur', close);
      document.removeEventListener('keydown', onKey, true);
    };
  }, [menu]);

  const rect = info?.rect;
  if (!rect || !bounds) return null;
  if (rect.y + rect.h < 0 || rect.y > bounds.height) return null;

  // 优先放在元素上方(可以盖住上方的内容,不挡住下面要接着点的元素);上方连贴边都放不下、
  // 会压住选中元素本身时才放到下方。
  let top = rect.y - size.height - GAP;
  if (top < EDGE) top = rect.y - size.height - 2;
  if (top < 0) top = rect.y + rect.h + GAP;
  if (top + size.height > bounds.height - EDGE) top = Math.max(EDGE, Math.min(rect.y + EDGE, bounds.height - size.height - EDGE));
  const left = Math.max(EDGE, Math.min(rect.x, bounds.width - size.width - EDGE));

  const styles = range ? { ...(info.styles || {}), ...range.styles } : (info.styles || {});
  const href = range ? range.href : info.href;
  const text = info.kind === 'text';
  const block = info.kind === 'block';
  const image = info.kind === 'image';
  const bold = isBoldWeight(styles.fontWeight);
  const italic = String(styles.fontStyle || '') === 'italic';
  const decoration = String(styles.textDecorationLine || '');
  const underline = decoration.includes('underline');
  const strike = decoration.includes('line-through');
  const align = normalizeAlign(styles.textAlign);
  const alignIcon = (ALIGN_OPTIONS.find(([value]) => value === align) || ALIGN_OPTIONS[0])[1];
  const borderWidth = cssPixels(styles.borderWidth);
  const borderVisible = styles.borderStyle && styles.borderStyle !== 'none' && borderWidth > 0;
  const radius = cssPixels(styles.borderRadius);

  const button = (pressed, label, icon, onClick, className) => (
    <button type="button" className={clsx('ace-html-toolbar-btn', className)} aria-label={label}
      aria-pressed={pressed} onClick={onClick}>
      <VsIcon name={icon} size={18} />
    </button>
  );
  const pick = (id) => setMenu(id);

  return (
    <div ref={ref} className="ace-html-toolbar" role="toolbar" aria-label="编辑元素"
      style={{ left, top }} onMouseDown={(event) => {
        if (!(event.target instanceof HTMLInputElement)) event.preventDefault();
      }}>
      <button type="button" className="ace-html-toolbar-btn ace-html-toolbar-ai" onClick={onAiEdit}>
        <VsIcon name="AiEdit" size={18} />
        <span>AI 编辑</span>
      </button>
      {caps.style && text && (
        <>
          <span className="ace-html-toolbar-sep" />
          {(caps.text || textEditing) && button(textEditing, '编辑文字', 'TextEdit', onEditText)}
          <FontSizeCombo value={Math.round(cssPixels(styles.fontSize)) || 16} open={menu === 'font-size'}
            onToggle={pick} onPick={(value) => onStyle({ 'font-size': `${value}px` })} />
          {button(bold, '加粗', 'TextBold', () => onStyle({ 'font-weight': bold ? '400' : '700' }))}
          {button(italic, '斜体', 'TextItalic', () => onStyle({ 'font-style': italic ? 'normal' : 'italic' }))}
          {button(underline, '下划线', 'TextUnderline',
            () => onStyle({ 'text-decoration-line': toggledDecoration(decoration, 'underline') }))}
          {button(strike, '删除线', 'TextStrikethrough',
            () => onStyle({ 'text-decoration-line': toggledDecoration(decoration, 'line-through') }))}
          {caps.link && (
            <Dropdown id="link" open={menu === 'link'} label="链接" onToggle={pick} pressed={!!href}
              trigger={<VsIcon name="Link" size={18} />}>
              <UrlMenu label="链接地址" initial={href} removable={range ? !!href : info.tag === 'a' && !!href}
                onClose={() => setMenu('')}
                onApply={(value) => {
                  setMenu('');
                  onLink(value);
                }} />
            </Dropdown>
          )}
          <Dropdown id="align" open={menu === 'align'} label="对齐" onToggle={pick}
            trigger={<VsIcon name={alignIcon} size={18} />}>
            {ALIGN_OPTIONS.map(([value, icon, label]) => (
              <button key={value} type="button" role="menuitemradio" aria-checked={align === value}
                className="ace-html-toolbar-menu-item" onClick={() => {
                  setMenu('');
                  onStyle({ 'text-align': value });
                }}>
                <VsIcon name={icon} size={18} />
                <span>{label}</span>
              </button>
            ))}
          </Dropdown>
          <span className="ace-html-toolbar-sep" />
          <Dropdown id="color" open={menu === 'color'} label="文字颜色" onToggle={pick}
            trigger={<span className="ace-html-toolbar-colored"><VsIcon name="TextColor" size={18} /><ColorSwatch value={styles.color} /></span>}>
            <ColorMenu label="文字颜色" value={styles.color} onPick={(value) => onStyle({ color: value })} />
          </Dropdown>
        </>
      )}
      {caps.style && block && (
        <>
          <span className="ace-html-toolbar-sep" />
          <Dropdown id="fill" open={menu === 'fill'} label="背景色" onToggle={pick}
            trigger={<span className="ace-html-toolbar-colored"><VsIcon name="FillColor" size={18} /><ColorSwatch value={styles.backgroundColor} /></span>}>
            <ColorMenu label="背景色" value={styles.backgroundColor} allowNone
              onPick={(value) => onStyle({ 'background-color': value })} />
          </Dropdown>
          <Dropdown id="border" open={menu === 'border'} label="边框" onToggle={pick}
            trigger={<span className="ace-html-toolbar-colored"><VsIcon name="BorderColor" size={18} /><ColorSwatch value={borderVisible ? styles.borderColor : ''} /></span>}>
            <ColorMenu label="边框颜色" value={borderVisible ? styles.borderColor : ''}
              onPick={(value) => onStyle(borderVisible
                ? { 'border-color': value }
                : { 'border-color': value, 'border-style': 'solid', 'border-width': '1px' })} />
            <div className="ace-html-toolbar-menu-row" role="group" aria-label="边框粗细">
              <VsIcon name="BorderWidth" size={18} />
              {HTML_TOOLBAR_BORDER_WIDTHS.map((width) => (
                <button key={width} type="button" role="menuitemradio"
                  aria-checked={(borderVisible ? borderWidth : 0) === width} className="ace-html-toolbar-chip"
                  onClick={() => onStyle(width > 0
                    ? { 'border-width': `${width}px`, ...(borderVisible ? {} : { 'border-style': 'solid' }) }
                    : { 'border-width': '0px' })}>
                  {width === 0 ? '无' : width}
                </button>
              ))}
            </div>
          </Dropdown>
          <Dropdown id="radius" open={menu === 'radius'} label="圆角" onToggle={pick}
            trigger={<VsIcon name="CornerRadius" size={18} />}>
            <div className="ace-html-toolbar-menu-heading">圆角 (px)</div>
            {HTML_TOOLBAR_RADII.map((value) => (
              <button key={value} type="button" role="menuitemradio" aria-checked={radius === value}
                className="ace-html-toolbar-menu-item" onClick={() => {
                  setMenu('');
                  onStyle({ 'border-radius': `${value}px` });
                }}>{value}</button>
            ))}
          </Dropdown>
        </>
      )}
      {caps.image && image && (
        <>
          <span className="ace-html-toolbar-sep" />
          <Dropdown id="image" open={menu === 'image'} label="替换图片" onToggle={pick}
            trigger={<VsIcon name="ImageReplace" size={18} />}>
            <UrlMenu label="图片地址" initial={info.src} removable={false}
              onClose={() => setMenu('')}
              onApply={(value) => {
                setMenu('');
                if (value) onImage(value);
              }} />
          </Dropdown>
        </>
      )}
      {caps.remove && (
        <>
          <span className="ace-html-toolbar-sep" />
          {button(false, '删除元素', 'Delete', onDelete, 'is-danger')}
        </>
      )}
    </div>
  );
}
