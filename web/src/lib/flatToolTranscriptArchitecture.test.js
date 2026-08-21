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
  const chat = source('components/ChatView.jsx');
  const rowClassName = between(chat, 'function chatRowClassName', 'function ChatFileDropOverlay');

  assert.match(rowClassName, /role === 'system' && 'ace-chat-row-assistant-gutter'/);
  assert.doesNotMatch(rowClassName, /item\?\.kind === 'tool'/);
});

run('top-level and recursively expanded activity items use a flat content stack', () => {
  const chat = source('components/ChatView.jsx');
  const recursiveItems = between(
    chat,
    'function renderExpandedActivityItems',
    'const chatColumnStyle',
  );
  const topLevelActivity = between(
    chat,
    "if (it.kind === 'activity_summary')",
    "const directive = it.kind === 'msg'",
  );

  assert.match(recursiveItems, /className="mt-1 flex flex-col gap-0\.5"/);
  assert.match(topLevelActivity, /className="mt-1 flex flex-col gap-0\.5"/);
  assert.doesNotMatch(recursiveItems, treeLayoutClass);
  assert.doesNotMatch(topLevelActivity, treeLayoutClass);
});

run('expanded subagent rows do not restore a tree rail or nested offset', () => {
  const group = source('components/SubagentGroupBlock.jsx');

  assert.match(group, /className="mt-1 flex flex-col gap-0\.5"/);
  assert.doesNotMatch(group, treeLayoutClass);
});

run('all passive tool lifecycle rows reuse the same fixed-height ActivityLine shell', () => {
  const activityLine = source('components/ActivityLine.jsx');
  const chat = source('components/ChatView.jsx');
  const tool = source('components/ToolBlock.jsx');
  const subagent = source('components/SubagentGroupBlock.jsx');

  assert.match(activityLine, /data-unified-activity-line="true"/);
  assert.match(activityLine, /flex h-7 w-full/);
  assert.match(activityLine, /flex h-4 w-4 shrink-0/);
  assert.doesNotMatch(activityLine, /<button/);
  assert.match(chat, /function ActivitySummaryBlock[\s\S]*?<ActivityLine/);
  assert.match(tool, /import \{ ActivityLine \}/);
  assert.ok((tool.match(/<ActivityLine/g) || []).length >= 4);
  assert.match(subagent, /<ActivityLine/);
});

run('bottom loading is projected into ActivityLine and the old bubble is gone', () => {
  const chat = source('components/ChatView.jsx');

  assert.match(chat, /ensureLiveActivity: busy/);
  assert.match(chat, /liveTurnId: currentTurnActivityId\(rawItems, activeTurnId, sid\)/);
  assert.match(chat, /activity\?\.label \|\| item\?\.title/);
  assert.doesNotMatch(chat, /function ActivityIndicator/);
  assert.doesNotMatch(chat, /data-conversation-activity-bubble/);
  assert.doesNotMatch(chat, /ace-pulse/);
});

console.log('flatToolTranscriptArchitecture tests passed');
