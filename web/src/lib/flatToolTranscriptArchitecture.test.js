import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
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

const treeLayoutClass = /\b(?:ml|pl)-\d|\bborder-l\b/;

run('tool rows share the transcript width while system rows retain the assistant gutter', () => {
  const presentation = source('lib/transcriptItemPresentation.js');
  const rowClassName = between(
    presentation,
    'export function transcriptRowClassName',
    'export function transcriptRowAttrs',
  );

  assert.match(rowClassName, /transcriptItemRole\(item\) === 'system'[\s\S]*?'ace-chat-row-assistant-gutter'/);
  assert.doesNotMatch(rowClassName, /item\?\.kind === 'tool'/);
});

run('top-level and recursively expanded activity items use a flat content stack', () => {
  const renderer = source('components/TranscriptItems.jsx');
  const activity = between(
    renderer,
    "if (renderKind === 'activity_summary')",
    "const directive = item?.kind === 'msg'",
  );

  assert.match(activity, /<ActivityDetailsReveal>/);
  assert.match(activity, /<TranscriptItems[\s\S]*?nested/);
  assert.doesNotMatch(activity, treeLayoutClass);
});

run('expanded subagent rows do not restore a tree rail or nested offset', () => {
  const group = source('components/SubagentGroupBlock.jsx');

  assert.match(group, /className="mt-1 flex flex-col gap-0\.5"/);
  assert.doesNotMatch(group, treeLayoutClass);
});

run('all passive tool lifecycle rows reuse the same fixed-height ActivityLine shell', () => {
  const activityLine = source('components/ActivityLine.jsx');
  const renderer = source('components/TranscriptItems.jsx');
  const tool = source('components/ToolBlock.jsx');
  const subagent = source('components/SubagentGroupBlock.jsx');

  assert.match(activityLine, /data-unified-activity-line="true"/);
  assert.match(activityLine, /flex h-7 w-full/);
  assert.match(activityLine, /flex h-4 w-4 shrink-0/);
  assert.doesNotMatch(activityLine, /<button/);
  assert.match(renderer, /function ActivitySummaryBlock[\s\S]*?<ActivityLine/);
  assert.match(tool, /import \{ ActivityLine \}/);
  assert.ok((tool.match(/<ActivityLine/g) || []).length >= 3);
  assert.match(tool, /const completedSummary = summary \|\| genericSummary;/);
  assert.match(subagent, /<ActivityLine/);
});

run('single-line activity UI uses the same responsive font size as assistant body text', () => {
  const styles = source('styles/globals.css');
  const subagent = source('components/SubagentGroupBlock.jsx');

  assert.match(
    styles,
    /\.ace-activity-line\s*\{\s*--ace-tool-call-font-size:\s*var\(--ace-font-size-body\);\s*\}/,
  );
  assert.match(
    styles,
    /font-size:\s*var\(--ace-tool-call-font-size,\s*var\(--ace-font-size-code-compact\)\)\s*!important;/,
  );
  assert.match(subagent, /truncate text-\[13px\] text-fg/);
  assert.doesNotMatch(subagent, /truncate text-\[12\.5px\] text-fg/);
});

