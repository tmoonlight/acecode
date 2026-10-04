import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { setTimeout as nextTask } from 'node:timers/promises';
import { parseSync } from '@babel/core';
import { expertDispatchDraftFromRef } from './expertComponents.js';
import { homeRefFromWorkspace } from './homeWorkspaceSelection.js';
import { homeComposerDraft, homeComposerDraftText, updateHomeComposerDrafts } from './homeComposerDrafts.js';
import { composerDraftFingerprint } from './composerDraft.js';
import { aiThemeCreationRef, homeComposerScopedWorkspace } from './aiThemeCreation.js';
import { scheduledTaskCreationRef } from './scheduledTaskCreation.js';
import { commandsWithFallback } from './slashCommands.js';
import {
  COMPOSER_COMMAND_TAG,
  composerDocumentFromText,
  composerDocumentWithSynchronizedLeadingCommand,
  composerTextFromDocument,
} from './richComposerModel.js';

function componentSource(path, name) {
  const source = readFileSync(new URL(path, import.meta.url), 'utf8');
  const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  const component = ast.program.body.map((node) => node.declaration || node)
    .find((node) => node.id?.name === name);
  assert.ok(component, `Missing production component ${name}`);
  const text = (node) => source.slice(node.start, node.end);
  return {
    ast,
    text,
    callback(variable) {
      const node = component.body.body.flatMap((statement) => statement.declarations || [])
        .find((declaration) => declaration.id.name === variable);
      assert.ok(node, `Missing production callback ${variable}`);
      return text(node.init.callee?.name === 'useCallback' ? node.init.arguments[0] : node.init);
    },
    effects: component.body.body.filter((node) => node.expression?.callee?.name === 'useEffect')
      .map((node) => node.expression.arguments),
  };
}

