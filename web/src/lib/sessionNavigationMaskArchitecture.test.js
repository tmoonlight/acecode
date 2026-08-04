import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

run('App starts masked for redirected session targets and renders one shell-level mask', () => {
  const app = source('App.jsx');
  const startupState = between(
    app,
    'const startupOpenTargetRef',
    'const [guidedTourState',
  );

  assert.match(startupState, /pendingSessionNavigationIdsRef = useRef\(new Set\(\)\)/);
  assert.match(startupState, /sessionNavigationTimersRef = useRef\(new Map\(\)\)/);
  assert.match(
    startupState,
    /sessionNavigationPending, setSessionNavigationPending\] = useState\(\s*\(\) => !!startupOpenTargetRef\.current/,
  );
  assert.match(
    app,
    /<SessionNavigationMask\s+open=\{sessionNavigationPending\}\s+onCancel=\{cancelSessionNavigation\}\s*\/>/,
  );
});

run('shared session resume flow owns overlap-safe mask cleanup and redirect handoff', () => {
  const app = source('App.jsx');
  const navigation = between(
    app,
    'const finishSessionNavigation',
    'const openSettingsSection',
  );

  assert.match(navigation, /pendingSessionNavigationIdsRef\.current\.add\(navigationId\)/);
  assert.match(navigation, /pendingSessionNavigationIdsRef\.current\.delete\(navigationId\)/);
  assert.match(navigation, /pendingSessionNavigationIdsRef\.current\.size === 0/);
  assert.match(
    navigation,
    /const navigationId = beginSessionNavigation\(\);[\s\S]*let handedOffToPageLoad = false;\s*try \{/,
  );
  assert.match(
    navigation,
    /window\.location\.href = url;\s*handedOffToPageLoad = true;\s*return true;/,
  );
  assert.match(
    navigation,
    /finally \{\s*if \(!handedOffToPageLoad\) finishSessionNavigation\(navigationId\);\s*\}/,
  );
  assert.match(navigation, /toast\(\{ kind: 'err', text: '恢复失败:' \+ \(e\.message \|\| ''\) \}\)/);
  assert.match(
    navigation,
    /const navigationIsPending = \(\) =>\s*pendingSessionNavigationIdsRef\.current\.has\(navigationId\)/,
  );
  const cancellationGuards = navigation.match(
    /if \(!navigationIsPending\(\)\) return false;/g,
  ) || [];
  assert.ok(
    cancellationGuards.length >= 3,
    'late workspace/resume completions must not activate a cancelled target',
  );
});

// 遮罩吞掉全部输入,所以「一定会散」必须是结构性保证,不能依赖调用方自觉:
// (1) 每次 begin 都挂一个兜底定时器;(2) finish 一定清掉它;(3) Esc 能强制散。
run('session navigation mask always has a way out', () => {
  const app = source('App.jsx');
  const navigation = between(
    app,
    'const finishSessionNavigation',
    'const resumeAndOpenSession',
  );

  // begin 必须挂兜底定时器,并把 handle 存进 ref 以便 finish 清理。
  assert.match(navigation, /setTimeout\(\s*\(\) => \{[\s\S]*finishSessionNavigation\(navigationId\);\s*\},\s*SESSION_NAVIGATION_MASK_TIMEOUT_MS\)/);
  assert.match(navigation, /sessionNavigationTimersRef\.current\.set\(navigationId, timer\)/);
  // finish 必须清定时器,否则遮罩散了之后还会弹一条假的超时 toast。
  assert.match(navigation, /clearTimeout\(timer\)/);
  assert.match(navigation, /sessionNavigationTimersRef\.current\.delete\(navigationId\)/);
  // Esc 取消:清掉全部在途导航,而不只是最后一个。
  assert.match(navigation, /const cancelSessionNavigation = useCallback/);
  assert.match(navigation, /Array\.from\(pendingSessionNavigationIdsRef\.current\)/);

  // 兜底必须晚于请求超时:正常路径应当是请求先超时、上层收尾。
  const maskTimeout = Number(/SESSION_NAVIGATION_MASK_TIMEOUT_MS = (\d+)/.exec(app)?.[1]);
  const requestTimeout = Number(
    /DEFAULT_REQUEST_TIMEOUT_MS = (\d+)/.exec(source('lib/api.js'))?.[1],
  );
  assert.ok(Number.isFinite(maskTimeout), 'mask timeout constant not found');
  assert.ok(Number.isFinite(requestTimeout), 'request timeout constant not found');
  assert.ok(
    maskTimeout > requestTimeout,
    `mask fallback (${maskTimeout}ms) must outlast the request timeout (${requestTimeout}ms)`,
  );
});

run('session navigation mask blocks the viewport and exposes accessible progress', () => {
  const mask = source('components/SessionNavigationMask.jsx');

  assert.match(mask, /if \(!open\) return null/);
  assert.match(mask, /fixed inset-0 z-\[11000\]/);
  assert.match(mask, /data-session-navigation-mask="true"/);
  assert.match(mask, /role="status"/);
  assert.match(mask, /aria-live="polite"/);
  assert.match(mask, /aria-busy="true"/);
  assert.match(mask, /t\('sessionNavigation\.opening'\)/);
  assert.match(mask, /aria-label=\{label\}/);
  assert.match(mask, /className="ace-spinner text-\[28px\]"/);
  assert.match(mask, />\{label\}<\/span>/);
  assert.match(mask, /tabIndex=\{0\}[\s\S]*autoFocus/);
  // Esc 走 onKeyDown 分流到 onCancel,其余按键仍然被吞掉。
  assert.match(mask, /onKeyDown=\{onKeyDown\}/);
  assert.match(mask, /event\.key === 'Escape'/);
  assert.match(mask, /onCancel\(\)/);
  assert.match(mask, /onPointerDown=\{stopInteraction\}/);
  // 遮罩本身仍然不持有任何计时器 —— 生命周期归 App 管。
  assert.doesNotMatch(mask, /setTimeout|requestAnimationFrame/);
});