run('processed summary divider paints across the row without changing layout height', () => {
  const renderer = source('components/TranscriptItems.jsx');
  const styles = source('styles/globals.css');
  const divider = between(
    styles,
    '.ace-activity-line-processed::after',
    '/* Tool 中的真实代码与 diff 不继承上面的正文覆盖。 */',
  );

  assert.match(renderer, /item\?\.mode === 'processed' \? 'ace-activity-line-processed' : ''/);
  assert.match(styles, /\.ace-activity-line-processed\s*\{\s*position:\s*relative;/);
  assert.match(divider, /position:\s*absolute;/);
  assert.match(divider, /right:\s*0;/);
  assert.match(divider, /left:\s*0;/);
  assert.match(divider, /height:\s*1px;/);
  assert.doesNotMatch(divider, /(?:^|\n)\s*(?:margin|padding|border(?:-[a-z-]+)?):/);
});

run('activity chevrons sit beside content, reveal on hover, and stay visible for processed summaries', () => {
  const activityLine = source('components/ActivityLine.jsx');
  const styles = source('styles/globals.css');
  const chevronIndex = activityLine.indexOf('ace-activity-line-chevron');
  const spacerIndex = activityLine.indexOf('className="min-w-0 flex-1"', chevronIndex);
  const trailingIndex = activityLine.indexOf('{trailing &&', chevronIndex);

  assert.notEqual(chevronIndex, -1);
  assert.ok(chevronIndex < spacerIndex);
  assert.ok(spacerIndex < trailingIndex);
  assert.match(activityLine, /d="M4 6L8 10L12 6"/);
  assert.match(activityLine, /strokeWidth="1\.2"/);
  assert.match(activityLine, /rotate\(\$\{expanded \? 0 : -90\}deg\)/);
  assert.doesNotMatch(activityLine, /name=\{expanded \? 'expandDown' : 'expandRight'\}/);
  assert.match(styles, /\.ace-activity-line-chevron\s*\{[\s\S]*?opacity:\s*0;/);
  assert.match(styles, /\.ace-activity-line:hover \.ace-activity-line-chevron,[\s\S]*?color:\s*var\(--ace-fg\);[\s\S]*?opacity:\s*1;/);
  assert.match(styles, /\.ace-activity-line-processed \.ace-activity-line-chevron\s*\{\s*opacity:\s*1;/);
  assert.match(
    styles,
    /\.ace-activity-line-chevron svg\s*\{[\s\S]*?transition:\s*transform 150ms cubic-bezier\(\.2, 0, 0, 1\);/,
  );
});

run('expanded activity details quickly draw downward and release clipping after entry', () => {
  const renderer = source('components/TranscriptItems.jsx');
  const styles = source('styles/globals.css');
  const revealComponent = between(renderer, 'function ActivityDetailsReveal', 'function ActivitySummaryBlock');
  const revealStyles = between(
    styles,
    '@keyframes ace-activity-details-reveal',
    '/* Tool 中的真实代码与 diff 不继承上面的正文覆盖。 */',
  );

  assert.match(revealComponent, /data-activity-details-reveal="true"/);
  assert.match(revealComponent, /ace-activity-details-reveal-inner mt-1 flex min-h-0 flex-col gap-0\.5/);
  assert.match(revealComponent, /event\.target === event\.currentTarget/);
  assert.match(revealStyles, /from\s*\{[\s\S]*?grid-template-rows:\s*0fr;/);
  assert.match(revealStyles, /to\s*\{[\s\S]*?grid-template-rows:\s*1fr;/);
  assert.match(
    revealStyles,
    /\.ace-activity-details-reveal\s*\{[\s\S]*?min-width:\s*0;[\s\S]*?width:\s*100%;/,
  );
  assert.match(
    revealStyles,
    /\.ace-activity-details-reveal-inner\s*\{[\s\S]*?min-width:\s*0;[\s\S]*?width:\s*100%;/,
  );
  assert.match(revealStyles, /animation:\s*ace-activity-details-reveal 150ms cubic-bezier\(\.2, 0, 0, 1\) both;/);
  assert.match(revealStyles, /\.ace-activity-details-reveal\.is-settled\s*\{\s*overflow:\s*visible;\s*animation:\s*none;/);
  assert.match(revealStyles, /@media \(prefers-reduced-motion:\s*reduce\)[\s\S]*?animation:\s*none;/);
  assert.match(revealStyles, /@media \(prefers-reduced-motion:\s*reduce\)[\s\S]*?\.ace-activity-line-chevron svg\s*\{\s*transition:\s*none;/);
  assert.match(renderer, /\{expanded && \(\s*<ActivityDetailsReveal>/);
  assert.match(renderer, /<TranscriptItems[\s\S]*?nested/);
});

run('activity summaries pass their actual title element into expansion anchoring', () => {
  const activityLine = source('components/ActivityLine.jsx');
  const renderer = source('components/TranscriptItems.jsx');

  assert.match(activityLine, /data-activity-title-anchor="true"/);
  assert.match(activityLine, /onToggle\(event\);/);
  assert.match(
    renderer,
    /onToggle=\{\(event\) => onToggleActivity\?\.\(item\.id, event\?\.currentTarget\)\}/,
  );
});

run('bottom loading is projected into ActivityLine and the old bubble is gone', () => {
  const chat = source('components/ChatView.jsx');
  const renderer = source('components/TranscriptItems.jsx');

  assert.match(chat, /ensureLiveActivity: busy/);
  assert.match(chat, /liveTurnId: currentTurnActivityId\(rawItems, activeTurnId, sid\)/);
  assert.match(renderer, /activity\?\.label \|\| item\?\.title/);
  assert.doesNotMatch(chat, /function ActivityIndicator/);
  assert.doesNotMatch(chat, /data-conversation-activity-bubble/);
  assert.doesNotMatch(chat, /ace-pulse/);
});

console.log('flatToolTranscriptArchitecture tests passed');