const app = componentSource('../App.jsx', 'App');
const page = componentSource('../components/ExpertComponentsPage.jsx', 'ExpertComponentsPage');
const detail = componentSource('../components/ExpertCatalog.jsx', 'ExpertDetailDialog');
const chat = componentSource('../components/ChatView.jsx', 'ChatView');
// Execute both the real setup callbacks and their dependency arrays. Merely
// calling the setup on every navigation would hide the original regression.
const draftEffects = chat.effects.filter(([setup]) => (
  /api\.getSessionDraft\(targetSid|onInitialDraftConsumed\?\./.test(chat.text(setup))
));
assert.ok(draftEffects.length > 0);
const creationTemplateNode = app.ast.program.body.find((node) => node.id?.name === 'expertManagerCreationDraft');
const creationTemplate = vm.runInNewContext(`(${app.text(creationTemplateNode)})`)();

function fixture(workspaceHash = 'workspace-a') {
  let route = { ...homeRefFromWorkspace({ workspaceHash }), home: false, expertComponents: true };
  let drafts = { 'workspace-a': 'older draft A', 'workspace-b': 'older draft B' };
  let composer = '';
  let pendingRender = false;
  let previousEffects = [];
  let consumed = 0;
  const loads = [];
  const refs = {
    activeRefRef: { current: route },
    pendingForkComposerRef: { current: null },
    preserveComposerInputOnSessionChangeRef: { current: false },
    composerDirtyRef: { current: false },
    composerValueRef: { current: '' },
    draftEditVersionRef: { current: 0 },
    draftLastSavedRef: { current: {} },
    draftSessionKeyRef: { current: '' },
    draftSaveQueueRef: { current: new Map() },
  };
  const navigate = (next) => {
    route = typeof next === 'function' ? next(route) : next;
    refs.activeRefRef.current = route;
    pendingRender = true;
  };
  const changeDraft = (workspace, text) => {
    const next = updateHomeComposerDrafts(drafts, workspace, text);
    if (next !== drafts) pendingRender = true;
    drafts = next;
  };
  const api = new Proxy({
    getSessionDraft: async (sid) => {
      loads.push(sid);
      return { text: 'saved session draft' };
    },
  }, {
    get(target, key) {
      assert.ok(key in target, `Prefill must not call API ${String(key)}`);
      return target[key];
    },
  });
  const scope = {
    ...refs, api, homeRefFromWorkspace, aiThemeCreationRef, scheduledTaskCreationRef, health: {}, homeComposerDraft, composerDraftFingerprint,
    createApi: () => api, refreshWorkspaceGitInfo: async () => {},
    navigateToRef: navigate, replaceActiveRef: navigate,
    rememberRecentExpert() {}, onRememberExpert() {},
    onHomeComposerDraftChange: changeDraft,
    onHomeComposerDraftLoad: undefined,
    restoreChatInputFocusSoon() {}, setDraftReadyKey() {}, setComposerSubmitting() {},
    setComposerValue(text) {
      if (composer !== text) pendingRender = true;
      composer = text;
    },
    restoreComposerDraft(draft) {
      if (composer !== draft.text) pendingRender = true;
      composer = draft.text;
    },
    toast(error) { assert.fail(JSON.stringify(error)); },
    setBusyKey() {}, onClose() {},
    setShowSettings() {},
    expertManagerCreationDraft: () => creationTemplate,
  };
  const evaluate = (source) => vm.runInNewContext(`(${source})`, scope);
  const consume = evaluate(app.callback('consumeInitialDraftText'));
  scope.onInitialDraftConsumed = () => { consumed += 1; consume(); };
  scope.onDispatchToNewTask = evaluate(app.callback('dispatchExpertToNewTask'));
  const dispatch = evaluate(page.callback('dispatchToNewTask'));
  const create = evaluate(app.callback('startConversationalExpertCreation'));
  const createTheme = evaluate(app.callback('startAiThemeCreation'));
  const createScheduled = evaluate(app.callback('startScheduledTaskCreation'));

  function render() {
    let commits = 0;
    do {
      assert.ok(++commits < 12, 'Prefill must settle without a render loop');
      pendingRender = false;
      const sid = route.sessionId || '';
      const workspace = route.workspaceHash || '';
      const homeDraftWorkspace = homeComposerScopedWorkspace(workspace, route.composerDraftScope);
      const draftSessionKey = sid ? `${workspace}:${sid}` : '';
      refs.composerValueRef.current = composer;
      refs.draftSessionKeyRef.current = draftSessionKey;
      const context = {
        ...scope, sid, draftSessionKey, draftWorkspaceHash: workspace,
        homeComposerDrafts: drafts,
        homeDraftWorkspaceHash: sid ? '' : homeDraftWorkspace,
        stagedExpertDraft: expertDispatchDraftFromRef(route),
        currentHomeDraftText: homeComposerDraftText(drafts, homeDraftWorkspace),
      };
      const effects = draftEffects.map(([setup, deps], index) => {
        const effect = vm.runInNewContext(`({ setup: ${chat.text(setup)}, deps: ${chat.text(deps)} })`, context);
        const previous = previousEffects[index];
        effect.changed = !previous || effect.deps.some((dep, i) => !Object.is(dep, previous.deps[i]));
        effect.cleanup = previous?.cleanup;
        return effect;
      });
      effects.forEach((effect) => { if (effect.changed) effect.cleanup?.(); });
      effects.forEach((effect) => { if (effect.changed) effect.cleanup = effect.setup(); });
      previousEffects = effects;
    } while (pendingRender);
  }

  render();
  return {
    get text() { return composer; },
    get route() { return route; },
    get consumed() { return consumed; },
    get drafts() { return drafts; },
    loads, render,
    async dispatch(expert, prompt) { await dispatch(expert, prompt); render(); },
    async dispatchDetail(expert) {
      await vm.runInNewContext(`(${detail.callback('invoke')})`, {
        ...scope, expert, onDispatch: dispatch, onOpeningPrompt: dispatch,
      })('dispatch');
      render();
    },
    create() { create(); render(); },
    createTheme() { createTheme(); render(); },
    createScheduled() { createScheduled(); render(); },
    navigate(next) { navigate(next); render(); },
    edit(text) {
      composer = text;
      refs.draftEditVersionRef.current += 1;
      changeDraft(homeComposerScopedWorkspace(route.workspaceHash || '', route.composerDraftScope), text);
      render();
    },
  };
}

const expert = { id: 'expert-a', quick_prompts: ['first prompt', 'second prompt', 'third prompt'] };
const cases = [
  ['scheduled task callback prefills an isolated editable skill draft without creating or sending', () => {
    const view = fixture();
    view.createScheduled();
    assert.equal(view.text, '/scheduled-task 我希望在明天X点提醒我参加会议，重复X天');
    assert.equal(view.route.composerDraftScope, 'scheduled-task');
    assert.equal(view.consumed, 1);
    view.edit('/scheduled-task 明天九点提醒我开会，重复三天');
    view.render();
    assert.equal(view.text, '/scheduled-task 明天九点提醒我开会，重复三天');
    assert.equal(view.drafts['workspace-a'], 'older draft A');
    view.navigate(homeRefFromWorkspace({ workspaceHash: 'workspace-a' }));
    assert.equal(view.text, 'older draft A');
    assert.deepEqual(view.loads, []);
  }],
  ['AI theme callback closes settings and consumes its isolated draft without sending or replacing ordinary work', () => {
    const view = fixture();
    view.createTheme();
    assert.equal(view.text, '/ai-theme 我想生成关于 XXX 的主题，X 色是它的主色调。');
    assert.equal(view.route.composerDraftScope, 'ai-theme');
    assert.equal(view.route.initialDraftText, undefined);
    assert.equal(view.consumed, 1);
    view.edit('custom theme request');
    view.render();
    assert.equal(view.text, 'custom theme request');
    assert.equal(view.consumed, 1);
    assert.equal(view.drafts['workspace-a'], 'older draft A');
    view.navigate(homeRefFromWorkspace({ workspaceHash: 'workspace-a' }));
    assert.equal(view.text, 'older draft A');
    assert.deepEqual(view.loads, []);
  }],
  ['selected prompts arrive in a reused same-workspace composer and stay editable', async () => {
    const view = fixture();
    await view.dispatch(expert, expert.quick_prompts[1]);
    assert.equal(view.text, 'second prompt');
    assert.equal(view.route.expertId, expert.id);
    assert.equal(view.route.initialDraftText, undefined);
    assert.equal(view.consumed, 1);
    view.edit('edited second prompt');
    view.render();
    assert.equal(view.text, 'edited second prompt');
    assert.equal(view.consumed, 1);
    view.navigate({ ...view.route, expertComponents: true, home: false });
    await view.dispatch(expert, expert.quick_prompts[1]);
    assert.equal(view.text, 'second prompt');
    assert.equal(view.consumed, 2);
    await view.dispatch(expert, expert.quick_prompts[2]);
    assert.equal(view.text, 'third prompt');
    assert.deepEqual(view.loads, []);
  }],
  ['new expert restores the full creation sentence without sending', () => {
    const view = fixture();
    view.create();
    assert.equal(view.text, '/expert-manager 帮我创建一个 XXX 专家，擅长 XXXXX。我的经验是：[请补充你的行业背景、相关经验]。');
    assert.equal(view.route.expertId, undefined);
    assert.equal(view.route.sessionId, undefined);
    view.edit('my creation request');
    view.navigate({ ...view.route, expertComponents: true, home: false });
    view.create();
    assert.equal(view.text, creationTemplate);
    assert.equal(view.consumed, 2);
    assert.deepEqual(view.loads, []);
  }],
  ['the creation handoff forms a Skill tag and keeps its request text unchanged', () => {
    const view = fixture();
    view.create();
    const commands = commandsWithFallback({ skills: [{ name: 'expert-manager', description: 'Manage experts' }] });
    const document = composerDocumentFromText(view.text, commands);
    const tag = document[0].children.find((child) => child.type === COMPOSER_COMMAND_TAG);
    assert.equal(tag?.name, 'expert-manager');
    assert.equal(tag?.kind, 'skill');
    assert.equal(tag?.token, '/expert-manager');
    assert.equal(composerTextFromDocument(document), creationTemplate);

    // The catalog can arrive after the navigation draft and after user edits.
    view.edit(`${view.text} 补充我的背景`);
    const waitingForCatalog = composerDocumentFromText(view.text, []);
    const synchronized = composerDocumentWithSynchronizedLeadingCommand(waitingForCatalog, view.text, commands);
    const lateTag = synchronized[0].children.find((child) => child.type === COMPOSER_COMMAND_TAG);
    assert.equal(lateTag?.name, 'expert-manager');
    assert.equal(lateTag?.kind, 'skill');
    assert.equal(composerTextFromDocument(synchronized), view.text);
    assert.deepEqual(view.loads, []);
  }],
  ['card and detail dispatch default to the first prompt, including teams', async () => {
    const card = fixture();
    await card.dispatch(expert);
    assert.equal(card.text, 'first prompt');
    const dialog = fixture();
    await dialog.dispatchDetail({ ...expert, id: 'team-a', type: 'team' });
    assert.equal(dialog.text, 'first prompt');
    assert.equal(dialog.route.expertId, 'team-a');
  }],
  ['experts without prompts keep the normal home draft', async () => {
    const view = fixture();
    await view.dispatch({ id: 'no-prompts', quick_prompts: [] });
    assert.equal(view.text, 'older draft A');
    assert.equal(view.route.expertId, 'no-prompts');
    assert.equal(view.consumed, 0);
  }],
  ['prefill and edits survive workspace navigation without changing another draft', async () => {
    const view = fixture();
    await view.dispatch(expert, 'workspace A prompt');
    view.edit('workspace A edit');
    view.navigate(homeRefFromWorkspace({ workspaceHash: 'workspace-b' }));
    assert.equal(view.text, 'older draft B');
    view.navigate(homeRefFromWorkspace({ workspaceHash: 'workspace-a' }));
    assert.equal(view.text, 'workspace A edit');
    assert.equal(view.drafts['workspace-b'], 'older draft B');
    view.navigate({ ...homeRefFromWorkspace({ workspaceHash: 'workspace-b' }), initialDraftText: 'workspace B prompt' });
    assert.equal(view.text, 'workspace B prompt');
    assert.equal(view.drafts['workspace-a'], 'workspace A edit');
  }],
  ['no-workspace prefill and explicit empty payloads are consumed', async () => {
    const view = fixture('');
    await view.dispatch(expert, 'local prompt');
    assert.equal(view.text, 'local prompt');
    view.navigate({ ...view.route, initialDraftText: '' });
    assert.equal(view.text, '');
    assert.equal(view.consumed, 2);
  }],
  ['new-task payloads do not replace an existing-session draft', async () => {
    const view = fixture();
    view.navigate({ workspaceHash: 'workspace-a', sessionId: 'existing', initialDraftText: 'new-task only' });
    await nextTask();
    view.render();
    assert.equal(view.text, 'saved session draft');
    assert.equal(view.consumed, 0);
    assert.deepEqual(view.loads, ['existing']);
  }],
];

const failures = [];
for (const [name, run] of cases) {
  try {
    await run();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}: ${error.message}`);
    failures.push(error);
  }
}
if (failures.length) throw new AggregateError(failures, 'Expert prefill regressions');
