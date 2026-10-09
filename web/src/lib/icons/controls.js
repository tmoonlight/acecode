// ACECode-authored outlines. Every symbol uses the same 20-unit optical grid.
const p = (d, attrs = {}) => ['path', { d, ...attrs }];
const r = (x, y, width, height, rx = 1.6, attrs = {}) => ['rect', { x, y, width, height, rx, ...attrs }];
const c = (cx, cy, radius, attrs = {}) => ['circle', { cx, cy, r: radius, ...attrs }];
const dot = (cx, cy, radius = .8) => c(cx, cy, radius, { fill: 'currentColor', stroke: 'none' });
const left = [p('M16.5 10H3.5M8.2 5.2 3.8 9.6Q3.4 10 3.8 10.4L8.2 14.8')];
const right = [p('M3.5 10H16.5M11.8 5.2 16.2 9.6Q16.6 10 16.2 10.4L11.8 14.8')];
const down = [p('m5.5 7.7 4.1 4.1q.4.4.8 0l4.1-4.1')];
const up = [p('m5.5 12.3 4.1-4.1q.4-.4.8 0l4.1 4.1')];
const panel = r(2.5, 3.5, 15, 13);
const panelLeft = [panel, p('M7.5 3.5v13')];
const panelRight = [panel, p('M12.5 3.5v13')];
const search = [c(8.7, 8.7, 5.9), p('m13 13 4.3 4.3')];
const ellipsis = [dot(4.5, 10), dot(10, 10), dot(15.5, 10)];
const goalReticle = [c(10, 10, 5.8), p('M10 2.5V5.2M10 14.8V17.5M2.5 10H5.2M14.8 10H17.5')];

function softPolygon(points, radius = .3) {
  return points.map(([x, y], index) => {
    const near = ([px, py]) => {
      const length = Math.hypot(px - x, py - y);
      const amount = Math.min(radius, length / 3) / length;
      return [x + (px - x) * amount, y + (py - y) * amount].map((value) => Number(value.toFixed(4)));
    };
    const a = near(points[(index + points.length - 1) % points.length]);
    const b = near(points[(index + 1) % points.length]);
    return `${index ? 'L' : 'M'}${a.join(' ')}Q${Number(x.toFixed(4))} ${Number(y.toFixed(4))} ${b.join(' ')}`;
  }).join('') + 'Z';
}

const swarmCenters = [[10, 10], [10, 5.7], [13.72, 7.85], [13.72, 12.15], [10, 14.3], [6.28, 12.15], [6.28, 7.85]];

