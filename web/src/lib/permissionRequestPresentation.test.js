import assert from 'node:assert/strict';
import { planPermissionPresentation } from './permissionRequestPresentation.js';

// 执行审批显示真实边界/理由,解释器或不透明脚本不提供可误解的会话放行。
const escalation = planPermissionPresentation({
  tool: 'bash',
  args: {
    command: 'pnpm install',
    justification: 'Install the project dependencies.',
    permission: { reason: 'escalation_requested', sandbox: 'full-access', always_allow_prefix: 'pnpm install' },
  },
});
assert.equal(escalation.title, '模型申请在沙盒外执行');
assert.match(escalation.body, /沙盒外/);
assert.equal(escalation.justification, 'Install the project dependencies.');
assert.equal(escalation.allowSessionLabel, '本次会话允许: pnpm install');
assert.equal(escalation.hideAllowSession, false);
const opaque = planPermissionPresentation({
  tool: 'bash', args: { permission: { reason: 'dangerous_command', sandbox: 'workspace-write', always_allow_prefix: '' } },
});
assert.equal(opaque.title, '模型要执行一条危险命令');
assert.equal(opaque.hideAllowSession, true);
assert.match(opaque.body, /沙盒内/);
assert.match(planPermissionPresentation({
  tool: 'bash', args: { permission: { reason: 'unknown_command_without_sandbox', sandbox: 'full-access' } },
}).body, /没有可用沙盒/);

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('plan permission presentation recognizes EnterPlanMode requests', () => {
  const view = planPermissionPresentation({
    tool: 'EnterPlanMode',
    args: { kind: 'enter_plan_mode', plan_file_path: 'C:/tmp/plan.md' },
  });
  assert.equal(view.isPlanEnter, true);
  assert.equal(view.isPlanApproval, false);
  assert.equal(view.hideAllowSession, true);
  assert.equal(view.title, '进入 Plan 模式');
  assert.equal(view.primaryLabel, '进入 Plan');
  assert.equal(view.planFilePath, 'C:/tmp/plan.md');
});

run('plan permission presentation recognizes plan approval payloads', () => {
  const view = planPermissionPresentation({
    tool: 'ExitPlanMode',
    args: {
      kind: 'plan_approval',
      plan_file_path: 'C:/tmp/plan.md',
      plan: '1. Inspect\n2. Implement',
    },
  });
  assert.equal(view.isPlanEnter, false);
  assert.equal(view.isPlanApproval, true);
  assert.equal(view.hideAllowSession, true);
  assert.equal(view.title, '计划审批');
  assert.equal(view.primaryLabel, '批准计划');
  assert.equal(view.planFilePath, 'C:/tmp/plan.md');
  assert.equal(view.planText, '1. Inspect\n2. Implement');
});

run('plan permission presentation leaves generic tool requests unchanged', () => {
  const view = planPermissionPresentation({
    tool: 'bash',
    args: { command: 'git status' },
  });
  assert.equal(view.isPlanEnter, false);
  assert.equal(view.isPlanApproval, false);
  assert.equal(view.hideAllowSession, false);
  assert.equal(view.title, '权限请求');
  assert.equal(view.primaryLabel, '允许一次');
});

// 场景:模型申请 with_additional_permissions(只加一个目录的写权限)。
// 期望:标题说明是「临时加宽」、正文强调仍在沙盒内、额外权限逐条列出、
// 「本次会话允许」按钮记的是权限而不是命令前缀、不提供「以后都允许」。
run('additional permission requests list the extra grants and stay sandboxed', () => {
  const view = planPermissionPresentation({
    tool: 'bash',
    args: {
      command: 'pnpm install',
      justification: 'Needs the shared cache.',
      permission: {
        reason: 'additional_permissions_requested',
        request: 'with_additional_permissions',
        sandbox: 'workspace-write',
        always_allow_prefix: 'pnpm install',
        proposed_prefix_rule: 'pnpm install',
        additional_permissions: { write: ['/home/u/.cache/pnpm'], read: [], network: true },
      },
    },
  });
  assert.equal(view.title, '模型申请临时加宽沙盒权限');
  assert.match(view.body, /仍在沙盒内/);
  assert.deepEqual(view.additionalPermissions, [
    { kind: 'write', path: '/home/u/.cache/pnpm' },
    { kind: 'network', path: '' },
  ]);
  assert.equal(view.hideAllowSession, false);
  assert.equal(view.allowSessionLabel, '本次会话保留这些权限');
  assert.equal(view.rememberPrefix, '');
});

