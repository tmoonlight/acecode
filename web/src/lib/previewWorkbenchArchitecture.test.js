import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

function source(relativePath) {
  return readFileSync(new URL(relativePath, import.meta.url), 'utf8');
}

function between(text, start, end) {
  const from = text.indexOf(start);
  const to = text.indexOf(end, from + start.length);
  assert.ok(from >= 0, `missing start marker: ${start}`);
  assert.ok(to > from, `missing end marker: ${end}`);
  return text.slice(from, to);
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

run('preview plus menu exposes exactly terminal, browser and side chat actions', () => {
  const preview = source('../components/PreviewDetailsPanel.jsx');
  const menu = between(preview, 'const items = useMemo(() => [', '], [onOpenBrowser');
  assert.deepEqual(
    [...menu.matchAll(/label: '([^']+)'/g)].map((match) => match[1]),
    ['终端', '浏览器', '侧边聊天'],
  );
  assert.equal((menu.match(/key:/g) || []).length, 3);
  assert.match(preview, /aria-haspopup="menu"/);
  assert.match(preview, /data-ace-native-overlay="overlap"/);
  assert.match(preview, /event\.key === 'Escape'/);
});

run('details close button hides the panel without closing tabs or Browser pages', () => {
  const preview = source('../components/PreviewDetailsPanel.jsx');
  const chat = source('../components/ChatView.jsx');
  const closeTitleIndex = preview.indexOf('title="隐藏详情面板"');
  const closeButtonStart = preview.lastIndexOf('<button', closeTitleIndex);
  const closeButtonEnd = preview.indexOf('</button>', closeTitleIndex);
  const closeAction = preview.slice(closeButtonStart, closeButtonEnd);
  const hideCallback = between(chat, 'const hidePreviewPanel = useCallback', 'const reorderPreview');

  assert.match(closeAction, /onClick=\{onHide\}/);
  assert.doesNotMatch(closeAction, /onCloseAll/);
  assert.match(chat, /previewTabsOpen && !sidePanelCollapsed && !previewPanelHidden/);
  assert.match(hideCallback, /setPreviewPanelHidden\(true\)/);
  assert.doesNotMatch(hideCallback, /closePreviewTab|closeVisiblePreviewTabs|closeAgentBrowserPage/);
  assert.match(chat, /const openFilePreview[\s\S]*setPreviewPanelHidden\(false\)/);
  assert.match(chat, /const showBrowserPage[\s\S]*setPreviewPanelHidden\(false\)/);
});

run('dirty file tabs show a solid dot and every destructive tab action is guarded', () => {
  const preview = source('../components/PreviewDetailsPanel.jsx');
  const chat = source('../components/ChatView.jsx');
  const styles = source('../styles/globals.css');

  assert.match(preview, /const dirty = previewTabHasUnsavedDraft\(tab\)/);
  assert.match(preview, /ace-preview-details-tab-dirty-dot/);
  assert.match(styles, /\.ace-preview-details-tab-dirty-dot\s*\{[\s\S]*width: 9px;[\s\S]*border-radius: 999px/);
  assert.match(chat, /previewTabsWithUnsavedDrafts\(affected\)/);
  for (const kind of ['one', 'all', 'others', 'right']) {
    assert.match(chat, new RegExp(`requestPreviewClose\\('${kind}'`));
  }
  assert.match(chat, />\s*放弃并关闭\s*</);
});

run('file details use source editing for text and semantic WYSIWYG for Markdown', () => {
  const filePreview = source('../components/FilePreviewContent.jsx');
  const markdownEditor = source('../components/MarkdownWysiwygEditor.jsx');

  assert.match(filePreview, /api\.readEditableFile\(cwd, path\)/);
  assert.match(filePreview, /api\.saveEditableFile\(cwd, path, text, current\.readId\)/);
  assert.match(filePreview, /<MarkdownWysiwygEditor/);
  assert.match(filePreview, /<textarea[\s\S]*className="ace-file-text-editor"/);
  assert.match(markdownEditor, /markdownToSlate\(normalizedValue\)/);
  assert.match(markdownEditor, /slateToMarkdown\(legalDocument\(document\)\)/);
  assert.match(markdownEditor, /compositionRef\.current\.active/);
  assert.match(markdownEditor, /event\.key\.toLowerCase\(\) === 's'/);
});

run('side chat owns a separate draft and dispatches through the existing side-question API', () => {
  const chat = source('../components/ChatView.jsx');
  const composer = source('../components/SideQuestionComposer.jsx');
  const app = source('../App.jsx');

  assert.match(chat, /const \[sideQuestionDraft, setSideQuestionDraft\] = useState\(''\)/);
  assert.match(chat, /runSideQuestion\(sideQuestionDraft, \{ command: 'side' \}\)/);
  assert.match(chat, /<SideQuestionComposer[\s\S]*value=\{sideQuestionDraft\}/);
  assert.match(composer, /不会改变主输入草稿/);
  assert.match(app, /onOpenConsole=\{consoleAvailable \? \(\) => setConsoleDockOpen\(true\) : null\}/);
});
