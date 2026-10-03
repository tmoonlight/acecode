// swarmMode.js 的单元测试(add-mesh-swarm-mode)。
//
// 蜂群模式是会话级状态,输入框芯片 = 服务端值的镜像 + 未提交的本地选择。
// 覆盖:
//  - 模式名归一(旧客户端的 true = 星型,未知值 = 关闭)
//  - 本地选择优先显示,只有与服务端不同才随消息提交
//  - 服务端确认后本地选择清空
//  - 菜单两项互斥,点已选中的那项 = 关闭

import assert from 'node:assert/strict';
import {
  effectiveSwarmMode,
  normalizeSwarmMode,
  pickSwarmMode,
  reconcileSwarmChoice,
  swarmModeForSubmission,
  swarmModeLabel,
  swarmModeTitle,
} from './swarmMode.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:服务端快照、旧会话 meta、旧客户端排队项里可能出现的各种取值。
// 期望:规范名原样、大小写不敏感;布尔 true 是星型;其它一律关闭。
run('蜂群模式名归一:true 兼容星型,未知值按关闭', () => {
  assert.equal(normalizeSwarmMode('mesh'), 'mesh');
  assert.equal(normalizeSwarmMode(' STAR '), 'star');
  assert.equal(normalizeSwarmMode(true), 'star');
  assert.equal(normalizeSwarmMode(false), 'off');
  assert.equal(normalizeSwarmMode(undefined), 'off');
  assert.equal(normalizeSwarmMode('grid'), 'off');
});

// 场景:会话在服务端是网状,用户在菜单里改成关闭但还没发消息。
// 期望:芯片显示本地选择;提交时只带与服务端不同的值;没有本地选择时不提交 ——
// 否则 /swarm 刚切过的模式会被一条普通消息悄悄改回去。
run('只有本地选择与服务端不同时才随消息提交', () => {
  assert.equal(effectiveSwarmMode(null, 'mesh'), 'mesh');
  assert.equal(effectiveSwarmMode('off', 'mesh'), 'off');
  assert.equal(swarmModeForSubmission(null, 'mesh'), null);
  assert.equal(swarmModeForSubmission('off', 'mesh'), 'off');
  assert.equal(swarmModeForSubmission('mesh', 'mesh'), null);
  assert.equal(swarmModeForSubmission('star', undefined), 'star');
});

// 场景:消息带着 swarm_mode 发出后,服务端广播 session_updated。
// 期望:服务端已是本地选择的值时清空本地选择(重新跟随服务端),否则保留。
run('服务端确认后清空本地选择', () => {
  assert.equal(reconcileSwarmChoice('mesh', 'mesh'), null);
  assert.equal(reconcileSwarmChoice('mesh', 'off'), 'mesh');
  assert.equal(reconcileSwarmChoice(null, 'star'), null);
});

// 场景:菜单里「蜂群模式（星型）」「蜂群模式（网状）」两项。
// 期望:两项互斥;点当前已选中的那项等于关闭;文案与提示按模式给出。
run('菜单两项互斥,再点已选中的一项即关闭', () => {
  assert.equal(pickSwarmMode('off', 'mesh'), 'mesh');
  assert.equal(pickSwarmMode('star', 'mesh'), 'mesh');
  assert.equal(pickSwarmMode('mesh', 'mesh'), 'off');
  assert.equal(swarmModeLabel('star'), '蜂群模式（星型）');
  assert.equal(swarmModeLabel('mesh'), '蜂群模式（网状）');
  assert.equal(swarmModeLabel('off'), '');
  assert.notEqual(swarmModeTitle('mesh'), '');
});
