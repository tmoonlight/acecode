import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const componentsRoot = path.join(srcRoot, 'components');

function source(name) {
  return fs.readFileSync(path.join(componentsRoot, name), 'utf8');
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

run('Git and session details share one review-panel renderer', () => {
  const shared = source('ChangeReviewDetails.jsx');
  const sessionAdapter = source('ChangeReview.jsx');
  const gitAdapter = source('GitChangeReview.jsx');

  assert.match(shared, /className="ace-review-panel"/);
  assert.match(shared, /className="ace-review-file-list"/);
  for (const adapter of [sessionAdapter, gitAdapter]) {
    assert.match(adapter, /import \{ ChangeReviewDetails \} from '\.\/ChangeReviewDetails\.jsx';/);
    assert.match(adapter, /<ChangeReviewDetails/);
    assert.doesNotMatch(adapter, /className="ace-review-panel"/);
    assert.doesNotMatch(adapter, /className="ace-review-file-list"/);
  }
});

run('File tree and Git/non-Git review rows expose shared Explorer reveal metadata', () => {
  const fileTree = source('SidePanel.jsx');
  const gitList = source('GitChangesPanel.jsx');
  const sessionReview = source('ChangeReview.jsx');
  const sharedDetails = source('ChangeReviewDetails.jsx');

  assert.match(fileTree, /data-desktop-file-absolute-path=\{absolutePath \|\| undefined\}/);
  assert.match(gitList, /data-desktop-review-absolute-path=\{cwd \? joinWorkspacePath\(cwd, row\.path\) : undefined\}/);
  assert.match(gitList, /data-desktop-review-can-reveal=\{row\.status === 'D' \? 'false' : 'true'\}/);
  assert.match(sessionReview, /data-desktop-review-absolute-path=\{cwd \? joinWorkspacePath\(cwd, group\.file\) : undefined\}/);
  assert.match(sharedDetails, /data-desktop-review-absolute-path=\{cwd \? joinWorkspacePath\(cwd, row\.path\) : undefined\}/);
  assert.match(sharedDetails, /data-desktop-review-can-reveal=\{(?:status|row\.status) === 'D' \? 'false' : 'true'\}/);
});

run('Top bar keeps direct task search while new-conversation and loop stay in quick actions', () => {
  const topBar = source('TopBar.jsx');
  assert.doesNotMatch(topBar, /<QuickBtn[^>]*title="新对话"/);
  assert.doesNotMatch(topBar, /<QuickBtn[^>]*title="循环"/);
  assert.match(
    topBar,
    /<QuickBtn title="前进"[\s\S]*?<\/QuickBtn>\s*<QuickBtn title="搜索任务" onClick=\{onOpenSearch\}>/,
  );
  assert.match(topBar, /invokeTopBarQuickAction/);
});

run('Top bar left and right panel buttons share pressed toggle state', () => {
  const topBar = source('TopBar.jsx');
  assert.match(topBar, /pressed=\{!sidebarCollapsed\}/);
  assert.match(topBar, /pressed=\{!rightPanelCollapsed\}/);
});
