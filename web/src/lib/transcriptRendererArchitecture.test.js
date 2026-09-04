import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function sourceFiles(directory = srcRoot) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) return sourceFiles(absolute);
    if (!entry.isFile() || !/\.(?:js|jsx)$/.test(entry.name) || entry.name.endsWith('.test.js')) {
      return [];
    }
    return [path.relative(srcRoot, absolute).replace(/\\/g, '/')];
  });
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

run('main, expanded, and sub-agent transcript paths share one renderer', () => {
  const chat = source('components/ChatView.jsx');
  const panel = source('components/SubagentPanel.jsx');
  const renderer = source('components/TranscriptItems.jsx');

  assert.match(chat, /import \{ TranscriptItems \} from '\.\/TranscriptItems\.jsx';/);
  assert.equal((chat.match(/<TranscriptItems/g) || []).length, 1);
  assert.doesNotMatch(chat, /renderExpandedActivityItems|windowedItems\.map\(/);
  assert.match(panel, /import \{ TranscriptItems \} from '\.\/TranscriptItems\.jsx';/);
  assert.equal((panel.match(/<TranscriptItems/g) || []).length, 1);
  assert.doesNotMatch(panel, /<Message|<ToolBlock|items\.map\(/);
  assert.match(renderer, /<TranscriptItems[\s\S]*?items=\{detailItems\}[\s\S]*?nested/);
});

run('shared renderer is the only chat surface that dispatches projected item kinds', () => {
  const renderer = source('components/TranscriptItems.jsx');

  for (const kind of [
    'termination_notice',
    'completion_summary',
    'media_group',
    'subagent_group',
    'activity_summary',
    'tool',
  ]) {
    assert.match(renderer, new RegExp(`renderKind === '${kind}'`));
  }

  // MiniSession is a deliberately tiny card projection rather than a full transcript;
  // TrajectoryView's `tool` cells belong to its own timeline schema.
  const intentionalNonTranscriptDispatchers = new Set([
    'components/MiniSession.jsx',
    'components/trajectory/TrajectoryView.jsx',
  ]);
  const projectedKindDispatch = /\b[\w$]+(?:\?\.)?\.kind\s*===\s*['"](?:tool|activity_summary|completion_summary|media_group|subagent_group|termination_notice)['"]/;
  const offenders = sourceFiles()
    .filter((relativePath) => /\.jsx$/.test(relativePath))
    .filter((relativePath) => relativePath !== 'components/TranscriptItems.jsx')
    .filter((relativePath) => !intentionalNonTranscriptDispatchers.has(relativePath))
    .filter((relativePath) => projectedKindDispatch.test(source(relativePath)));
  assert.deepEqual(offenders, []);

  const fullTranscriptConsumers = sourceFiles()
    .filter((relativePath) => /\.jsx$/.test(relativePath))
    .filter((relativePath) => /project(?:Collapsed|Subagent)TranscriptItems/.test(source(relativePath)))
    .sort();
  assert.deepEqual(fullTranscriptConsumers, [
    'components/ChatView.jsx',
    'components/SubagentPanel.jsx',
  ]);
  for (const relativePath of fullTranscriptConsumers) {
    assert.doesNotMatch(
      source(relativePath),
      /<(?:Message|ToolBlock|ActivitySummaryBlock|CompletionSummaryBlock|MediaGroupBlock|SubagentGroupBlock|TerminationNoticeBlock)\b/,
      `${relativePath} must delegate projected rows to TranscriptItems`,
    );
  }
});

run('sub-agent transcript uses full collapse projection with explicit read-only capabilities', () => {
  const panel = source('components/SubagentPanel.jsx');
  const helper = source('lib/subagentTranscript.js');

  assert.match(helper, /projectCollapsedTranscriptItems\(items,/);
  assert.match(helper, /filterNormalizedItem: isSubagentTranscriptItemVisible/);
  assert.match(panel, /projectSubagentTranscriptItems\(transcript\.items,/);
  assert.match(panel, /capabilities=\{READ_ONLY_TRANSCRIPT_CAPABILITIES\}/);
  assert.doesNotMatch(panel, /normalizeToolInvocationItems|transcriptItemsForPanel/);
});

run('sub-agent tail following reuses the shared state machine and content observer', () => {
  const panel = source('components/SubagentPanel.jsx');

  assert.match(panel, /nextChatTailFollowState\(tailFollowStateRef\.current, action\)/);
  assert.match(panel, /observeChatTailContent\(\s*contentRef\.current,/);
  assert.match(panel, /shouldAutoFollowChatTail\(tailFollowStateRef\.current\)/);
  assert.doesNotMatch(panel, /scrollHeight - .*clientHeight < 48/);
});

run('sub-agent read-only boundary omits main-session mutation and navigation callbacks', () => {
  const panel = source('components/SubagentPanel.jsx');
  const message = source('components/Message.jsx');
  const rendererCallStart = panel.indexOf('<TranscriptItems');
  const rendererCallEnd = panel.indexOf('/>', rendererCallStart);
  assert.ok(rendererCallStart >= 0 && rendererCallEnd > rendererCallStart);
  const rendererCall = panel.slice(rendererCallStart, rendererCallEnd);

  assert.doesNotMatch(rendererCall, /onFork=|onOpenFilePreview=|onLocateInFileTree=|annotationPresentations=/);
  assert.match(rendererCall, /capabilities=\{READ_ONLY_TRANSCRIPT_CAPABILITIES\}/);
  assert.match(message, /function UserBubble\(\{[\s\S]*?showFooter,[\s\S]*?\{showFooter && \(/);
  assert.match(message, /<UserBubble[\s\S]*?showFooter=\{showFooter\}/);
});

console.log('transcriptRendererArchitecture tests passed');
