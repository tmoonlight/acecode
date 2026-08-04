import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('normal composer places the goal tray directly above its card', () => {
  const inputBar = source('components/InputBar.jsx');
  const trayIndex = inputBar.indexOf('<GoalStatusBar');
  const composerIndex = inputBar.indexOf("'ace-composer-card relative", trayIndex);

  assert.match(inputBar, /import \{ GoalStatusBar \} from '\.\/GoalStatusBar\.jsx';/);
  assert.match(inputBar, /\{!isHero && goal && \(/);
  assert.ok(trayIndex >= 0);
  assert.ok(composerIndex > trayIndex);
  assert.match(inputBar, /onEdit=\{onGoalEdit\}/);
  assert.match(inputBar, /onStatusChange=\{onGoalStatusChange\}/);
  assert.match(inputBar, /onClear=\{onGoalClear\}/);
});

run('goal tray exposes Codex-aligned actions and uses the shared immediate modal', () => {
  const tray = source('components/GoalStatusBar.jsx');

  assert.match(tray, /import \{ Modal \} from '\.\/Modal\.jsx';/);
  assert.match(tray, /<Modal/);
  assert.match(tray, /label="编辑目标"/);
  assert.match(tray, /onStatusChange\?\.\(state\.statusAction\)/);
  assert.match(tray, /label="清除目标"/);
  assert.match(tray, /new ResizeObserver\(measure\)/);
  assert.match(tray, /window\.setInterval\(\(\) => setNowMs\(Date\.now\(\)\), 1000\)/);
  assert.match(tray, /event\.ctrlKey \|\| event\.metaKey/);
});

run('chat actions use the existing goal command route and retain authoritative events', () => {
  const chatView = source('components/ChatView.jsx');

  assert.match(chatView, /name: 'goal'/);
  assert.match(chatView, /display_text: `\/goal \$\{args\}`/);
  assert.match(chatView, /\(objective\) => runGoalCommand\('edit', objective\)/);
  assert.match(chatView, /\(action\) => runGoalCommand\(action\)/);
  assert.match(chatView, /\(\) => runGoalCommand\('clear'\)/);
  assert.match(chatView, /onGoalEdit=\{editGoal\}/);
  assert.match(chatView, /onGoalStatusChange=\{changeGoalStatus\}/);
  assert.match(chatView, /onGoalClear=\{clearGoal\}/);
  assert.doesNotMatch(chatView, /setGoal\(/);
});

run('running-turn interrupt remains separate from goal pause and resume', () => {
  const controls = source('lib/goalControl.js');
  const chatView = source('components/ChatView.jsx');
  const stopStart = chatView.indexOf('const stopCurrentWork = useCallback');
  const stopEnd = chatView.indexOf('const runGoalCommand', stopStart);
  const stopHandler = chatView.slice(stopStart, stopEnd);

  assert.match(controls, /visible: isBusy/);
  assert.match(controls, /action: isBusy \? 'abort' : 'none'/);
  assert.doesNotMatch(controls, /pause_goal/);
  assert.match(stopHandler, /if \(!sid \|\| !busy\) return;/);
  assert.match(stopHandler, /abort\(\);/);
  assert.doesNotMatch(stopHandler, /goal|executeCommand|pause|resume/);
});

run('goal tray styling is responsive and uses theme tokens', () => {
  const styles = source('styles/globals.css');
  const start = styles.indexOf('.ace-goal-status-bar');
  const end = styles.indexOf('/* WorkBuddy-style composer footer', start);
  const goalStyles = styles.slice(start, end >= 0 ? end : start + 2400);

  assert.ok(start >= 0);
  assert.match(goalStyles, /background: var\(--ace-surface\)/);
  assert.match(goalStyles, /border: 1px solid var\(--ace-border\)/);
  assert.match(goalStyles, /\.ace-goal-status-bar \+ \.ace-composer-card/);
  assert.match(goalStyles, /@media \(max-width: 560px\)/);
  assert.doesNotMatch(goalStyles, /#[0-9a-f]{3,8}\b/i);
});

console.log('goalStatusBarArchitecture tests passed');
