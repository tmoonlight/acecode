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

run('Desktop feedback has a dedicated route before generic builtins and messages', () => {
  const routing = source('lib/builtinCommandRouting.js');
  const routeStart = routing.indexOf('export function inputRouteForText');
  const routeEnd = routing.indexOf('export function sessionCreateOptionsForText', routeStart);
  const routeBody = routing.slice(routeStart, routeEnd);

  assert.match(routing, /export function desktopFeedbackRequestForText\(text\)/);
  assert.ok(
    routeBody.indexOf("kind: 'desktop_feedback'")
      < routeBody.indexOf("kind: 'side_question'"),
  );
  assert.ok(
    routeBody.indexOf("kind: 'desktop_feedback'")
      < routeBody.indexOf("kind: 'builtin'"),
  );
  assert.match(
    routing,
    /if \(desktopFeedbackRequestForText\(text\)[\s\S]*?return \{ auto_start: false \};/,
  );
});

run('ChatView submits feedback directly without agent or session-command paths', () => {
  const chatView = source('components/ChatView.jsx');
  const feedbackStart = chatView.indexOf("if (route.kind === 'desktop_feedback')");
  const feedbackEnd = chatView.indexOf("if (route.kind === 'side_question')", feedbackStart);
  const feedbackBlock = chatView.slice(feedbackStart, feedbackEnd);
  const genericBuiltin = chatView.indexOf('const isBuiltin =', feedbackEnd);

  assert.notEqual(feedbackStart, -1);
  assert.notEqual(feedbackEnd, -1);
  assert.ok(feedbackStart < genericBuiltin);
  assert.match(feedbackBlock, /if \(!sid\)/);
  assert.match(feedbackBlock, /if \(hasExtras\)/);
  assert.match(feedbackBlock, /if \(composerSubmitting\) return/);
  assert.match(feedbackBlock, /buildCurrentSessionDesktopFeedbackPayload\(\{/);
  assert.match(feedbackBlock, /sessionId: sid/);
  assert.match(feedbackBlock, /api\.submitDesktopFeedback\(requestPayload\)/);
  // cwd 历史只记编辑器文本(inputHistoryTextForPayload),不记拼上内联粘贴块正文的 display_text。
  assert.match(feedbackBlock, /recordInputHistory\(historyText\)/);
  assert.doesNotMatch(feedbackBlock, /recordInputHistory\(route\.display_text\)/);
  assert.match(feedbackBlock, /clearCurrentSessionDraft\(\)/);
  assert.match(feedbackBlock, /setComposerSubmitting\(true\)/);
  assert.match(feedbackBlock, /setComposerSubmitting\(false\)/);
  assert.doesNotMatch(feedbackBlock, /api\.executeCommand/);
  assert.doesNotMatch(feedbackBlock, /api\.sendInput/);
  assert.doesNotMatch(feedbackBlock, /enqueueInput/);
  assert.doesNotMatch(feedbackBlock, /createHomeComposerSession/);
  assert.doesNotMatch(feedbackBlock, /setTailFollowFromAction/);
  assert.doesNotMatch(feedbackBlock, /applyEvent/);
});

run('feedback remains discovery-only in generic session command parsing', () => {
  const slashCommands = source('lib/slashCommands.js');
  const parserStart = slashCommands.indexOf('export function parseExecutableBuiltinCommand');
  const parser = slashCommands.slice(parserStart);

  // feedback 留在下拉的基础 builtin 表里(可被发现),但不进可执行白名单。
  assert.match(slashCommands, /\{ name: 'feedback', descriptionKey: 'feedback' \}/);
  assert.doesNotMatch(
    parser,
    /parseLeadingCommand\(text,\s*\[[^\]]*['"]feedback['"]/,
  );
});
