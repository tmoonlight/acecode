import { createElement } from 'react';
import { toolIconName } from '../lib/toolIcons.js';
import { fileTypeIconForPath } from '../lib/fileTypeIcons.js';
import { ICON_VIEW_BOX, INTERFACE_ICONS, iconStrokeWidth } from '../lib/interfaceIcons.js';

const ICONS = {
  add: 'Add',
  alarm: 'Alarm',
  archive: 'Archive',
  arrowLeft: 'ArrowLeft',
  arrowRight: 'ArrowRight',
  back: 'Backwards',
  brain: 'Brain',
  sparkle: 'Sparkle',
  brightness: 'Brightness',
  bug: 'Bug',
  chat: 'ChatBubble',
  check: 'Check',
  sliders: 'Sliders',
  clearAll: 'ClearAll',
  close: 'Close',
  code: 'Code',
  command: 'TerminalReadWrite',
  collapseAll: 'CollapseAll',
  columns: 'Columns',
  computer: 'Computer',
  office: 'Office',
  copy: 'Copy',
  darkTheme: 'DarkTheme',
  delete: 'Delete',
  document: 'Document',
  edit: 'Edit',
  editWindow: 'EditWindow',
  ellipsis: 'Ellipsis',
  ellipsisVertical: 'EllipsisVertical',
  embedding: 'Embedding',
  expandDown: 'ExpandDown',
  expandRight: 'ExpandRight',
  expandUp: 'ExpandUp',
  expert: 'Expert',
  extension: 'Extension',
  eye: 'Eye',
  file: 'Document',
  folder: 'FolderClosed',
  folderAdd: 'AddFolder',
  folderOpen: 'FolderOpened',
  fork: 'Fork',
  glyphDown: 'GlyphDown',
  glyphUp: 'GlyphUp',
  globe: 'BrowserGlobe',
  help: 'StatusHelp',
  hook: 'FishHook',
  info: 'StatusInformation',
  keyboard: 'Keyboard',
  leftBar: 'LeftBar',
  lightbulb: 'IntellisenseLightBulbSparkle',
  list: 'List',
  listPanel: 'ListPanel',
  lock: 'Lock',
  mcp: 'MCP',
  newSession: 'NewSession',
  ok: 'StatusOK',
  openFile: 'OpenFile',
  panelLeft: 'PanelLeft',
  panelRight: 'PanelRight',
  panelLeftFilled: 'PanelLeftFilled',
  panelRightFilled: 'PanelRightFilled',
  panelBottom: 'PanelBottom',
  panelBottomFilled: 'PanelBottomFilled',
  palette: 'Palette',
  pin: 'Pin',
  pinOff: 'PinOff',
  refresh: 'Refresh',
  rightBar: 'RightBar',
  run: 'Run',
  running: 'StatusRunning',
  save: 'Save',
  screenFull: 'ScreenFull',
  screenNormal: 'ScreenNormal',
  search: 'Search',
  searchSparkle: 'SearchSparkle',
  send: 'Send',
  settings: 'Settings',
  stop: 'Stop',
  terminal: 'TerminalReadWrite',
  trajectory: 'Trajectory',
  tool: 'Tool',
  warning: 'StatusWarning',
  wordWrap: 'WordWrap',
  world: 'World',
  workspaceMenu: 'WorkspaceMenu',
  worktree: 'Worktree',
  error: 'StatusError',
};

const TOOL_ICON_MAP = new Map([
  ['*', 'tool'],
  ['$', 'terminal'],
  ['!', 'warning'],
  ['R', 'openFile'],
  ['W', 'save'],
  ['E', 'edit'],
  ['D', 'ok'],
  ['S', 'search'],
  ['\u2192', 'openFile'],
  ['\u270D', 'save'],
  ['\u270E', 'edit'],
  ['\u2713', 'ok'],
  ['\u2717', 'error'],
  ['\u{1F50D}', 'search'],
  ['\u26A0', 'warning'],
  ['\u26A0\uFE0F', 'warning'],
]);

// Keep the legacy mono argument for callers; functional artwork always inherits color.
export function VsIcon({
  name,
  size = 16,
  mono = true,
  strong = false,
  className = '',
  alt = '',
  style,
  ...props
}) {
  const file = ICONS[name] || name;
  const definition = Object.prototype.hasOwnProperty.call(INTERFACE_ICONS, file)
    ? INTERFACE_ICONS[file]
    : null;
  const accessibilityProps = alt
    ? { role: 'img', 'aria-label': alt }
    : { 'aria-hidden': 'true' };
  return (
    <span
      className={['ace-icon', definition && 'ace-icon-inline', className].filter(Boolean).join(' ')}
      data-icon-name={file}
      data-monochrome={mono ? 'true' : 'false'}
      style={{
        width: size,
        height: size,
        ...(!definition && { '--ace-icon-url': `url("/vs-icons/${file}.svg")` }),
        ...style,
      }}
      {...accessibilityProps}
      {...props}
    >
      {definition && (
        <svg
          className="ace-icon-svg"
          viewBox={`0 0 ${ICON_VIEW_BOX} ${ICON_VIEW_BOX}`}
          fill="none"
          stroke="currentColor"
          strokeLinecap="round"
          strokeLinejoin="round"
          aria-hidden="true"
          focusable="false"
          style={{ width: '100%', height: '100%', strokeWidth: iconStrokeWidth(size, strong) }}
        >
          {definition.map(([tag, attributes], index) => createElement(tag, { ...attributes, key: index }))}
        </svg>
      )}
    </span>
  );
}

