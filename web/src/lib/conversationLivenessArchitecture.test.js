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

run('composer 附近不再存在重复的固定活动栏', () => {
  const chat = source('components/ChatView.jsx');
  const input = source('components/InputBar.jsx');
  assert.doesNotMatch(chat, /ConversationActivityRail|connectedTop/);
  assert.equal(
    fs.existsSync(path.join(srcRoot, 'components/ConversationActivityRail.jsx')),
    false,
  );
  assert.match(input, /'border-t border-border px-2\.5 py-2 bg-surface shrink-0'/);
});

run('前台状态投影进统一活动行，后台状态仍在 transcript 尾部呈现', () => {
  const chat = source('components/ChatView.jsx');
  assert.match(chat, /selectConversationActivity\(\{/);
  assert.match(chat, /ensureLiveActivity: busy/);
  assert.match(chat, /activity=\{it\.live \? conversationActivity : null\}/);
  assert.match(chat, /conversationActivity\.kind === CONVERSATION_ACTIVITY_KIND\.BACKGROUND[\s\S]*?<ActivityLine/);
  assert.doesNotMatch(chat, /data-conversation-activity-bubble|<ActivityIndicator/);
});

run('活动行状态不再由旧的文本或工具可见性条件漏掉', () => {
  const chat = source('components/ChatView.jsx');
  const start = chat.indexOf('() => projectCollapsedTranscriptItems');
  const end = chat.indexOf('// 尾部窗口', start);
  const mount = chat.slice(start, end);
  assert.ok(start >= 0 && end > start);
  assert.doesNotMatch(
    mount,
    /hasVisibleStreamingAssistant|hasActiveTool|permissionRequests\.length/,
  );
});

run('ActivityLine 保持流内固定单行且 selector 包含恢复和后台状态', () => {
  const chat = source('components/ChatView.jsx');
  const line = source('components/ActivityLine.jsx');
  const selector = source('lib/conversationActivity.js');
  assert.match(line, /flex h-7 w-full/);
  assert.doesNotMatch(line, /rounded-2xl|shadow-sm|ace-pulse/);
  assert.doesNotMatch(line, /\bfixed\b|\bsticky\b|\babsolute\b|bottom-|inset-/);
  assert.doesNotMatch(line, /#[0-9a-fA-F]{3,8}/);
  assert.doesNotMatch(chat, /function ActivityIndicator/);
  assert.match(selector, /正在恢复权限请求/);
  assert.match(selector, /正在恢复提问请求/);
  assert.match(selector, /主会话仍可继续输入/);
});

console.log('conversationLivenessArchitecture tests passed');
