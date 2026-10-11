import assert from 'node:assert/strict';
import { test } from 'node:test';
import { expertAvatarIdentity, sessionAvatarIdentity, stableLook, stableSeed } from './officeCharacters.js';
import { officeActor } from './desktopOfficeState.js';
import { normalizeSubagentTask } from './subagentTasks.js';

test('team member portrait stays the same across sessions and office actors', () => {
  const expert = { id: 'opc-value-designer', avatar_identity: 'expert:opc-value-designer' };
  const identity = expertAvatarIdentity(expert);
  const source = { id: 'child-one', expert_id: 'opc-team', expert_member_id: expert.id,
    expert: { id: 'opc-team', member_id: expert.id, avatar_identity: identity }, busy: true };
  const first = officeActor(source, 'root');
  const second = officeActor({ ...source, id: 'child-two' }, 'root');
  assert.equal(first.avatarIdentity, identity);
  assert.equal(first.seed, stableSeed(identity));
  assert.deepEqual(stableLook(first.seed), stableLook(second.seed));
  assert.equal(sessionAvatarIdentity(normalizeSubagentTask(source)), identity);
});

test('team leader, internal members and generic sessions retain distinct identities', () => {
  assert.equal(expertAvatarIdentity({ id: 'team', references_existing_experts: true,
    lead_expert_id: 'leader' }), 'expert:leader');
  assert.equal(expertAvatarIdentity({ id: 'team', type: 'team', lead_agent_id: 'leader' }), 'expert:team/leader');
  const root = officeActor({ id: 'root', expert: { avatar_identity: 'expert:leader' } }, 'root');
  assert.equal(root.seed, stableSeed('expert:leader'));
  assert.equal(root.avatarIdentity, 'expert:leader');
  assert.equal(officeActor({ id: 'root' }, 'root').avatarIdentity, '');
});

test('custom avatar metadata survives task normalization', () => {
  const expert = { avatar_identity: 'expert:custom', avatar_url: '/api/experts/custom/avatar' };
  assert.deepEqual(normalizeSubagentTask({ id: 'child', expert }).expert, expert);
});
