import assert from 'node:assert/strict';
import {
  AUDIT_TIME_OPTIONS,
  EXEC_RULE_DECISIONS,
  MANAGED_RULES_FILE,
  MANAGED_SANDBOXED_RULES_FILE,
  appendEntryLine,
  auditExportFilename,
  auditQueryString,
  auditRowPresentation,
  auditSummaryCounts,
  blockedPathToWritableRoot,
  execRuleRows,
  execRulesHaveChanges,
  execRulesPayload,
  mergeAuditEntries,
  otherRuleFiles,
  parseEntryLines,
  parsePatternText,
  patternDisplay,
  platformNotes,
  sandboxDraft,
  sandboxDraftHasChanges,
  sandboxPayload,
  securityErrorCode,
  validateExecRuleRows,
  validateSandboxDraft,
  validateSandboxEntry,
} from './securityCenter.js';

// 场景:textarea 一行一条 → 清单。期望:去空白 / 空行 / 重复,保留顺序。
{
  assert.deepEqual(parseEntryLines(' ~/.ssh \n\n~/.ssh\r\n**/.env\n'), ['~/.ssh', '**/.env']);
  assert.deepEqual(parseEntryLines(''), []);
  assert.deepEqual(parseEntryLines(undefined), []);
}

// 场景:条目校验与后端同一套规则。期望:记号 / 绝对路径通过,相对路径 / 未知记号 /
// ~user / 非黑名单里的通配被拒。
{
  assert.equal(validateSandboxEntry('~', false), '');
  assert.equal(validateSandboxEntry('~/.aws', true), '');
  assert.equal(validateSandboxEntry(':workspace_roots/build', false), '');
  assert.equal(validateSandboxEntry(':acecode_home\\config.json', true), '');
  assert.equal(validateSandboxEntry('C:\\data\\out', false), '');
  assert.equal(validateSandboxEntry('/var/tmp', false), '');
  assert.equal(validateSandboxEntry('**/.env', true), '');
  assert.notEqual(validateSandboxEntry('**/.env', false), '');
  assert.notEqual(validateSandboxEntry('src', false), '');
  assert.notEqual(validateSandboxEntry('~bob/x', false), '');
  assert.notEqual(validateSandboxEntry(':home', false), '');
  assert.notEqual(validateSandboxEntry('', false), '');
}

// 场景:快照 → 草稿 → payload 往返;只有 deny 的错误被报出来;改动检测。
{
  const snapshot = {
    enabled: true, network_access: false, deny_defaults: true,
    filesystem: { read: [], write: ['D:/out'], deny: ['~/.ssh'] },
  };
  const draft = sandboxDraft(snapshot);
  assert.equal(draft.write, 'D:/out');
  assert.equal(draft.deny, '~/.ssh');
  assert.equal(sandboxDraftHasChanges(draft, snapshot), false);
  assert.deepEqual(sandboxPayload(draft), {
    enabled: true, network_access: false, deny_defaults: true,
    filesystem: { read: [], write: ['D:/out'], deny: ['~/.ssh'] },
  });
  const edited = { ...draft, deny: '~/.ssh\nsrc/secret', read: 'relative' };
  const errors = validateSandboxDraft(edited);
  assert.match(errors.deny, /绝对路径/u);
  assert.match(errors.read, /绝对路径/u);
  assert.equal(errors.write, undefined);
  assert.equal(sandboxDraftHasChanges({ ...draft, networkAccess: true }, snapshot), true);
  assert.equal(appendEntryLine('~/.ssh', 'D:\\data'), '~/.ssh\nD:\\data');
  assert.equal(appendEntryLine('~/.ssh', '~/.ssh'), '~/.ssh');
  assert.equal(appendEntryLine('', '  '), '');
}

// 场景:被拦路径 → 建议可写目录。期望:文件取上一级,目录取自身,盘符根保留。
{
  assert.equal(blockedPathToWritableRoot('D:\\data\\out\\x.txt'), 'D:\\data\\out');
  assert.equal(blockedPathToWritableRoot('/home/u/proj/.env'), '/home/u/proj');
  assert.equal(blockedPathToWritableRoot('D:/data/out'), 'D:/data/out');
  assert.equal(blockedPathToWritableRoot('D:/data/out/'), 'D:/data/out');
  assert.equal(blockedPathToWritableRoot(''), '');
}

