// 会话级蜂群模式(add-mesh-swarm-mode):off | star | mesh。
//
// 模式存在服务端会话上(SessionMeta.swarm_mode),输入框只是它的镜像:
//   - 服务端值来自 messages 快照与 session_updated 事件(transcript store 的 swarmMode);
//   - 用户在菜单里改选时先记成「本地选择」,随下一条消息写给服务端,服务端确认
//     (session_updated)后本地选择清空,重新跟随服务端。
// 只有本地选择与服务端不同才随消息提交 —— 否则 /swarm 命令或别的标签页刚切过的
// 模式会被一条普通消息悄悄改回去,网状树里还有子 agent 在跑时还会直接 409。

export const SWARM_MODES = Object.freeze(['off', 'star', 'mesh']);

// 旧客户端 / 旧会话里的布尔 true 等价星型;其它无法识别的值一律按关闭处理。
export function normalizeSwarmMode(value) {
  if (value === true) return 'star';
  if (typeof value !== 'string') return 'off';
  const mode = value.trim().toLowerCase();
  return SWARM_MODES.includes(mode) ? mode : 'off';
}

// 输入框显示的模式:未提交的本地选择优先,否则跟随服务端。
export function effectiveSwarmMode(localChoice, serverMode) {
  return localChoice == null ? normalizeSwarmMode(serverMode) : normalizeSwarmMode(localChoice);
}

// 随消息提交的 swarm_mode;null = 不提交(与服务端一致或没有本地选择)。
export function swarmModeForSubmission(localChoice, serverMode) {
  if (localChoice == null) return null;
  const local = normalizeSwarmMode(localChoice);
  return local === normalizeSwarmMode(serverMode) ? null : local;
}

// 服务端已经是本地选择的值 → 清空本地选择,重新跟随服务端。
export function reconcileSwarmChoice(localChoice, serverMode) {
  if (localChoice == null) return null;
  return normalizeSwarmMode(localChoice) === normalizeSwarmMode(serverMode) ? null : localChoice;
}

// 菜单里点一个模式:点已选中的那个 = 关闭,否则切到它(两种蜂群互斥)。
export function pickSwarmMode(current, picked) {
  const target = normalizeSwarmMode(picked);
  return normalizeSwarmMode(current) === target ? 'off' : target;
}

const LABELS = {
  star: '蜂群模式（星型）',
  mesh: '蜂群模式（网状）',
};

const TITLES = {
  star: '主 Agent 积极派遣子 Agent 并汇总结果',
  mesh: 'Agent 之间可以互相派任务、发消息，组成协作网络',
};

export function swarmModeLabel(mode) {
  return LABELS[normalizeSwarmMode(mode)] || '';
}

export function swarmModeTitle(mode) {
  return TITLES[normalizeSwarmMode(mode)] || '';
}
