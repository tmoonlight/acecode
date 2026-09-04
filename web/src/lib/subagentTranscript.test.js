import assert from 'node:assert/strict';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';
import {
  isSubagentTranscriptItemVisible,
  projectSubagentTranscriptItems,
} from './subagentTranscript.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function user(id, content = 'do it') {
  return { kind: 'msg', id, role: 'user', content, ts: id * 1000 };
}

function assistant(id, content = 'done') {
  return { kind: 'msg', id, role: 'assistant', content, ts: id * 1000 };
}

function tool(id, {
  name = 'file_read',
  verb = 'Read',
  object = `file-${id}.txt`,
  isDone = true,
} = {}) {
  return {
    kind: 'tool',
    id,
    ts: id * 1000,
    tool: {
      tool: name,
      isDone,
      success: isDone,
      summary: { verb, object, metrics: [] },
      output: isDone ? 'ok' : '',
      hunks: [],
      attachments: [],
    },
  };
}

function toolWrapper(id, role, content, toolCallId) {
  return {
    kind: 'msg',
    id,
    role,
    content,
    ts: id * 1000,
    metadata: { tool_call_id: toolCallId },
  };
}

run('main and sub-agent surfaces project the same completed tool run into one summary row', () => {
  const raw = [user(1), tool(2), tool(3), tool(4), assistant(5)];
  const main = projectCollapsedTranscriptItems(raw);
  const child = projectSubagentTranscriptItems(raw);
  assert.deepEqual(child, main);
  assert.deepEqual(child.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.deepEqual(child[1].coveredItemIds, [2, 3, 4]);
  assert.equal(child[1].collapsedItems.length, 3);
});

run('running sub-agent tools use the same live expandable activity row as the main transcript', () => {
  const child = projectSubagentTranscriptItems(
    [user(1), tool(2, { name: 'bash', verb: 'Ran', object: 'build', isDone: false })],
    {
      deferTrailingToolSummary: true,
      ensureLiveActivity: true,
      liveTurnId: 'subagent:child-1',
    },
  );
  assert.deepEqual(child.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.equal(child[1].live, true);
  assert.equal(child[1].collapsedItems.length, 1);
  assert.equal(child[1].collapsedItems[0].kind, 'tool');
  assert.equal(child[1].collapsedItems[0].tool.isDone, false);
});

run('AskUserQuestion stays filtered before child activity collapse', () => {
  const ask = tool(3, { name: 'AskUserQuestion', verb: 'Ask', object: 'choose' });
  ask.tool.toolCallId = 'call-ask';
  const raw = [
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: AskUserQuestion] {}', 'call-ask'),
    ask,
    toolWrapper(4, 'tool_result', 'User answered', 'call-ask'),
    tool(5),
    assistant(6),
  ];
  const child = projectSubagentTranscriptItems(raw);
  assert.equal(isSubagentTranscriptItemVisible(ask), false);
  assert.deepEqual(child.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.deepEqual(child[1].coveredItemIds, [5]);
  assert.equal(child[1].collapsedItems.some((item) => item.tool?.tool === 'AskUserQuestion'), false);
  assert.equal(child.some((item) => [2, 3, 4].includes(item.id)), false);
});