// 场景:平台说明。期望:Windows 受限令牌给出「只拦写」与「准断网」两条;seatbelt
// 真隔离时没有提示;不可用时报原因。
{
  const windows = platformNotes({ available: true, read_isolation: false, network_enforced: false, network_best_effort: true });
  assert.equal(windows.length, 2);
  assert.match(windows[0].text, /只能拦截写入/u);
  assert.match(windows[1].text, /不能真正断网/u);
  assert.deepEqual(platformNotes({ available: true, read_isolation: true, network_enforced: true }), []);
  assert.match(platformNotes({ available: false, reason: 'bwrap missing' })[0].text, /bwrap missing/u);
}

// 场景:命令前缀文本 ↔ pattern。期望:空白分词、`|` 为候选并集,展示往返一致。
{
  assert.deepEqual(parsePatternText('  git   status|diff --short '), [['git'], ['status', 'diff'], ['--short']]);
  assert.deepEqual(parsePatternText(''), []);
  assert.equal(patternDisplay([['git'], ['status', 'diff']]), 'git status|diff');
  assert.equal(patternDisplay(['git', ['a', 'b']]), 'git a|b');
}

// 场景:托管文件快照 → 行;非托管文件只读;payload 按决策分文件;删光也写空数组。
{
  const snapshot = {
    files: [
      { name: MANAGED_RULES_FILE, managed: true, rules: [
        { pattern: ['pnpm', 'test'], display: 'pnpm test', decision: 'allow', justification: 'ci' },
        { pattern: ['git', 'push'], display: 'git push', decision: 'prompt', justification: '' },
        { pattern: ['curl'], display: 'curl', decision: 'forbidden', justification: '' },
      ] },
      { name: MANAGED_SANDBOXED_RULES_FILE, managed: true, rules: [
        { pattern: ['git', 'push', '--force'], display: 'git push --force', decision: 'allow', justification: '' },
      ] },
      { name: 'custom.rules', managed: false, error: 'bad', rules: [] },
    ],
  };
  const rows = execRuleRows(snapshot);
  assert.deepEqual(rows.map((row) => row.decision), ['allow', 'prompt', 'forbidden', 'allow_sandboxed']);
  assert.equal(rows[0].justification, 'ci');
  assert.equal(otherRuleFiles(snapshot).length, 1);
  assert.equal(execRulesHaveChanges(rows, snapshot), false);
  const payload = execRulesPayload(rows);
  assert.deepEqual(payload.files[MANAGED_RULES_FILE], [
    { pattern: ['pnpm', 'test'], decision: 'allow', justification: 'ci' },
    { pattern: ['git', 'push'], decision: 'prompt' },
    { pattern: ['curl'], decision: 'forbidden' },
  ]);
  assert.deepEqual(payload.files[MANAGED_SANDBOXED_RULES_FILE], [
    { pattern: ['git', 'push', '--force'], decision: 'allow' },
  ]);
  assert.deepEqual(execRulesPayload([]).files, { [MANAGED_RULES_FILE]: [], [MANAGED_SANDBOXED_RULES_FILE]: [] });
  assert.equal(execRulesHaveChanges([], snapshot), true);
  assert.equal(EXEC_RULE_DECISIONS.length, 4);
}

// 场景:行校验。期望:空前缀、`rm` / `python` 放行、重复行都报错;`rm` 禁止可以;
// 候选并集里任一禁用项都拦。
{
  const ok = { key: 'a', decision: 'prompt', pattern: 'git push', justification: '' };
  assert.deepEqual(validateExecRuleRows([ok]), {});
  assert.match(validateExecRuleRows([{ key: 'b', decision: 'allow', pattern: '' }]).b, /不能为空/u);
  assert.match(validateExecRuleRows([{ key: 'c', decision: 'allow', pattern: 'rm -rf' }]).c, /不能放行/u);
  assert.match(validateExecRuleRows([{ key: 'd', decision: 'allow_sandboxed', pattern: 'Python -c' }]).d, /不能放行/u);
  assert.match(validateExecRuleRows([{ key: 'e', decision: 'allow', pattern: 'git|python status' }]).e, /不能放行/u);
  assert.deepEqual(validateExecRuleRows([{ key: 'f', decision: 'forbidden', pattern: 'rm' }]), {});
  const dup = validateExecRuleRows([ok, { ...ok, key: 'g', pattern: ' git   push ' }]);
  assert.match(dup.a, /重复/u);
  assert.match(dup.g, /重复/u);
}

// 场景:筛选 → query string。期望:时间窗按 now 换算 since_ms,空条件为空串,分页
// 带 before_id 与 limit。
{
  const now = 1_700_000_000_000;
  assert.equal(auditQueryString({ category: '', decision: '', time: 'all', q: '' }, { limit: 0, now }), '');
  const query = auditQueryString({ category: 'sandbox', decision: 'blocked', time: '1h', q: ' ssh ' }, { beforeId: 42, limit: 50, now });
  assert.equal(query, `?category=sandbox&decision=blocked&since_ms=${now - AUDIT_TIME_OPTIONS[1].ms}&q=ssh&limit=50&before_id=42`);
}

