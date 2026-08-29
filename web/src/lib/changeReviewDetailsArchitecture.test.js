import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const componentsRoot = path.join(srcRoot, 'components');
const stylesRoot = path.join(srcRoot, 'styles');

function source(name) {
  return fs.readFileSync(path.join(componentsRoot, name), 'utf8');
}

function styles(name) {
  return fs.readFileSync(path.join(stylesRoot, name), 'utf8');
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

run('Shared review details default to single-column wrapped diffs with manual toolbar controls', () => {
  const shared = source('ChangeReviewDetails.jsx');
  const globals = styles('globals.css');

  assert.match(shared, /const \[sideBySide, setSideBySide\] = useState\(false\);/);
  assert.match(shared, /const \[wrapLines, setWrapLines\] = useState\(true\);/);
  assert.match(shared, /const outputFormat = sideBySide \? 'side-by-side' : 'line-by-line';/);
  assert.doesNotMatch(shared, /REVIEW_SIDE_BY_SIDE_MIN_WIDTH|ResizeObserver/);
  assert.match(
    shared,
    /ace-change-del[\s\S]*?aria-label="双列"[\s\S]*?aria-pressed=\{sideBySide\}[\s\S]*?aria-label="自动换行"[\s\S]*?aria-pressed=\{wrapLines\}[\s\S]*?ace-review-collapse-all-btn/,
  );
  assert.match(shared, /wrapLines && 'is-wrapped'/);
  assert.match(shared, /<PatchDiff[\s\S]*?outputFormat=\{outputFormat\}[\s\S]*?wrapLines=\{wrapLines\}/);

  assert.match(globals, /\.ace-review-layout-btn\[aria-pressed="true"\][\s\S]*?var\(--ace-accent\)/);
  assert.match(globals, /\.ace-review-diff \{\s*min-width: 720px;/);
  assert.match(globals, /\.ace-review-diff\.is-wrapped \{\s*min-width: 0;/);
  assert.match(
    globals,
    /\.ace-review-diff\.is-wrapped \.d2h-code-side-linenumber,\s*\.ace-review-diff\.is-wrapped \.d2h-code-linenumber \{\s*display: table-cell;/,
  );
  assert.match(
    globals,
    /\.ace-review-diff\.is-wrapped \.d2h-code-line-ctn[\s\S]*?white-space: pre-wrap;[\s\S]*?overflow-wrap: anywhere;/,
  );
});

run('File tree and Git/non-Git review rows expose shared Explorer reveal metadata', () => {
  const fileTree = source('SidePanel.jsx');
  const gitList = source('GitChangesPanel.jsx');
  const sessionReview = source('ChangeReview.jsx');
  const compactList = source('ChangeFileList.jsx');
  const sharedDetails = source('ChangeReviewDetails.jsx');

  assert.match(fileTree, /data-desktop-file-absolute-path=\{absolutePath \|\| undefined\}/);
  assert.match(gitList, /<ChangeFileList[\s\S]*?guardDeletedPreview/);
  assert.match(sessionReview, /<ChangeFileList/);
  assert.match(compactList, /data-desktop-review-absolute-path=\{absolutePath\}/);
  assert.match(compactList, /data-desktop-review-can-reveal=\{canReveal == null \? undefined : String\(canReveal\)\}/);
  assert.match(compactList, /data-desktop-review-additions=\{additions\}/);
  assert.match(compactList, /data-desktop-review-deletions=\{deletions\}/);
  assert.match(sharedDetails, /data-desktop-review-absolute-path=\{cwd \? joinWorkspacePath\(cwd, row\.path\) : undefined\}/);
  assert.match(sharedDetails, /data-desktop-review-can-reveal=\{(?:status|row\.status) === 'D' \? 'false' : 'true'\}/);
});

run('Git and session compact changes share one flat/tree renderer and one cwd-scoped preference owner', () => {
  const sidePanel = source('SidePanel.jsx');
  const gitList = source('GitChangesPanel.jsx');
  const sessionReview = source('ChangeReview.jsx');
  const compactList = source('ChangeFileList.jsx');

  assert.match(gitList, /import \{ ChangeFileList \} from '\.\/ChangeFileList\.jsx';/);
  assert.match(sessionReview, /import \{ ChangeFileList \} from '\.\/ChangeFileList\.jsx';/);
  assert.match(gitList, /<ChangeFileList[\s\S]*?viewMode=\{viewMode\}/);
  assert.match(sessionReview, /<ChangeFileList[\s\S]*?viewMode=\{viewMode\}/);

  assert.match(
    sidePanel,
    /usePreference\(\s*CHANGE_LIST_VIEW_BY_CWD_STORAGE_KEY,\s*DEFAULT_CHANGE_LIST_VIEW_BY_CWD,\s*validateChangeListViewByCwd,/,
  );
  assert.match(
    sidePanel,
    /const defaultChangeListView = filesEnabled\s*\? CHANGE_LIST_VIEW_TREE\s*:\s*CHANGE_LIST_VIEW_FLAT;/,
  );
  assert.match(
    sidePanel,
    /changeListViewForCwd\(\s*storedChangeListViewsByCwd,\s*cwd,\s*defaultChangeListView,\s*\)/,
  );
  assert.match(
    sidePanel,
    /setStoredChangeListViewsByCwd\(\(current\) => \([\s\S]*?updateChangeListViewForCwd\(current, cwd, viewMode\)/,
  );
  assert.match(sidePanel, /role="group" aria-label="变更文件展示方式"/);
  assert.match(sidePanel, /aria-pressed=\{changeListView === option\.key\}/);
  assert.equal((sidePanel.match(/CHANGE_LIST_VIEW_BY_CWD_STORAGE_KEY/g) || []).length, 2);
  assert.doesNotMatch(gitList, /usePreference\(/);
  assert.doesNotMatch(sessionReview, /usePreference\(/);

  assert.match(compactList, /role=\{treeMode \? 'tree' : undefined\}/);
  assert.match(compactList, /aria-expanded=\{!collapsed\}/);
  assert.match(compactList, /changeTreeAncestorPaths\(selectedFile, cwd\)/);
  assert.match(compactList, /data-change-compact-file=\{row\.path\}/);
});

run('Compact Changes directories preserve canonical paths and use responsive middle ellipsis', () => {
  const gitList = source('GitChangesPanel.jsx');
  const sessionReview = source('ChangeReview.jsx');
  const compactList = source('ChangeFileList.jsx');
  const globals = styles('globals.css');

  assert.match(gitList, /<ChangeFileList[\s\S]*?rows=\{rows\}/);
  assert.match(sessionReview, /<ChangeFileList[\s\S]*?rows=\{rows\}/);
  assert.match(compactList, /function ChangeDirectoryName\(\{ node \}\)/);
  assert.match(compactList, /Array\.isArray\(node\.segments\)/);
  assert.match(compactList, /ace-change-tree-path-prefix/);
  assert.match(compactList, /ace-change-tree-path-separator/);
  assert.match(compactList, /ace-change-tree-path-suffix/);
  assert.match(compactList, /onClick=\{\(\) => onToggle\(node\.path\)\}/);
  assert.match(compactList, /title=\{node\.path\}/);
  assert.match(compactList, /aria-label=\{node\.path\}/);

  assert.match(
    globals,
    /\.ace-change-tree-path-prefix \{[\s\S]*?text-overflow: ellipsis;[\s\S]*?white-space: nowrap;/,
  );
  assert.match(
    globals,
    /\.ace-change-tree-path-suffix \{[\s\S]*?flex: 0 0 auto;[\s\S]*?max-width: 60%;/,
  );
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
