import assert from 'node:assert/strict';
import fs from 'node:fs';
import { DEFAULT_SINGLE_LAYOUT, solveSingleContentLayout } from './singleLayout.js';
import { clampPopupWidth, popupWidthRange, solveResponsiveSidebars, SIDEBAR_RESTORE_MARGIN } from './responsiveSidebars.js';

const defaults = {
  widths: DEFAULT_SINGLE_LAYOUT,
  preferences: { sidebarCollapsed: false, sidePanelCollapsed: false, sidePanelListCollapsed: false },
};
const solve = (width, options = {}) => solveResponsiveSidebars({ ...defaults, shellWidth: width, ...options });
function run(name, test) { test(); console.log(`[pass] ${name}`); }

run('popup resize bounds leave an outside click area and preserve the saved width across viewport clamps', () => {
  const savedWidth = 620;
  const range = popupWidthRange(800, 420);
  assert.deepEqual(range, { min: 420, max: 776 });
  assert.equal(clampPopupWidth(100, range), 420);
  assert.equal(clampPopupWidth(1000, range), 776);
  assert.equal(clampPopupWidth(savedWidth, popupWidthRange(480, 420)), 456);
  assert.equal(clampPopupWidth(savedWidth, range), savedWidth);
  assert.deepEqual(popupWidthRange(320, 420), { min: 296, max: 296 });
  assert.deepEqual(popupWidthRange(0, 160), { min: 0, max: 0 });
});

run('resizing either popup never changes the docked layout decision', () => {
  for (const width of [700, 800, 1100, 1400]) {
    const initial = solve(width, { previewAvailable: true });
    for (const sidebarPopup of [160, 400, 650]) for (const rightPanelPopup of [420, 600, 900]) {
      assert.deepEqual(solve(width, { previewAvailable: true,
        widths: { ...DEFAULT_SINGLE_LAYOUT, sidebarPopup, rightPanelPopup } }), initial);
    }
  }
});

run('transcript pressure collapses right first then left, including a wide custom left bar', () => {
  for (const sidebar of [160, 270, 360]) {
    const widths = { ...DEFAULT_SINGLE_LAYOUT, sidebar };
    let previous = solve(1920, { widths });
    assert.equal(previous.leftDocked, true);
    assert.equal(previous.rightDocked, true);
    let rightCollapsed = false;
    for (let width = 1920; width >= 320; width -= 1) {
      previous = solve(width, { widths, previous });
      if (previous.rightAutoCollapsed) rightCollapsed = true;
      if (previous.leftAutoCollapsed) {
        assert.ok(rightCollapsed);
        assert.equal(previous.rightDocked, false);
      }
      const transcript = width - (previous.leftDocked ? sidebar : 0)
        - (previous.rightDocked ? widths.sidePanel : 0) - 6;
      assert.ok(transcript >= Math.min(350, width - 6));
    }
  }
});

run('every font size keeps docked sidebars at 350px of transcript and collapses below it', () => {
  for (const fontSize of ['small', 'medium', 'large']) {
    const rightThreshold = 270 + 280 + 350 + 6;
    assert.equal(solve(rightThreshold, { fontSize }).rightDocked, true);
    assert.equal(solve(rightThreshold - 1, { fontSize }).rightDocked, false);
    const leftThreshold = 270 + 350 + 6;
    assert.equal(solve(leftThreshold, { fontSize }).leftDocked, true);
    assert.equal(solve(leftThreshold - 1, { fontSize }).leftDocked, false);
  }
});

run('restore uses predicted expanded geometry plus hysteresis, without oscillation', () => {
  const threshold = 270 + 280 + 356;
  let state = solve(threshold - 1);
  assert.equal(state.rightDocked, false);
  for (let i = 0; i < 30; i++) {
    state = solve(threshold + (i % 2 ? 1 : -1), { previous: state });
    assert.equal(state.rightDocked, false);
  }
  state = solve(threshold + SIDEBAR_RESTORE_MARGIN, { previous: state });
  assert.equal(state.rightDocked, true);
  state = solve(threshold, { previous: state });
  assert.equal(state.rightDocked, true);
});

run('all persisted open/closed combinations survive shrink, grow and a fresh load', () => {
  for (const sidebarCollapsed of [true, false]) for (const sidePanelCollapsed of [true, false]) {
    const preferences = Object.freeze({ sidebarCollapsed, sidePanelCollapsed, sidePanelListCollapsed: false });
    const options = { preferences };
    let previous = solve(1920, options);
    for (const width of [1100, 800, 500, 320, 500, 800, 1100, 1920]) {
      previous = solve(width, { ...options, previous });
      if (sidebarCollapsed) assert.equal(previous.leftDocked, false);
      if (sidePanelCollapsed) assert.equal(previous.rightDocked, false);
    }
    assert.equal(previous.leftDocked, !sidebarCollapsed);
    assert.equal(previous.rightDocked, !sidePanelCollapsed);
    assert.deepEqual(previous, solve(1920, options));
  }
});

run('previews and subagent transcripts contribute real width, while auto-fit can retain readable chat', () => {
  assert.equal(solve(1300).rightDocked, true);
  assert.equal(solve(1300, { previewAvailable: true }).rightDocked, false);
  assert.equal(solve(1200, { subagentOpen: true }).rightDocked, false);
  const state = solve(1920, { previewAvailable: true, previewAutoFit: true });
  assert.equal(state.rightDocked, true);
  const layout = solveSingleContentLayout({ contentWidth: 1920 - 270, previewPanelVisible: true,
    minimumChatWidth: state.minimumChatWidth, previewPanelAutoFit: true });
  assert.ok(layout.chatWidth - 6 >= 350);
});

run('a closed panel cannot expand in place unless its predicted width fits', () => {
  const preferences = { sidebarCollapsed: true, sidePanelCollapsed: true };
  assert.equal(solve(600, { preferences }).canDockLeft, false);
  assert.equal(solve(600, { preferences }).canDockRight, false);
  assert.equal(solve(1920, { preferences }).canDockLeft, true);
  assert.equal(solve(1920, { preferences }).canDockRight, true);
  assert.equal(solve(900, { preferences: { ...preferences, sidePanelCollapsed: false } }).canDockLeft, false);
});

run('explicit maximized preview and disabled layout retain their existing behavior', () => {
  const maximized = solve(800, { previewAvailable: true,
    preferences: { ...defaults.preferences, sidePanelMaximized: true } });
  assert.equal(maximized.active, false);
  assert.equal(maximized.rightDocked, true);
  assert.equal(solve(320, { enabled: false }).leftDocked, true);
});

run('the native window minimum allows reaching a transcript-only layout for every supported sidebar width', () => {
  const header = fs.readFileSync(new URL('../../../src/apps/desktop/web_host.hpp', import.meta.url), 'utf8');
  const minimum = Number(header.match(/kMinimumDesktopWindowWidth\s*=\s*(\d+)/)?.[1]);
  assert.ok(minimum > 0, 'read the production native window minimum');
  for (const sidebar of [160, 270, 360]) {
    const state = solve(minimum, { widths: { ...DEFAULT_SINGLE_LAYOUT, sidebar } });
    assert.equal(state.leftDocked, false, `native minimum ${minimum} must allow the ${sidebar}px left sidebar to collapse`);
    assert.equal(state.rightDocked, false, 'native resizing must reach a transcript-only layout');
  }
});
