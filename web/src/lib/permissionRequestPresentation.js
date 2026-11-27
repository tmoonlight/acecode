function listOf(value) {
  return Array.isArray(value) ? value.filter((item) => typeof item === 'string' && item.length > 0) : [];
}

export function planPermissionPresentation(request) {
  const requestKind = request?.args && typeof request.args === 'object' ? request.args.kind : '';
  const isPlanEnter = request?.tool === 'EnterPlanMode' || requestKind === 'enter_plan_mode';
  const isPlanApproval = request?.tool === 'ExitPlanMode' || requestKind === 'plan_approval';
  const planText = isPlanApproval && typeof request?.args?.plan === 'string'
    ? request.args.plan
    : '';
  const planFilePath = (isPlanApproval || isPlanEnter) && typeof request?.args?.plan_file_path === 'string'
    ? request.args.plan_file_path
    : '';

  const shellPermission = request?.tool === 'bash' && request?.args?.permission &&
    typeof request.args.permission === 'object' ? request.args.permission : null;
  const reason = shellPermission?.reason;
  const shellRequest = typeof shellPermission?.request === 'string' ? shellPermission.request : 'use_default';
  const isAdditional = shellRequest === 'with_additional_permissions';
  const shellTitle = {
    dangerous_command: '模型要执行一条危险命令',
    escalation_requested: '模型申请在沙盒外执行',
    additional_permissions_requested: '模型申请临时加宽沙盒权限',
    unknown_command_without_sandbox: '命令需要确认',
    rule_prompt: '执行规则要求确认这条命令',
  }[reason];
  const shellBody = reason === 'unknown_command_without_sandbox'
    ? '本平台没有可用沙盒,未知命令需要确认'
    : isAdditional
      ? '允许后,这条命令仍在沙盒内执行,只额外放行下面列出的权限。'
      : shellPermission?.sandbox === 'full-access'
        ? '允许后,这条命令将在沙盒外执行。'
        : '允许后,这条命令将在沙盒内执行。';
  const prefix = typeof shellPermission?.always_allow_prefix === 'string'
    ? shellPermission.always_allow_prefix.trim() : '';
  // 额外权限申请:「本次会话允许」记的是这些权限而不是命令前缀。
  const additional = shellPermission?.additional_permissions && typeof shellPermission.additional_permissions === 'object'
    ? shellPermission.additional_permissions : null;
  const additionalPermissions = additional
    ? [
      ...listOf(additional.write).map((path) => ({ kind: 'write', path })),
      ...listOf(additional.read).map((path) => ({ kind: 'read', path })),
      ...(additional.network ? [{ kind: 'network', path: '' }] : []),
    ]
    : [];
  const scopedWriteRoot = typeof shellPermission?.scoped_write_root === 'string'
    ? shellPermission.scoped_write_root.trim() : '';
  const rememberPrefix = !isAdditional && typeof shellPermission?.proposed_prefix_rule === 'string'
    ? shellPermission.proposed_prefix_rule.trim() : '';
  const deniedPath = typeof shellPermission?.denied_path === 'string'
    ? shellPermission.denied_path.trim() : '';
  const allowSessionLabel = isAdditional
    ? '本次会话保留这些权限'
    : prefix ? `本次会话允许: ${prefix}` : '本次会话允许';
  // 自动模式（试验）退回弹窗时,daemon 在 permission.auto_review 里说明原因:
  // denial_limit = 裁决连续拦截达到上限,这一次改为问人(reason 是裁决的理由);
  // no_verdict = 裁决没有结果。任何工具的确认都可能带,不限 bash。
  const anyPermission = request?.args?.permission && typeof request.args.permission === 'object'
    ? request.args.permission : null;
  const review = anyPermission?.auto_review && typeof anyPermission.auto_review === 'object'
    ? anyPermission.auto_review : null;
  const reviewReason = typeof review?.reason === 'string' ? review.reason.trim() : '';
  const reviewNote = !review ? ''
    : review.why === 'denial_limit'
      ? (reviewReason ? `自动检查拦下了这个操作:${reviewReason}` : '自动检查拦下了这个操作')
      : '自动检查没有结果,需要你来确认';
  return {
    reviewNote,
    isPlanEnter,
    isPlanApproval,
    planText,
    planFilePath,
    // 计划类请求与没有可记前缀的 bash 请求不提供「本次会话允许」;额外权限申请
    // 例外(它记的是权限而不是前缀)。
    hideAllowSession: isPlanEnter || isPlanApproval || (!!shellPermission && !isAdditional && !prefix),
    allowSessionLabel,
    justification: shellPermission && typeof request?.args?.justification === 'string'
      ? request.args.justification : '',
    title: isPlanApproval ? '计划审批' : isPlanEnter ? '进入 Plan 模式' : shellTitle || '权限请求',
    body: isPlanApproval
      ? 'Agent 已完成计划,请求批准后退出 Plan 模式。'
      : isPlanEnter
        ? 'Agent 请求进入 Plan 模式,先探索并写计划,批准后再改代码。'
        : shellPermission ? shellBody : 'Agent 请求执行以下操作:',
    primaryLabel: isPlanApproval ? '批准计划' : isPlanEnter ? '进入 Plan' : '允许一次',
    additionalPermissions,
    deniedPath,
    scopedWriteRoot,
    scopedLabel: scopedWriteRoot ? `只放行写入 ${scopedWriteRoot}` : '',
    rememberPrefix,
    rememberLabel: rememberPrefix ? `以后都允许: ${rememberPrefix}` : '',
  };
}
