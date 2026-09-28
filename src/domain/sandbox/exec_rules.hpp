#pragma once

// exec 规则文件(openspec add-auto-mode-sandbox,与 Codex execpolicy 的 `.rules`
// 互通的子集)。
//
// 语法(Starlark 子集,手写解析,不引入解释器):
//
//     # 注释
//     prefix_rule(
//         pattern = ["git", ["commit", "add"]],   # 有序 token;列表项 = 任一相等
//         decision = "allow",                     # allow(默认) / prompt / forbidden
//         justification = "why",                  # 可选
//         match = ["git commit -m x"],            # 可选,加载时校验必须命中
//         not_match = ["git push"],               # 可选,加载时校验必须不命中
//     )
//     host_executable(name = "git", paths = ["/usr/bin/git"])   # 整条忽略
//
// 任何其它语法、缺 pattern、非法 decision、match/not_match 校验失败 → 该文件
// **整体**跳过(不部分生效)并记 LOG_WARN。加载位置:全局 `<data_dir>/rules/*.rules`,
// 项目 `<cwd>/.acecode/rules/*.rules`。项目作用域的 allow 降级为 AllowSandboxed:
// 免确认但不绕过沙盒 —— 项目文件不可信,不能凭它把命令放到沙盒外。

#include "command_classifier.hpp"

#include <string>
#include <vector>

namespace acecode::sandbox {

// Sandboxed:全局目录里的 `*.sandboxed.rules`(align-codex-sandboxing D6),由
// 「批准并记住」沙盒内批准写入;allow 与项目作用域一样降级为 AllowSandboxed。
enum class RuleScope { Global, Project, Sandboxed };

enum class RuleDecision {
    NoMatch,
    Allow,            // 全局 allow:免确认,绕过沙盒
    AllowSandboxed,   // 项目 allow:免确认,仍在沙盒内
    Prompt,
    Forbidden,
};

const char* rule_decision_name(RuleDecision d);

struct PrefixRule {
    // pattern[i] 是第 i 个 token 的候选集合(单字符串 = 只有一个候选)。
    std::vector<std::vector<std::string>> pattern;
    RuleDecision decision = RuleDecision::Allow;   // 只会是 Allow / Prompt / Forbidden
    std::string justification;
    RuleScope scope = RuleScope::Global;
    std::string source_file;
};

struct RuleMatch {
    const PrefixRule* rule = nullptr;
    RuleDecision decision = RuleDecision::NoMatch;   // 已按作用域降级
};

struct RuleEvaluation {
    RuleDecision decision = RuleDecision::NoMatch;   // 整体决策(逐段取最严格)
    std::vector<RuleMatch> matches;                  // 命中的规则(所有段)
    std::string justification;                       // 第一条决定性规则的说明
};

struct ParsedRulesFile {
    std::vector<PrefixRule> rules;
    std::string error;   // 非空 = 整文件跳过的原因
};

// 解析一份规则文件内容。scope / source_file 会写进每条规则。
ParsedRulesFile parse_rules_text(const std::string& text, RuleScope scope,
                                 const std::string& source_file);

class ExecRules {
public:
    // 从 `<global_dir>/*.rules` 与 `<project_dir>/*.rules` 加载;目录不存在即空。
    // 解析失败的文件记 LOG_WARN 并跳过。
    static ExecRules load(const std::string& global_rules_dir,
                          const std::string& project_rules_dir);

    void add_rule(PrefixRule rule) { rules_.push_back(std::move(rule)); }
    bool empty() const { return rules_.empty(); }
    std::size_t size() const { return rules_.size(); }
    const std::vector<PrefixRule>& rules() const { return rules_; }
    const std::vector<std::string>& skipped_files() const { return skipped_files_; }

    // 单段匹配:所有命中规则里最严格的决策(已按作用域降级)。
    RuleEvaluation evaluate_segment(const CommandSegment& segment) const;
    // 多段:逐段评估,整体取最严格;只有每段都是 allow(任一作用域)才算 allow,
    // 且只要有一段是 AllowSandboxed,整体就是 AllowSandboxed。
    RuleEvaluation evaluate(const std::vector<CommandSegment>& segments) const;
    RuleEvaluation evaluate(const CommandClassification& command) const;

private:
    std::vector<PrefixRule> rules_;
    std::vector<std::string> skipped_files_;
};

// 单条规则是否命中一段命令(首 token 允许 basename 回退,大小写不敏感)。
bool prefix_rule_matches(const PrefixRule& rule, const CommandSegment& segment);

// ---- 「批准并记住」的规则写回(align-codex-sandboxing D6)----

// 全局规则目录里两个写回文件的文件名:沙盒外批准 / 沙盒内批准。
constexpr const char* kRememberedRulesFile = "default.rules";
constexpr const char* kRememberedSandboxedRulesFile = "default.sandboxed.rules";

// 禁用前缀(移植 Codex BANNED_PREFIX_SUGGESTIONS 并补 cmd / PowerShell 拼写):
// 解释器、shell、`rm` / `del`、`sudo`、`git` 单独等永远不作为可记住的前缀。
// 首 token 按 basename 比较(`C:\...\node.exe` = `node`),大小写不敏感。
bool is_banned_prefix(const std::vector<std::string>& tokens);

// 推导要写进规则文件的 pattern(每段一条,单候选 token 列表)。
//   proposed 非空:必须不在禁用名单、且每一段命令都以它开头,才作为唯一 pattern;
//   否则每段用 always_allow_prefix_tokens_for_segment;任一段推不出前缀或命中禁用
//   名单 → 返回空(不提供「记住」选项)。不可安全拆段的命令同样返回空。
std::vector<std::vector<std::string>> derive_remember_patterns(
    const CommandClassification& command, const std::vector<std::string>& proposed);

// `prefix_rule(pattern=["git", "commit"], decision="allow")`(与 Codex amend.rs 同款)。
std::string format_prefix_rule(const std::vector<std::string>& pattern);

// 追加规则到文件(不存在则创建,父目录一并创建);已存在同 pattern 的 allow 规则
// 时跳过。返回错误信息,空 = 成功。
std::string append_prefix_rules(const std::string& file,
                                const std::vector<std::vector<std::string>>& patterns);

// ---- 托管规则文件的整体重写(openspec add-security-center D3)----

// 只有「批准并记住」写的两个文件由设置页托管:界面上增删改的就是它们,
// 用户手写的其它 *.rules 只读展示。
bool is_managed_rules_file(const std::string& file_name);
// 文件名 → 作用域:`*.sandboxed.rules` 为 Sandboxed,其余 Global。
RuleScope rules_file_scope(const std::string& file_name);

// 完整格式化一条规则:pattern 保留候选并集(`["git", ["status", "diff"]]`),
// decision 与非空 justification 一并输出,字符串按 Starlark 转义。
std::string format_prefix_rule_full(const PrefixRule& rule);
// 整个文件的文本:注释头 + 每条规则一行;空表只剩注释头(加载后无规则、无错误)。
std::string render_rules_file(const std::vector<PrefixRule>& rules);
// 先对渲染结果做 parse_rules_text 往返校验,过了才原子落盘;返回错误信息,空 = 成功。
std::string write_rules_file(const std::string& file, const std::vector<PrefixRule>& rules);

} // namespace acecode::sandbox
