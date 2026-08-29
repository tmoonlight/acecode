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

run('底部 live 主状态用独立文字副本每两秒扫光且详情保持静态', () => {
  const line = source('components/ActivityLine.jsx');
  const styles = source('styles/globals.css');

  assert.match(line, /const activityLabel = label \|\| '正在处理';/);
  assert.match(line, /const liveCopyClassName = live \? 'ace-activity-line-live-copy' : '';/);
  assert.equal((line.match(/liveCopyClassName/g) || []).length, 2);
  assert.match(line, /\{activityLabel\}[\s\S]*?\{live && \([\s\S]*?className="ace-activity-line-shimmer-sweep" aria-hidden="true"[\s\S]*?className="ace-activity-line-shimmer-highlight">\{activityLabel\}/);
  assert.match(line, /\{detail && \([\s\S]*?className="min-w-0 truncate text-fg-mute"/);
  assert.match(styles, /@keyframes ace-activity-line-shimmer-sweep\s*\{[\s\S]*?0%,\s*50%\s*\{\s*transform:\s*translateX\(-50%\);[\s\S]*?100%\s*\{\s*transform:\s*translateX\(125%\);/);
  assert.match(styles, /@keyframes ace-activity-line-shimmer-highlight\s*\{[\s\S]*?0%,\s*50%\s*\{\s*transform:\s*translateX\(50%\);[\s\S]*?100%\s*\{\s*transform:\s*translateX\(-125%\);/);
  assert.match(styles, /\.ace-activity-line-live-copy\s*\{[\s\S]*?position:\s*relative;[\s\S]*?-webkit-text-fill-color:\s*currentColor;/);
  assert.match(styles, /\.ace-activity-line-shimmer-sweep\s*\{[\s\S]*?mask-image:\s*linear-gradient\(90deg, transparent 0%, #000 20% 30%, transparent 50% 100%\);[\s\S]*?animation:\s*ace-activity-line-shimmer-sweep 2s steps\(48, end\) infinite;/);
  assert.match(styles, /\.ace-activity-line-shimmer-highlight\s*\{[\s\S]*?color:\s*rgba\(255, 255, 255, 0\.92\);[\s\S]*?animation:\s*ace-activity-line-shimmer-highlight 2s steps\(48, end\) infinite;/);
  const reducedMotionStart = styles.indexOf('@media (prefers-reduced-motion: reduce)', styles.indexOf('@keyframes ace-activity-line-shimmer-sweep'));
  const reducedMotionEnd = styles.indexOf('/* Tool 中的真实代码与 diff', reducedMotionStart);
  assert.ok(reducedMotionStart >= 0 && reducedMotionEnd > reducedMotionStart);
  assert.doesNotMatch(styles.slice(reducedMotionStart, reducedMotionEnd), /ace-activity-line-live-copy|ace-activity-line-shimmer-(?:sweep|highlight)/);
});

console.log('conversationLivenessArchitecture tests passed');
