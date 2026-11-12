import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  expertAvatarIdentity,
  sessionAvatarIdentity,
  stableLook,
  stableSeed,
  subagentAvatarIdentity,
} from './officeCharacters.js';
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

// 场景:子代理在对话、摘要面板、后台任务页签里前面都画一个头像。
// 期望:绑定了专家的子代理用专家那张;没绑定的按子会话 id 生成,同一个子代理每次都是同一张脸,
// 不同子代理互不相同;没有子会话 id 时不画。
test('sub-agents get a stable generated portrait unless an expert is attached', () => {
  assert.equal(subagentAvatarIdentity('child-one'), 'subagent:child-one');
  assert.equal(subagentAvatarIdentity('child-one'), subagentAvatarIdentity('child-one', null));
  assert.notEqual(subagentAvatarIdentity('child-one'), subagentAvatarIdentity('child-two'));
  assert.deepEqual(
    stableLook(stableSeed(subagentAvatarIdentity('child-one'))),
    stableLook(stableSeed(subagentAvatarIdentity('child-one'))),
  );
  assert.equal(subagentAvatarIdentity('child-one', { avatar_identity: 'expert:reviewer' }), 'expert:reviewer');
  // 专家信息不完整(没有头像身份)时仍按子会话 id 生成。
  assert.equal(subagentAvatarIdentity('child-one', { id: 'reviewer' }), 'subagent:child-one');
  assert.equal(subagentAvatarIdentity(''), '');
  assert.equal(subagentAvatarIdentity(undefined, null), '');
});

// 场景:小人的脸部配饰里原来有一种「小胡子」。
// 期望:不再有人抽到它;配饰表仍是 8 格(那一格留空),原来抽到小胡子的人只是脸上没了胡子,
// 其他人的长相不变。
// 回归:小胡子画在眼睛正下方,脸只有 7 像素宽,胡子和眼睛连成一个「门」字形,
// 用户在后台任务列表里圈出来说难看;直接从表里删掉一格会让所有人的配饰重新洗牌。
test('the mustache is gone and nobody else changes face', () => {
  const { FACEWEAR } = globalThis.ACEOfficeCharacters;
  assert.equal(FACEWEAR.length, 8);
  assert.ok(!FACEWEAR.includes('mustache'));
  for (let index = 0; index < 3000; index += 1) {
    const look = stableLook(stableSeed(`subagent:sample-${index}`));
    assert.ok(!look.acc.includes('mustache'), `sample ${index}`);
  }
  // 改之前这两位是 ['bald', 'mustache'] 和 ['bow', 'mustache']。
  assert.deepEqual(stableLook(stableSeed('subagent:sample-3')).acc, ['bald']);
  assert.deepEqual(stableLook(stableSeed('subagent:sample-11')).acc, ['bow']);
  // 没抽到小胡子的人原样不动(改之前就是这副长相)。
  assert.deepEqual(stableLook(stableSeed('subagent:child-one')), {
    hair: '#b9bcc6', skin: '#94603e', shirt: '#3a3b45', pants: '#3a3f52',
    acc: ['cap', 'tie'], accColor: '#f08a3c',
  });
});

test('custom avatar metadata survives task normalization', () => {
  const expert = { avatar_identity: 'expert:custom', avatar_url: '/api/experts/custom/avatar' };
  assert.deepEqual(normalizeSubagentTask({ id: 'child', expert }).expert, expert);
});