// 场景:越权申请之前刚有一次沙盒拒绝,后端给出了被拒路径所在目录与可记住前缀。
// 期望:多出「只放行写入 <目录>」与「以后都允许: <前缀>」两个选项的文案,
// 并把被拒路径透出给用户看。
run('escalation requests expose scoped-grant and remember options', () => {
  const view = planPermissionPresentation({
    tool: 'bash',
    args: {
      command: 'pnpm install',
      permission: {
        reason: 'escalation_requested',
        request: 'require_escalated',
        sandbox: 'full-access',
        always_allow_prefix: 'pnpm install',
        proposed_prefix_rule: 'pnpm install',
        scoped_write_root: '/home/u/.cache/pnpm',
        denied_path: '/home/u/.cache/pnpm/store.lock',
      },
    },
  });
  assert.equal(view.scopedWriteRoot, '/home/u/.cache/pnpm');
  assert.equal(view.scopedLabel, '只放行写入 /home/u/.cache/pnpm');
  assert.equal(view.rememberLabel, '以后都允许: pnpm install');
  assert.equal(view.deniedPath, '/home/u/.cache/pnpm/store.lock');
});

// 场景:不透明脚本(没有可记前缀、没有被拒路径)的越权申请。期望:两个新
// 选项都不出现,回到旧的「允许一次 / 拒绝」形态。
run('opaque commands offer neither scoped nor remember options', () => {
  const view = planPermissionPresentation({
    tool: 'bash',
    args: { permission: { reason: 'escalation_requested', request: 'require_escalated', sandbox: 'full-access', always_allow_prefix: '' } },
  });
  assert.equal(view.scopedWriteRoot, '');
  assert.equal(view.rememberPrefix, '');
  assert.equal(view.hideAllowSession, true);
});

// 场景:自动模式（试验）因拦截次数达到上限而退回弹窗,daemon 把裁决的理由放在
// permission.auto_review 里。期望:确认框多出一句说明并带上理由;裁决没有结果时
// 是另一句固定说明;普通确认没有这句话。非 bash 工具同样适用。
run('auto review fallback explains why the user is being asked', () => {
  const limit = planPermissionPresentation({
    tool: 'bash',
    args: { command: 'git push --force',
      permission: { reason: 'dangerous_command', sandbox: 'full-access',
        auto_review: { why: 'denial_limit', reason: 'B2: force-push rewrites history' } } },
  });
  assert.equal(limit.reviewNote, '自动检查拦下了这个操作:B2: force-push rewrites history');
  assert.equal(limit.title, '模型要执行一条危险命令');

  const silent = planPermissionPresentation({
    tool: 'bash',
    args: { permission: { reason: 'dangerous_command', auto_review: { why: 'denial_limit', reason: '  ' } } },
  });
  assert.equal(silent.reviewNote, '自动检查拦下了这个操作');

  const unavailable = planPermissionPresentation({
    tool: 'mcp__docs__publish',
    args: { title: 'x', permission: { auto_review: { why: 'no_verdict', reason: 'HTTP 429' } } },
  });
  assert.equal(unavailable.reviewNote, '自动检查没有结果,需要你来确认');
  assert.equal(unavailable.title, '权限请求');

  assert.equal(planPermissionPresentation({ tool: 'bash', args: { permission: { reason: 'dangerous_command' } } }).reviewNote, '');
  assert.equal(planPermissionPresentation({ tool: 'file_write', args: { path: 'a.txt' } }).reviewNote, '');
});