export const controlIcons = {
  Columns: [panel, p('M10 3.5v13')],
  Add: [p('M10 4v12M4 10h12')],
  AddFolder: [p('M8.5 17H3.8q-1.3 0-1.3-1.3V5.3Q2.5 4 3.8 4h4.3q.5 0 .8.4l1.1 1.7q.3.4.8.4h5.4q1.3 0 1.3 1.3v2.7'), p('M14 11.5v6M11 14.5h6')],
  ArrowLeft: left,
  ArrowRight: right,
  Backwards: left,
  Check: [p('M4.5 10.2 8 13.5Q8.4 13.9 8.8 13.4L15.6 6.2')],
  ClearAll: [p('m3 4 3 3m0-3L3 7M9 5.5h8M3 11h14M3 16h14')],
  Close: [p('m5 5 10 10M15 5 5 15')],
  Copy: [r(6.5, 6.5, 10.5, 11), p('M12.5 3H4.6Q3 3 3 4.6v8')],
  Delete: [p('M3.2 5.5h13.6M7 5.5V3.7q0-1.2 1.2-1.2h3.6Q13 2.5 13 3.7v1.8M4.8 5.5l.6 10.3q.1 1.7 1.7 1.7h5.8q1.6 0 1.7-1.7l.6-10.3M8 8.8v5.3M12 8.8v5.3')],
  Document: [p('M5.2 2.5h6.2q.6 0 1 .4l3.2 3.2q.4.4.4 1v8.9q0 1.5-1.5 1.5H5.2q-1.5 0-1.5-1.5V4q0-1.5 1.5-1.5ZM11.7 2.7v3.7q0 .9.9.9h3.2'), p('M6.8 10h6.4M6.8 13.2h5')],
  Edit: [p('m4 12.2 9.1-9.1q.8-.8 1.6 0l2.2 2.2q.8.8 0 1.6l-9.1 9.1q-.2.2-.6.3l-3.6.6q-.5.1-.4-.4l.5-3.7q.1-.4.3-.6ZM11.8 4.4l3.8 3.8')],
  EditWindow: [p('M8.8 3.5H4.1q-1.6 0-1.6 1.6v10.8q0 1.6 1.6 1.6h10.8q1.6 0 1.6-1.6v-5'), p('m8.2 10.2 7.1-7.1q.6-.6 1.2 0l1.4 1.4q.6.6 0 1.2l-7.1 7.1-3.1.6q-.4.1-.3-.3l.6-2.9ZM13.8 4.6l2.6 2.6')],
  Ellipsis: ellipsis,
  EllipsisVertical: [dot(10, 4.5), dot(10, 10), dot(10, 15.5)],
  ExpandDown: down,
  ExpandRight: [p('m7.7 5.5 4.1 4.1q.4.4 0 .8l-4.1 4.1')],
  ExpandUp: up,
  FolderClosed: [p('M3.8 4h4.3q.5 0 .8.4l1.1 1.7q.3.4.8.4h5.4q1.3 0 1.3 1.3v7.9q0 1.3-1.3 1.3H3.8q-1.3 0-1.3-1.3V5.3Q2.5 4 3.8 4Z')],
  FolderOpened: [p('M2.5 13.5V5.3Q2.5 4 3.8 4h4.3q.5 0 .8.4l1.1 1.7q.3.4.8.4h4.5Q16.5 6.5 16.5 8'), p('M5.5 9h11q1.1 0 .9 1.1l-1.3 5.8q-.2 1.1-1.4 1.1H3.7q-1.1 0-.9-1.1l1.3-5.8Q4.3 9 5.5 9Z')],
  GlyphDown: down,
  GlyphUp: up,
  LeftBar: panelLeft,
  List: [p('M7.5 5h9M7.5 10h9M7.5 15h9'), dot(3.5, 5, .65), dot(3.5, 10, .65), dot(3.5, 15, .65)],
  ListPanel: [p('M3 5h10M3 10h14M3 15h8')],
  NewSession: [p('M10 3.5H6.5q-3 0-3 3v7q0 3 3 3h7q3 0 3-3v-2.5'), p('M8.6 9.9 15.1 3.4q.6-.6 1.2 0l.3.3q.6.6 0 1.2L10.1 11.4Q9.5 12 8.72 12.195L7.9 12.4Q7.5 12.5 7.6 12.1L7.805 11.28Q8 10.5 8.6 9.9Z')],
  OpenFile: [p('M8.5 17H5q-1.5 0-1.5-1.5V4q0-1.5 1.5-1.5h6.4q.6 0 1 .4l3.2 3.2q.4.4.4 1V8M11.5 2.8v3.6q0 .9.9.9h3.2'), c(12.4, 12.5, 3), p('m14.6 14.7 2.4 2.4')],
  PanelBottom: [panel, p('M2.5 12.5h15')],
  PanelBottomFilled: [panel, p('M2.5 12.5h15v2.4q0 1.6-1.6 1.6H4.1q-1.6 0-1.6-1.6Z', { fill: 'currentColor', stroke: 'none' }), p('M2.5 12.5h15')],
  PanelLeft: panelLeft,
  PanelLeftFilled: [panel, p('M4.1 3.5h3.4v13H4.1q-1.6 0-1.6-1.6V5.1q0-1.6 1.6-1.6Z', { fill: 'currentColor', stroke: 'none' }), p('M7.5 3.5v13')],
  PanelRight: panelRight,
  PanelRightFilled: [panel, p('M12.5 3.5h3.4q1.6 0 1.6 1.6v9.8q0 1.6-1.6 1.6h-3.4Z', { fill: 'currentColor', stroke: 'none' }), p('M12.5 3.5v13')],
  Queue: [p('M3 4.5h9M3 9h7M3 13.5h4M16 4.5v9q0 2-2 2h-4m2.5-2.5L10 15.5l2.5 2.5')],
  Refresh: [p('M16.5 8a6.8 6.8 0 0 0-11.4-3.3L3 7M3 3.5V7h3.5M3.5 12a6.8 6.8 0 0 0 11.4 3.3L17 13M13.5 13H17v3.5')],
  RightBar: panelRight,
  Run: [p('M6 3.8q0-.8.7-.4l10 6q.9.6 0 1.2l-10 6q-.7.4-.7-.4Z')],
  Save: [p('M4.2 2.8h10q.5 0 .9.4l1.7 1.7q.4.4.4.9v10q0 1.4-1.4 1.4H4.2q-1.4 0-1.4-1.4V4.2q0-1.4 1.4-1.4ZM6 2.8v4.4q0 .8.8.8h6.4q.8 0 .8-.8V2.8M6 17.2V12q0-.8.8-.8h6.4q.8 0 .8.8v5.2M11.5 4.3v2.2')],
  ScreenFull: [p('M3 7.5V4q0-1 1-1h3.5M12.5 3H16q1 0 1 1v3.5M17 12.5V16q0 1-1 1h-3.5M7.5 17H4q-1 0-1-1v-3.5')],
  ScreenNormal: [p('M3 7.5h3.5q1 0 1-1V3M12.5 3v3.5q0 1 1 1H17M17 12.5h-3.5q-1 0-1 1V17M7.5 17v-3.5q0-1-1-1H3')],
  Search: search,
  Send: [p('M3.4 3.3 17 9.4q1.1.6 0 1.2L3.4 16.7q-.8.4-.6-.5l1.7-5.6q.2-.6 0-1.2L2.8 3.8q-.2-.9.6-.5ZM4.7 10h7')],
  Stop: [r(4, 4, 12, 12, 1.8)],
  StopFilled: [r(3, 3, 14, 14, 1, { fill: 'currentColor', stroke: 'none' })],
  TerminalReadWrite: [r(2.5, 4.5, 15, 11), p('m5.5 7.5 2.1 2.1q.4.4 0 .8l-2.1 2.1M10.5 12.5h4')],
  WorkspaceMenu: ellipsis,
  // Functional symbols formerly drawn locally in individual components.
  ArrowUp: [p('M10 16.5V3.5M5.2 8.2l4.4-4.4q.4-.4.8 0l4.4 4.4')],
  ArrowDown: [p('M10 3.5v13M5.2 11.8l4.4 4.4q.4.4.8 0l4.4-4.4')],
  ArrowUpRight: [p('M4.5 15.5 15.5 4.5M6.5 4.5h8q1 0 1 1v8')],
  Clock: [c(10, 10, 7.1), p('M10 5.8v3.7q0 .5.4.8l2.8 1.7')],
  Compact: [p('M3 8V5q0-1 1-1h3M13 4h3q1 0 1 1v3M17 12v3q0 1-1 1h-3M7 16H4q-1 0-1-1v-3M6.5 8l2 2-2 2M13.5 8l-2 2 2 2')],
  Download: [p('M10 2.8v9.7M6.5 9.5l3.1 3.1q.4.4.8 0l3.1-3.1M3 12.5v3q0 1.5 1.5 1.5h11q1.5 0 1.5-1.5v-3')],
  Goal: [
    ...goalReticle,
    dot(10, 10, .9),
  ],
  GoalBlocked: [
    ...goalReticle,
    p('M5.9 5.9L14.1 14.1'),
  ],
  GripVertical: [dot(7.5, 5, .7), dot(12.5, 5, .7), dot(7.5, 10, .7), dot(12.5, 10, .7), dot(7.5, 15, .7), dot(12.5, 15, .7)],
  MagicWand: [p('m3.1 15.1 8.3-8.3q.5-.5 1 0l.8.8q.5.5 0 1l-8.3 8.3q-.5.5-1 0l-.8-.8q-.5-.5 0-1ZM9.2 9l1.8 1.8M6 2.5v3M4.5 4h3M15 10.5v3M13.5 12h3M14 2l.5 1.5L16 4l-1.5.5L14 6l-.5-1.5L12 4l1.5-.5Z')],
  Maximize: [r(4, 4, 12, 12, 1.4)],
  Minimize: [p('M4 10h12')],
  Minus: [p('M4 10h12')],
  Pause: [r(5, 4, 3.2, 12, 1), r(11.8, 4, 3.2, 12, 1)],
  Restore: [r(3, 6.5, 10.5, 10.5, 1.4), p('M6.5 6.5V4.4Q6.5 3 7.9 3h7.7Q17 3 17 4.4v7.7q0 1.4-1.4 1.4h-2.1')],
  ShieldWarning: [p('M10 2.5Q6.8 4.7 3.5 5v4.1q0 5.1 6.5 8.4 6.5-3.3 6.5-8.4V5Q13.2 4.7 10 2.5ZM10 6.5v4.2'), dot(10, 13.5, .65)],
  Signal: [p('M4 15v-3M8 15V9M12 15V6M16 15V3')],
  Sliders: [p('M3 6.5h4.5M10.5 6.5H17M3 13.5h7.5M13.5 13.5H17'), c(9, 6.5, 1.6), c(12, 13.5, 1.6)],
  Sparkle: [p('M8 3.5q.6 4.9 5.5 5.5Q8.6 9.6 8 14.5 7.4 9.6 2.5 9 7.4 8.4 8 3.5ZM15 10q.4 3.1 3.5 3.5-3.1.4-3.5 3.5-.4-3.1-3.5-3.5 3.1-.4 3.5-3.5ZM15.5 2.5v4M13.5 4.5h4')],
  Swarm: swarmCenters.map(([x, y], index) => p(softPolygon(Array.from({ length: 6 }, (_, n) => [x + 2.35 * Math.cos(n * Math.PI / 3), y + 2.35 * Math.sin(n * Math.PI / 3)])), index === 0 ? { fill: 'currentColor', fillOpacity: .14 } : {})),
  Upload: [p('M10 12.5V2.8M6.5 6l3.1-3.1q.4-.4.8 0L13.5 6M3 12.5v3q0 1.5 1.5 1.5h11q1.5 0 1.5-1.5v-3')],
  User: [c(10, 6, 3.2), p('M3.5 17v-.9q0-4.1 6.5-4.1t6.5 4.1v.9')],
  ZoomIn: [...search, p('M8.7 5.8v5.8M5.8 8.7h5.8')],
  ZoomOut: [...search, p('M5.8 8.7h5.8')],
};
