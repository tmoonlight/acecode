#pragma once

// 安全中心(openspec add-security-center)的纯逻辑层:沙盒配置快照 / PUT 解析、
// 托管规则文件快照 / PUT 解析、审计查询解析与序列化。不碰 Crow、不碰锁,
// routes_security.cpp 只做鉴权、加锁、落盘与下发。

#include "config/config.hpp"
#include "sandbox/exec_rules.hpp"
#include "sandbox/sandbox_backend.hpp"
#include "security/audit_log.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace acecode::web {

// ---- 沙盒配置(GET/PUT /api/config/sandbox)----

// 响应体:可编辑字段 + 只读透传(windows_backend / writable_roots / exclude_tmpdir)
// + 内置默认 deny 名单 + 平台探测结果。read_isolation = 该后端能否拦读
// (Windows 受限令牌管不了读,deny / read 清单只保证写不通)。
nlohmann::json sandbox_settings_snapshot(const SandboxConfig& sandbox,
                                         const sandbox::BackendProbe& probe);

// 单条清单条目校验:`~`、`~/x`、`:workspace_roots[/sub]`、`:tmpdir[/sub]`、
// `:acecode_home[/sub]` 记号原样接受;其余必须是绝对 / 根路径;allow_glob 时
// 允许 `*` `?` `[` 通配(deny 清单),`**/x` 形式也接受。返回错误文案,空 = 合法。
std::string validate_sandbox_entry(const std::string& entry, bool allow_glob);

// 解析 PUT body:出现的字段才改(`enabled` / `network_access` / `deny_defaults` /
// `filesystem.{read,write,deny}`),清单整体替换、去空白、去重。失败时 out 不变,
// field 指出出错字段(如 `filesystem.write`),error 是给用户看的文案。
bool parse_sandbox_settings_request(const nlohmann::json& body, SandboxConfig& out,
                                    std::string& error, std::string& field);

// ---- 命令规则(GET/PUT /api/security/exec-rules)----

struct ExecRulesFileSnapshot {
    std::string name;      // 文件名(不含目录)
    std::string path;      // 绝对路径(UTF-8)
    bool managed = false;  // default.rules / default.sandboxed.rules
    sandbox::RuleScope scope = sandbox::RuleScope::Global;
    std::string error;     // 整文件被跳过的解析错误;空 = 正常
    bool exists = false;
    std::vector<sandbox::PrefixRule> rules;
};

// 扫描 `<dir>/*.rules`(按名排序);两个托管文件即使不存在也列出(空规则),
// 界面上才有地方加第一条。目录不存在 = 只有两个空托管文件。
std::vector<ExecRulesFileSnapshot> read_exec_rules_dir(const std::string& dir);

nlohmann::json prefix_rule_to_json(const sandbox::PrefixRule& rule);
nlohmann::json exec_rules_snapshot(const std::string& dir,
                                   const std::vector<ExecRulesFileSnapshot>& files);

// 解析 PUT body `{files:{"default.rules":[{pattern, decision, justification}], ...}}`:
//   pattern 是字符串数组,每项可为字符串或字符串数组(候选并集);
//   decision ∈ allow / prompt / forbidden(sandboxed 文件只接受 allow);
//   非托管文件名 → 错误;allow 规则整条 pattern 命中禁用名单(与「记住」同源)或
//   首 token 是解释器 / shell / rm / sudo 这类命令 → 错误(说明是哪条)。
// 失败时 out 不变。
bool parse_exec_rules_request(const nlohmann::json& body,
                              std::map<std::string, std::vector<sandbox::PrefixRule>>& out,
                              std::string& error);

// ---- 审计(/api/security/audit*)----

// query string → AuditQuery。非法数字 / limit 越界 / 未知 category、decision 报错。
// list 默认 limit 100、上限 500;导出由路由自己把 limit 拉到最大。
bool parse_audit_query(const std::map<std::string, std::string>& params,
                       security::AuditQuery& out, std::string& error);

nlohmann::json audit_page_to_json(const security::AuditPage& page);
nlohmann::json audit_summary_to_json(const security::AuditSummary& summary);

// 导出文件名:`acecode-audit-<yyyymmdd-hhmmss>.<ext>`。
std::string audit_export_filename(const std::string& format, std::int64_t now_ms);

} // namespace acecode::web
