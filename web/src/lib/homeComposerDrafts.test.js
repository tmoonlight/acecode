import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  NO_WORKSPACE_HOME_DRAFT_KEY,
  clearHomeComposerDraftIfMatch,
  homeComposerDraftKey,
  homeComposerDraftText,
  updateHomeComposerDrafts,
} from './homeComposerDrafts.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('home composer drafts are isolated by workspace and no-workspace mode', () => {
  let drafts = updateHomeComposerDrafts({}, 'workspace-a', 'draft A');
  drafts = updateHomeComposerDrafts(drafts, 'workspace-b', 'draft B');
  drafts = updateHomeComposerDrafts(drafts, '', 'local draft');

  assert.equal(homeComposerDraftKey(''), NO_WORKSPACE_HOME_DRAFT_KEY);
  assert.equal(homeComposerDraftText(drafts, 'workspace-a'), 'draft A');
  assert.equal(homeComposerDraftText(drafts, 'workspace-b'), 'draft B');
  assert.equal(homeComposerDraftText(drafts, ''), 'local draft');
  assert.equal(homeComposerDraftText(drafts, 'missing'), '');
});

run('home composer draft updates are immutable and empty text removes only its scope', () => {
  const original = {
    'workspace-a': 'draft A',
    'workspace-b': 'draft B',
  };
  const unchanged = updateHomeComposerDrafts(original, 'workspace-a', 'draft A');
  const cleared = updateHomeComposerDrafts(original, 'workspace-a', '');

  assert.equal(unchanged, original);
  assert.notEqual(cleared, original);
  assert.equal(homeComposerDraftText(original, 'workspace-a'), 'draft A');
  assert.equal(homeComposerDraftText(cleared, 'workspace-a'), '');
  assert.equal(homeComposerDraftText(cleared, 'workspace-b'), 'draft B');
});

run('accepted submission clears only the exact draft that was submitted', () => {
  const submitted = updateHomeComposerDrafts({}, 'workspace-a', 'first draft');
  const cleared = clearHomeComposerDraftIfMatch(submitted, 'workspace-a', 'first draft');
  assert.equal(homeComposerDraftText(cleared, 'workspace-a'), '');

  const editedAgain = updateHomeComposerDrafts(submitted, 'workspace-a', 'new draft');
  const protectedDraft = clearHomeComposerDraftIfMatch(
    editedAgain,
    'workspace-a',
    'first draft',
  );
  assert.equal(protectedDraft, editedAgain);
  assert.equal(homeComposerDraftText(protectedDraft, 'workspace-a'), 'new draft');
});

run('App owns navigation-lifetime drafts and ChatView keeps home/session paths separate', () => {
  const app = readFileSync(new URL('../App.jsx', import.meta.url), 'utf8');
  const chat = readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');

  assert.match(app, /const \[homeComposerDrafts, setHomeComposerDrafts\] = useState\(\{\}\)/);
  assert.match(app, /onHomeComposerDraftChange=\{updateHomeComposerDraft\}/);
  assert.match(app, /onHomeComposerDraftAccepted=\{acceptHomeComposerDraft\}/);
  assert.match(chat, /if \(!sid\) onHomeComposerDraftChange\?\.\(homeDraftWorkspaceHash, next\)/);
  assert.match(chat, /stagedExpertDraft\.present\s+\? stagedExpertDraft\.text\s+: currentHomeDraftText/s);
  assert.match(chat, /onHomeComposerDraftAccepted\?\.\(\s+submittedHomeDraftWorkspaceHash,\s+submittedHomeDraftText/s);
  assert.match(chat, /if \(!targetSid \|\| !targetKey\)[\s\S]*api\.getSessionDraft\(targetSid, targetWorkspaceHash\)/);
});
