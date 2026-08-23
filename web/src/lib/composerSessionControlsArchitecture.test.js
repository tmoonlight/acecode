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

function expectInOrder(text, markers) {
  let cursor = -1;
  for (const marker of markers) {
    const index = text.indexOf(marker, cursor + 1);
    assert.notEqual(index, -1, `missing marker: ${marker}`);
    assert.ok(index > cursor, `marker is out of order: ${marker}`);
    cursor = index;
  }
}

run('global, workspace, and active-session composers share integrated session controls', () => {
  const chatView = source('components/ChatView.jsx');
  const inputBar = source('components/InputBar.jsx');
  const sessionControlProps = chatView.match(/sessionControls=\{\{/g) || [];
  const gitPills = chatView.match(/<GitSessionPill/g) || [];

  assert.equal(sessionControlProps.length, 2);
  assert.equal(gitPills.length, 2);
  assert.match(chatView, /const homeTokenBudget = useMemo\(\(\) => normalizeTokenBudget\(\{/);
  assert.match(chatView, /tokenBudget: homeTokenBudget/);
  assert.match(chatView, /tokenBudget,\s+permissionMode,/);
  assert.match(inputBar, /textareaBaseHeight = LINE_HEIGHT \* 2 \+ textareaVerticalPadding/);
  assert.doesNotMatch(chatView, /import \{ StatusBar \}/);
  assert.doesNotMatch(chatView, /<StatusBar/);
});

run('new-conversation hero composer alone uses the 700 by 120 layout', () => {
  const chatView = source('components/ChatView.jsx');
  const inputBar = source('components/InputBar.jsx');
  const styles = source('styles/globals.css');

  assert.match(
    chatView,
    /data-tour-target="home-composer" className="ace-home-composer"/,
  );
  assert.match(
    styles,
    /\.ace-home-content\s*\{[^}]*width: min\(100%, 660px\);/s,
  );
  assert.match(
    styles,
    /\.ace-home-composer\s*\{[^}]*width: 700px;[^}]*max-width: calc\(100% \+ 40px\);/s,
  );
  assert.match(
    styles,
    /\.ace-inputbar-hero-card\s*\{[^}]*min-height: 120px;[^}]*display: flex;[^}]*flex-direction: column;/s,
  );
  assert.match(
    inputBar,
    /isHero \? 'ace-inputbar-hero-card rounded-2xl' : 'rounded-xl'/,
  );
  assert.match(
    inputBar,
    /isHero && 'ace-inputbar-hero-editor'/,
  );
});

run('composer footer preserves required left-to-right control order', () => {
  const component = source('components/ComposerSessionControls.jsx');
  const footer = component.slice(component.indexOf('data-composer-session-controls="true"'));

  expectInOrder(footer, [
    'data-composer-control="add-context"',
    'data-composer-control="swarm-mode"',
    'data-composer-control="expert"',
    'data-composer-control="permission"',
    'data-composer-control="selected-contexts"',
    '<ModelLoadIndicator load={modelLoad} />',
    'data-composer-control="token-budget"',
    'data-composer-control="model"',
    'data-composer-control="submit"',
  ]);
  assert.match(footer, /\{expertName && \(/);
  assert.match(footer, /\{swarmMode && \(/);
  assert.match(footer, /data-composer-control="swarm-mode"[\s\S]*role="status"/);
  assert.match(footer, /<SwarmModeIcon size=\{14\}/);
  assert.match(footer, /aria-label="关闭蜂群模式"/);
  assert.match(footer, /data-composer-control="expert"[\s\S]*role="status"/);
  assert.match(footer, /当前专家组件：\$\{expertName\}/);
  assert.doesNotMatch(footer, /openMenu === 'expert'|onExpertChange|expertLocked/);
});

run('swarm and expert selections survive submission until explicitly changed', () => {
  const chatView = source('components/ChatView.jsx');
  const inputBar = source('components/InputBar.jsx');
  const icon = source('components/SwarmModeIcon.jsx');

  assert.match(inputBar, /role="menuitemcheckbox"/);
  assert.match(inputBar, /aria-checked=\{swarmMode\}/);
  assert.match(inputBar, /<SwarmModeIcon size=\{15\}/);
  assert.match(inputBar, />蜂群模式</);
  assert.match(icon, /HEX_CELLS/);
  assert.match(icon, /<polygon/);
  assert.equal((icon.match(/\[[\d.]+,\s*[\d.]+\]/g) || []).length, 7);
  assert.match(icon, /stroke="currentColor"/);

  assert.match(chatView, /const \[composerSwarmMode, setComposerSwarmMode\] = useState\(false\)/);
  assert.match(chatView, /if \(swarmMode\) payload\.swarm_mode = true/);
  assert.match(chatView, /swarmMode: composerSwarmMode/);
  assert.match(chatView, /const explicitHomeSend = !isBuiltin && \(hasExtras \|\| hasSwarmMode/);
  assert.match(chatView, /preserveExtras: hasExtras \|\| hasSwarmMode/);
  assert.match(
    chatView,
    /const expertOptions = homeExpertId \? \{ expert_id: homeExpertId, expertId: homeExpertId \} : \{\}/,
  );
  assert.match(chatView, /if \(sid\) \{[\s\S]{0,80}setSessionExpertId\(expertId\)/);
  assert.match(inputBar, /onDisableSwarm=\{\(\) => onSwarmModeChange\?\.\(false\)\}/);

  const cleanupStart = chatView.indexOf('const clearComposerExtras = useCallback');
  const resetStart = chatView.indexOf('const resetComposerContextSelections = useCallback');
  const createStart = chatView.indexOf('const createHomeComposerSession = useCallback');
  assert.ok(cleanupStart >= 0 && resetStart > cleanupStart && createStart > resetStart);
  const submissionCleanup = chatView.slice(cleanupStart, resetStart);
  assert.doesNotMatch(
    submissionCleanup,
    /setComposerSwarmMode|setHomeExpertId|setSessionExpertId|onSessionExpertChanged/,
  );
  const contextReset = chatView.slice(resetStart, createStart);
  assert.match(contextReset, /clearComposerExtras\(\)/);
  assert.match(contextReset, /setComposerSwarmMode\(false\)/);
  assert.match(chatView, /resetComposerContextSelections\(\)/);
  assert.doesNotMatch(chatView, /clearComposerExtras\(\{[^)]*preserveSwarm/);

  const submitStart = chatView.indexOf('const submit = useCallback');
  const drainStart = chatView.indexOf('const drainQueuedInput = useCallback');
  assert.ok(submitStart >= 0 && drainStart > submitStart);
  assert.doesNotMatch(
    chatView.slice(submitStart, drainStart),
    /resetComposerContextSelections|setHomeExpertId|setSessionExpertId/,
  );
});

run('selected contexts remain accessible while the composer footer stays on one row', () => {
  const component = source('components/ComposerSessionControls.jsx');
  const styles = source('styles/globals.css');

  assert.match(
    styles,
    /\.ace-composer-session-footer\s*\{[^}]*grid-template-columns: minmax\(0, 1fr\) auto;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-context-strip\s*\{[^}]*flex: 1 1 auto;[^}]*overflow-x: auto;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-session-right\s*\{[^}]*flex-shrink: 0;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-permission-control,[\s\S]*?\.ace-composer-model-control\s*\{[^}]*min-width: 28px;[^}]*flex: 0 1 auto;[^}]*overflow: hidden;/,
  );
  assert.match(
    component,
    /data-composer-control="permission"\s+className="ace-composer-permission-control relative min-w-0"/,
  );
  assert.match(
    component,
    /data-composer-control="model"\s+className="ace-composer-model-control relative min-w-0"/,
  );
  assert.doesNotMatch(styles, /@container \(max-width: 560px\)/);
  assert.match(styles, /\.ace-composer-adaptive-chip\s*\{[^}]*min-width: 28px;[^}]*overflow: hidden;/s);
  assert.doesNotMatch(styles, /@container \(max-width: 410px\)/);
});

run('chat composer docks the git pill below the input without overlapping it', () => {
  const chatView = source('components/ChatView.jsx');
  const pill = source('components/GitSessionPill.jsx');
  const styles = source('styles/globals.css');
  const inputBar = source('components/InputBar.jsx');

  assert.match(chatView, /className="ace-composer-dock"/);
  assert.match(
    chatView,
    /<div className="ace-composer-dock">[\s\S]*<InputBar[\s\S]*<GitSessionPill[\s\S]*<\/div>/,
  );
  assert.match(pill, /ace-git-pill-bar/);
  assert.doesNotMatch(pill, /-mt-1\.5|-mt-\[|margin-top:\s*-/);
  assert.match(
    styles,
    /\.ace-composer-dock\s*\{[^}]*z-index: 30;[^}]*flex-shrink: 0;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-dock \.ace-inputbar-layer\s*\{[^}]*z-index: auto;/s,
  );
  assert.match(inputBar, /'ace-composer-card relative bg-surface transition'/);
  assert.doesNotMatch(inputBar, /focus-within:ring-2|border-\[1\.5px\]/);
});

run('composer permission and model selectors share a 13px label size', () => {
  const styles = source('styles/globals.css');

  assert.match(
    styles,
    /\.ace-composer-control-button\s*\{[^}]*font-size: 13px;/s,
  );
});

run('model selector leaves enough line height for lowercase descenders', () => {
  const styles = source('styles/globals.css');

  assert.match(
    styles,
    /\.ace-composer-model-label\s*\{[^}]*line-height: 1\.35;/s,
  );
});

run('model settings and refresh use independent model-menu header actions', () => {
  const component = source('components/ComposerSessionControls.jsx');
  const styles = source('styles/globals.css');

  expectInOrder(component, [
    'ace-composer-model-menu-header',
    'ace-composer-model-settings',
    '<VsIcon name="settings"',
    '<span>模型设置</span>',
    'onRefreshModels &&',
    'aria-label="刷新模型列表"',
    'ace-composer-model-options',
  ]);
  assert.doesNotMatch(component, /<span>选择模型<\/span>/);
  assert.match(component, /const openModelSettings = \(\) => \{\s+setOpenMenu\(''\);\s+onOpenModelSettings\?\.\(\);\s+\};/s);
  assert.match(
    styles,
    /\.ace-composer-model-settings\s*\{[^}]*align-self: stretch;[^}]*flex: 1 1 auto;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-model-settings\s*\{[^}]*color: var\(--ace-fg-2\);[^}]*transition: color 0\.14s ease;/s,
  );
  assert.match(
    styles,
    /\.ace-composer-model-settings:hover\s*\{[^}]*color: var\(--ace-fg\);[^}]*\}/s,
  );
  assert.doesNotMatch(
    styles,
    /\.ace-composer-model-settings:hover\s*\{[^}]*background:/s,
  );
});

run('both composer variants deep-link model settings through the app callback', () => {
  const app = source('App.jsx');
  const chatView = source('components/ChatView.jsx');
  const callbackProps = chatView.match(/^\s+onOpenModelSettings,$/gm) || [];

  assert.equal(callbackProps.length, 2);
  assert.match(app, /onOpenModelSettings=\{\(\) => openSettingsSection\('models'\)\}/);
});

run('compressed composer controls fall back to one representative SVG icon', () => {
  const component = source('components/ComposerSessionControls.jsx');
  const styles = source('styles/globals.css');

  assert.match(component, /ace-composer-swarm-chip/);
  assert.match(component, /ace-composer-expert-chip/);
  assert.match(component, /<VsIcon name="embedding" size=\{16\} className="ace-composer-model-glyph shrink-0"/);
  assert.match(component, /ace-composer-adaptive-content ace-composer-permission-label/);
  assert.match(component, /ace-composer-adaptive-content ace-composer-model-label/);
  assert.match(component, /function useAdaptiveComposerControls\(rootRef, measureKey\)[\s\S]*?new ResizeObserver\(schedule\)/s);
  assert.match(component, /const compactOrder = \['permission', 'expert', 'swarm-mode', 'model'\]/);
  assert.match(component, /content\.scrollWidth > content\.clientWidth \+ 1/);
  assert.match(component, /root\.setAttribute\('data-ultra-compact', 'true'\)/);
  assert.match(component, /window\.addEventListener\('resize', schedule\)/);
  assert.match(component, /data-adaptive-composer-control="true"\s+data-compact=\{compactControls\.has\('model'\) \? 'true' : 'false'\}\s+data-composer-control="model"/);
  assert.match(styles, /\[data-adaptive-composer-control="true"\]\[data-compact="true"\]\s*\{[^}]*width: 28px !important;[^}]*max-width: 28px !important;/s);
  assert.match(styles, /data-ultra-compact="true"[^}]*data-composer-control="token-budget"[^}]*\{\s*display: none;/s);
  assert.match(styles, /\.ace-composer-swarm-chip\[data-compact="true"\] \.ace-composer-adaptive-content\s*\{[^}]*display: none;/s);
  assert.match(styles, /\.ace-composer-expert-chip\[data-compact="true"\] \.ace-composer-adaptive-content\s*\{[^}]*display: none;/s);
  assert.match(styles, /\.ace-composer-permission-control\[data-compact="true"\] \.ace-composer-adaptive-content\s*\{[^}]*display: none;/s);
  assert.match(styles, /\.ace-composer-model-control\[data-compact="true"\] \.ace-composer-adaptive-content\s*\{[^}]*display: none;/s);
  assert.match(styles, /\.ace-composer-model-control\[data-compact="true"\] \.ace-composer-model-glyph\s*\{[^}]*display: inline-block;/s);
});