// 场景:审计行展示。期望:命令沙箱内 / 外、拒绝、沙箱拦截、规则记住四种标题;
// meta 把来源与原因翻成人话;多路径进 extraPaths。
{
  const inside = auditRowPresentation({ id: 1, category: 'command', decision: 'allow', source: 'auto', reason: 'known_safe', sandbox: 'workspace-write', target: 'git status', tool: 'bash', ts_ms: 5 });
  assert.equal(inside.title, '[命令安全] 沙箱内执行命令:git status');
  assert.equal(inside.tone, 'ok');
  assert.equal(inside.meta, '自动判定 · 已知安全命令 · 沙箱:workspace-write');
  const outside = auditRowPresentation({ category: 'command', decision: 'allow_session', source: 'user', reason: 'escalation_requested', sandbox: 'full-access', target: 'pnpm install' });
  assert.equal(outside.title, '[命令安全] 沙箱外执行命令:pnpm install');
  assert.equal(outside.tone, 'warn');
  const denied = auditRowPresentation({ category: 'command', decision: 'deny', source: 'user', reason: 'dangerous_command', target: 'rm -rf x' });
  assert.equal(denied.title, '[命令安全] 拒绝执行命令:rm -rf x');
  assert.equal(denied.tone, 'danger');
  const blocked = auditRowPresentation({ category: 'sandbox', decision: 'blocked', source: 'sandbox', reason: 'permission_denied', target: 'D:/secret', detail: { command: 'cat D:/secret', snippet: 'Permission denied' } });
  assert.equal(blocked.title, '[沙箱拦截] 沙箱拦截访问:D:/secret');
  assert.equal(blocked.snippet, 'Permission denied');
  const remembered = auditRowPresentation({ category: 'rule', decision: 'allow_remember', source: 'user', reason: 'remember_rule', target: 'pnpm install' });
  assert.equal(remembered.title, '[规则与授权] 记住命令规则:pnpm install');
  const patch = auditRowPresentation({ category: 'file', decision: 'allow', source: 'auto', reason: 'mode_auto', tool: 'apply_patch', target: 'a.txt', detail: { paths: ['a.txt', 'b.txt'] } });
  assert.equal(patch.title, '[文件安全] 修改文件:a.txt');
  assert.deepEqual(patch.extraPaths, ['a.txt', 'b.txt']);
  assert.equal(patch.meta, '自动判定 · 自动模式 · apply_patch');
}

// 场景:分页合并与汇总计数。期望:按 id 去重;放行类合计、拒绝类合计、拦截单列。
{
  const merged = mergeAuditEntries([{ id: 3 }, { id: 2 }], [{ id: 2 }, { id: 1 }]);
  assert.deepEqual(merged.map((entry) => entry.id), [3, 2, 1]);
  assert.deepEqual(auditSummaryCounts({ total: 9, by_decision: { allow: 4, allow_session: 1, deny: 2, forbidden: 1, blocked: 1 } }),
    { total: 9, allowed: 5, denied: 3, blocked: 1 });
  assert.deepEqual(auditSummaryCounts(null), { total: 0, allowed: 0, denied: 0, blocked: 0 });
  assert.equal(auditExportFilename('csv', new Date(2026, 8, 17, 9, 5, 3)), 'acecode_audit_20260917_090503.csv');
  assert.equal(auditExportFilename('jsonl', new Date(2026, 0, 1, 0, 0, 0)), 'acecode_audit_20260101_000000.jsonl');
}

// 场景:错误码归一。期望:404 → 不支持,401 → 认证,503+AUDIT_UNAVAILABLE 保留,
// 其它 5xx → 不可用,400 带原 code。
{
  assert.equal(securityErrorCode({ status: 404 }, 'load').code, 'SECURITY_UNSUPPORTED');
  assert.equal(securityErrorCode({ status: 401 }, 'load').code, 'SECURITY_AUTH_REQUIRED');
  assert.equal(securityErrorCode({ status: 503, code: 'AUDIT_UNAVAILABLE' }, 'load').code, 'AUDIT_UNAVAILABLE');
  assert.equal(securityErrorCode({ status: 500, code: 'UNAVAILABLE' }, 'save').code, 'SECURITY_UNAVAILABLE');
  assert.equal(securityErrorCode({ status: 400, code: 'BAD_REQUEST', message: 'x' }, 'save').code, 'BAD_REQUEST');
}

console.log('[pass] security center: sandbox draft, exec rule rows, audit filters and presentation');