export function FileTypeIcon({
  path,
  size = 20,
  className = '',
  glyphClassName = '',
  fallback = 'file',
  style,
  ...props
}) {
  const icon = fileTypeIconForPath(path);
  if (!icon) {
    return <VsIcon name={fallback} size={size} mono={false} className={className} {...props} />;
  }
  if (icon.id === '_pptx') {
    return (
      <span
        className={['ace-file-type-icon', className].filter(Boolean).join(' ')}
        aria-hidden="true"
        data-file-type-icon={icon.id}
        style={{
          width: size,
          height: size,
          color: icon.color,
          '--ace-file-type-color': icon.color,
          ...style,
        }}
        {...props}
      >
        <svg className="ace-pptx-file-icon" viewBox="0 0 20 20" focusable="false">
          <path fill="currentColor" d="M6 2.4h9.2c1 0 1.8.8 1.8 1.8v11.6c0 1-.8 1.8-1.8 1.8H6z" />
          <rect x="8.1" y="5" width="6.7" height="7.1" rx=".7" fill="rgba(255,255,255,.9)" />
          <path fill="rgba(227,121,51,.82)" d="M9.3 10.8V9.5h1.5V6.4h1.3v3.1h1.5v1.3z" />
          <path fill="#c84c2f" d="M2.2 5.1 10 3.8v12.4l-7.8-1.3z" />
          <path fill="#fff" d="M4.2 7h2.1c1.6 0 2.6.8 2.6 2.1 0 1.4-1 2.2-2.7 2.2h-.6v2H4.2zm1.4 1.2v1.9h.6c.8 0 1.2-.3 1.2-1s-.4-.9-1.2-.9z" />
        </svg>
      </span>
    );
  }
  return (
    <span
      className={['ace-file-type-icon', className].filter(Boolean).join(' ')}
      aria-hidden="true"
      data-file-type-icon={icon.id}
      style={{
        width: size,
        height: size,
        fontSize: size,
        color: icon.color,
        '--ace-file-type-color': icon.color,
        ...style,
      }}
      {...props}
    >
      {glyphClassName ? <span className={glyphClassName}>{icon.glyph}</span> : icon.glyph}
    </span>
  );
}

export function ToolSummaryIcon({ icon, tool = '', ok, className = '' }) {
  if (tool) return <VsIcon name={toolIconName(tool, icon)} size={14} className={className} />;
  if (!ok) return <VsIcon name="error" size={14} mono={false} className={className} />;
  const mapped = TOOL_ICON_MAP.get(icon) || (ICONS[icon] ? icon : 'ok');
  const statusIcon = mapped === 'ok' || mapped === 'warning' || mapped === 'error';
  return <VsIcon name={mapped} size={14} mono={!statusIcon} className={className} />;
}

export function PanelToggleIcon({ side = 'left', expanded = false, size = 16, className = '', ...props }) {
  const name = side === 'bottom' ? 'panelBottom' : side === 'right' ? 'panelRight' : 'panelLeft';
  return (
    <VsIcon
      name={expanded ? `${name}Filled` : name}
      size={size}
      className={className}
      {...props}
    />
  );
}

export function RefreshIcon({ size = 16, className = '', ...props }) {
  return <VsIcon name="refresh" size={size} className={className} {...props} />;
}

// Slash-command badge icon. It inherits currentColor inside the accent badge.
export function CommandGlyph({ kind = 'skill', command = '', size = 12, className = '', ...props }) {
  const name = kind === 'builtin'
    ? (String(command).toLowerCase() === 'goal' ? 'Goal' : 'tool')
    : kind === 'command' ? 'command' : 'lightbulb';
  return (
    <VsIcon
      name={name}
      size={size}
      className={className}
      {...props}
    />
  );
}

export function NavigationArrowIcon({ direction = 'back', size = 16, className = '', ...props }) {
  return (
    <VsIcon
      name={direction === 'forward' ? 'arrowRight' : 'arrowLeft'}
      size={size}
      className={className}
      {...props}
    />
  );
}
