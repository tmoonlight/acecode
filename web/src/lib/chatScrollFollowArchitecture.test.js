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

run('tail scheduler guards every delayed bottom write with current follow state', () => {
  const chatView = source('components/ChatView.jsx');
  const start = chatView.indexOf('const scheduleTailFollowScroll');
  const end = chatView.indexOf('const pauseTailFollowForReview', start);
  const scheduler = chatView.slice(start, end);

  assert.match(
    scheduler,
    /if \(activityExpansionAnchorRef\.current\) return false;/,
  );
  assert.match(
    scheduler,
    /if \(!shouldAutoFollowChatTail\(tailFollowStateRef\.current\)\) return false;/,
  );
  assert.match(scheduler, /cancelTailFollowScroll\(\);\s*if \(!scrollToBottom\(\)\) return;/);
  assert.match(
    scheduler,
    /requestAnimationFrame\(\(\) => \{[\s\S]*?if \(!scrollToBottom\(\)\) return;[\s\S]*?requestAnimationFrame/,
  );
});

run('transcript content size preserves an active expansion anchor before tail follow', () => {
  const chatView = source('components/ChatView.jsx');
  assert.match(chatView, /const transcriptContentRef = useRef\(null\)/);
  assert.match(
    chatView,
    /<div ref=\{transcriptContentRef\} className="flex flex-col gap-3">/,
  );
  assert.match(
    chatView,
    /useEffect\(\(\) => observeChatTailContent\(\s*transcriptContentRef\.current,\s*handleTranscriptContentResize,\s*\), \[handleTranscriptContentResize, sid\]\)/,
  );
  const resizeStart = chatView.indexOf('const handleTranscriptContentResize');
  const resizeEnd = chatView.indexOf('useEffect(() => observeChatTailContent', resizeStart);
  const resizeHandler = chatView.slice(resizeStart, resizeEnd);
  assert.match(
    resizeHandler,
    /if \(preserveActivityExpansionAnchor\(\)\) return;\s*scheduleTailFollowScroll\(\);/,
  );

  const effectStart = chatView.indexOf('// 只在用户仍跟随底部时自动滚到底');
  const effectEnd = chatView.indexOf('const handleTranscriptContentResize', effectStart);
  const layoutEffect = chatView.slice(effectStart, effectEnd);
  assert.match(layoutEffect, /activity\?\.detail/);
  assert.match(layoutEffect, /activity\?\.label/);
  assert.match(layoutEffect, /activity\?\.phase/);
  assert.match(layoutEffect, /scheduleTailFollowScroll/);
});

run('activity expansion starts review anchoring and manual scroll gestures cancel it', () => {
  const chatView = source('components/ChatView.jsx');
  const beginStart = chatView.indexOf('const beginActivityExpansionAnchor');
  const beginEnd = chatView.indexOf('const scheduleTailFollowScroll', beginStart);
  const begin = chatView.slice(beginStart, beginEnd);
  const toggleStart = chatView.indexOf('const toggleActivitySummary');
  const toggleEnd = chatView.indexOf('const openReviewPanel', toggleStart);
  const toggle = chatView.slice(toggleStart, toggleEnd);

  assert.match(begin, /cancelTailFollowScroll\(\);/);
  assert.match(begin, /setTailFollowFromAction\(\{ type: 'review_pause' \}\);/);
  assert.match(begin, /viewportTop: activityAnchorViewportTop/);
  assert.match(toggle, /beginActivityExpansionAnchor\(titleElement\)/);
  assert.match(toggle, /scheduleActivityExpansionAnchorMeasure\(\);/);
  assert.match(chatView, /const handleMessagesWheel[\s\S]*?cancelActivityExpansionAnchor\(\);/);
  assert.match(chatView, /const handleMessagesPointerDown[\s\S]*?cancelActivityExpansionAnchor\(\);/);
  assert.match(chatView, /onKeyDownCapture=\{handleMessagesKeyDownCapture\}/);
});
