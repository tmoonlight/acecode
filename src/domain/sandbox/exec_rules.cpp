#include "exec_rules.hpp"

#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <set>

namespace acecode::sandbox {

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

int severity(RuleDecision d) {
    switch (d) {
        case RuleDecision::Forbidden:      return 4;
        case RuleDecision::Prompt:         return 3;
        case RuleDecision::AllowSandboxed: return 2;
        case RuleDecision::Allow:          return 1;
        case RuleDecision::NoMatch:        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 极简 Starlark 子集解析器
// ---------------------------------------------------------------------------

struct Value {
    enum class Kind { String, List } kind = Kind::String;
    std::string str;
    std::vector<Value> list;
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    // 返回 false = 语法错误,error_ 里有原因。
    bool parse(std::vector<PrefixRule>& out, RuleScope scope, const std::string& source) {
        while (true) {
            skip_ws();
            if (eof()) return true;
            std::string ident;
            if (!read_ident(ident)) return fail("expected a call such as prefix_rule(...)");
            skip_ws();
            if (!consume('(')) return fail("expected '(' after " + ident);
            std::vector<std::pair<std::string, Value>> kwargs;
            if (!read_kwargs(kwargs)) return false;
            if (ident == "prefix_rule") {
                PrefixRule rule;
                rule.scope = scope;
                rule.source_file = source;
                if (!build_rule(kwargs, rule)) return false;
                out.push_back(std::move(rule));
            } else if (ident == "host_executable") {
                // 忽略。
            } else {
                return fail("unsupported call: " + ident);
            }
        }
    }

    const std::string& error() const { return error_; }

private:
    bool eof() const { return pos_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[pos_]; }

    bool fail(const std::string& why) {
        error_ = why + " (line " + std::to_string(line_) + ")";
        return false;
    }

    void skip_ws() {
        while (!eof()) {
            const char c = s_[pos_];
            if (c == '\n') { ++line_; ++pos_; continue; }
            if (c == ' ' || c == '\t' || c == '\r') { ++pos_; continue; }
            if (c == '#') {
                while (!eof() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    bool consume(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }

    bool read_ident(std::string& out) {
        out.clear();
        while (!eof()) {
            const char c = s_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                out += c;
                ++pos_;
            } else {
                break;
            }
        }
        return !out.empty();
    }

    bool read_string(std::string& out) {
        const char quote = peek();
        if (quote != '"' && quote != '\'') return fail("expected a string");
        ++pos_;
        out.clear();
        while (!eof() && s_[pos_] != quote) {
            char c = s_[pos_];
            if (c == '\n') return fail("unterminated string");
            if (c == '\\' && pos_ + 1 < s_.size()) {
                const char n = s_[pos_ + 1];
                if (n == 'n') out += '\n';
                else if (n == 't') out += '\t';
                else if (n == '\\') out += '\\';
                else if (n == '"') out += '"';
                else if (n == '\'') out += '\'';
                else return fail("unsupported escape in string");
                pos_ += 2;
                continue;
            }
            out += c;
            ++pos_;
        }
        if (eof()) return fail("unterminated string");
        ++pos_;
        return true;
    }

    bool read_value(Value& v, int depth = 0) {
        if (depth > 8) return fail("list nesting too deep");
        skip_ws();
        if (peek() == '[') {
            ++pos_;
            v.kind = Value::Kind::List;
            while (true) {
                skip_ws();
                if (consume(']')) return true;
                Value item;
                if (!read_value(item, depth + 1)) return false;
                v.list.push_back(std::move(item));
                skip_ws();
                if (consume(',')) continue;
                skip_ws();
                if (consume(']')) return true;
                return fail("expected ',' or ']' in list");
            }
        }
        v.kind = Value::Kind::String;
        return read_string(v.str);
    }

    bool read_kwargs(std::vector<std::pair<std::string, Value>>& out) {
        while (true) {
            skip_ws();
            if (consume(')')) return true;
            std::string key;
            if (!read_ident(key)) return fail("expected a keyword argument");
            skip_ws();
            if (!consume('=')) return fail("expected '=' after " + key);
            Value v;
            if (!read_value(v)) return false;
            out.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (consume(',')) continue;
            skip_ws();
            if (consume(')')) return true;
            return fail("expected ',' or ')' in call");
        }
    }

    bool build_rule(const std::vector<std::pair<std::string, Value>>& kwargs, PrefixRule& rule) {
        bool has_pattern = false;
        std::vector<std::string> match_examples;
        std::vector<std::string> not_match_examples;
        std::set<std::string> seen;
        for (const auto& [key, v] : kwargs) {
            if (!seen.insert(key).second) return fail("duplicate keyword: " + key);
            if (key == "pattern") {
                if (v.kind != Value::Kind::List || v.list.empty()) return fail("pattern must be a non-empty list");
                for (const auto& item : v.list) {
                    std::vector<std::string> alts;
                    if (item.kind == Value::Kind::String) {
                        if (item.str.empty()) return fail("pattern tokens must be non-empty");
                        alts.push_back(item.str);
                    } else {
                        if (item.list.empty()) return fail("pattern alternatives must be non-empty");
                        for (const auto& alt : item.list) {
                            if (alt.kind != Value::Kind::String || alt.str.empty()) {
                                return fail("pattern alternatives must be strings");
                            }
                            alts.push_back(alt.str);
                        }
                    }
                    rule.pattern.push_back(std::move(alts));
                }
                has_pattern = true;
            } else if (key == "decision") {
                if (v.kind != Value::Kind::String) return fail("decision must be a string");
                const std::string d = lower(v.str);
                if (d == "allow") rule.decision = RuleDecision::Allow;
                else if (d == "prompt") rule.decision = RuleDecision::Prompt;
                else if (d == "forbidden") rule.decision = RuleDecision::Forbidden;
                else return fail("invalid decision: " + v.str);
            } else if (key == "justification") {
                if (v.kind != Value::Kind::String) return fail("justification must be a string");
                rule.justification = v.str;
            } else if (key == "match" || key == "not_match") {
                if (v.kind != Value::Kind::List) return fail(key + " must be a list");
                for (const auto& item : v.list) {
                    if (item.kind != Value::Kind::String) return fail(key + " entries must be strings");
                    (key == "match" ? match_examples : not_match_examples).push_back(item.str);
                }
            } else {
                return fail("unsupported prefix_rule argument: " + key);
            }
        }
        if (!has_pattern) return fail("prefix_rule requires pattern");
        for (const auto& example : match_examples) {
            CommandClassification c = classify_command(example);
            bool any = false;
            for (const auto& seg : c.segments) {
                if (prefix_rule_matches(rule, seg)) { any = true; break; }
            }
            if (!any) return fail("match example does not match its rule: " + example);
        }
        for (const auto& example : not_match_examples) {
            CommandClassification c = classify_command(example);
            for (const auto& seg : c.segments) {
                if (prefix_rule_matches(rule, seg)) {
                    return fail("not_match example unexpectedly matches: " + example);
                }
            }
        }
        return true;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    int line_ = 1;
    std::string error_;
};

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "!unreadable rules file";
    std::string text(1024 * 1024 + 1, '\0');
    ifs.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(ifs.gcount()));
    return text;
}

void load_dir(const std::string& dir, RuleScope scope, std::vector<PrefixRule>& out,
              std::vector<std::string>& skipped) {
    if (dir.empty()) return;
    std::error_code ec;
    const std::filesystem::path root = path_from_utf8(dir);
    if (!std::filesystem::is_directory(root, ec) || ec) return;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) { ec.clear(); continue; }
        if (entry.path().extension() == ".rules") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const std::string name = path_to_utf8(file);
        // 全局目录里 `*.sandboxed.rules` 是「批准并记住」的沙盒内批准(D6):
        // allow 降级为免确认但仍沙盒。
        RuleScope file_scope = scope;
        const std::string filename = path_to_utf8(file.filename());
        const std::string suffix = ".sandboxed.rules";
        if (scope == RuleScope::Global && filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            file_scope = RuleScope::Sandboxed;
        }
        ParsedRulesFile parsed = parse_rules_text(read_text_file(file), file_scope, name);
        if (!parsed.error.empty()) {
            LOG_WARN("[sandbox] skipping exec rules file " + name + ": " + parsed.error);
            skipped.push_back(name + ": " + parsed.error);
            continue;
        }
        for (auto& rule : parsed.rules) out.push_back(std::move(rule));
    }
}

RuleDecision degrade_for_scope(RuleDecision d, RuleScope scope) {
    if (d == RuleDecision::Allow && (scope == RuleScope::Project || scope == RuleScope::Sandboxed)) {
        return RuleDecision::AllowSandboxed;
    }
    return d;
}

// Codex BANNED_PREFIX_SUGGESTIONS(core/src/exec_policy.rs)+ acecode 补的 cmd /
// PowerShell 拼写。每项是完整 token 序列:`["git"]` 禁而 `["git","commit"]` 不禁。
const std::vector<std::vector<std::string>>& banned_prefixes() {
    static const std::vector<std::vector<std::string>> banned = {
        {"/bin/bash"}, {"/bin/bash", "-c"}, {"/bin/bash", "-lc"}, {"/bin/sh"}, {"/bin/sh", "-c"},
        {"/bin/sh", "-lc"}, {"/bin/zsh"}, {"/bin/zsh", "-c"}, {"/bin/zsh", "-lc"}, {"rscript"},
        {"bash"}, {"bash", "-c"}, {"bash", "-lc"}, {"bun"}, {"bun", "-e"}, {"bun", "run"},
        {"cmd"}, {"cmd", "/c"}, {"cmd", "/k"}, {"cmd", "/d"}, {"dash"}, {"dash", "-c"},
        {"deno"}, {"deno", "eval"}, {"env"}, {"fish"}, {"fish", "-c"}, {"git"}, {"julia"},
        {"julia", "-e"}, {"ksh"}, {"ksh", "-c"}, {"lua"}, {"lua", "-e"}, {"node"}, {"node", "-e"},
        {"nodejs"}, {"nodejs", "-e"}, {"npm", "run"}, {"osascript"}, {"perl"}, {"perl", "-e"},
        {"php"}, {"php", "-r"}, {"pnpm", "run"}, {"powershell"}, {"powershell", "-command"},
        {"powershell", "-encodedcommand"}, {"powershell", "-file"}, {"powershell", "-c"},
        {"pwsh"}, {"pwsh", "-command"}, {"pwsh", "-encodedcommand"}, {"pwsh", "-file"},
        {"pwsh", "-c"}, {"pwsh", "-e"}, {"pwsh", "-ec"}, {"pwsh", "-f"}, {"py"}, {"py", "-3"},
        {"pypy"}, {"pypy3"}, {"python"}, {"python", "-"}, {"python", "-c"}, {"python3"},
        {"python3", "-"}, {"python3", "-c"}, {"pythonw"}, {"pyw"}, {"rm"}, {"ruby"},
        {"ruby", "-e"}, {"sh"}, {"sh", "-c"}, {"sh", "-lc"}, {"sudo"}, {"yarn", "run"}, {"zsh"},
        {"zsh", "-c"}, {"zsh", "-lc"},
        // acecode 补:cmd / PowerShell 的删除与提权拼写,以及通用启动器。
        {"del"}, {"erase"}, {"rd"}, {"rmdir"}, {"remove-item"}, {"ri"}, {"doas"}, {"su"},
        {"runas"}, {"start"}, {"start-process"}, {"xargs"}, {"eval"}, {"exec"}, {"source"},
        {"wscript"}, {"cscript"}, {"mshta"}, {"npx"}, {"invoke-expression"}, {"iex"},
    };
    return banned;
}

} // namespace

const char* rule_decision_name(RuleDecision d) {
    switch (d) {
        case RuleDecision::NoMatch:        return "no_match";
        case RuleDecision::Allow:          return "allow";
        case RuleDecision::AllowSandboxed: return "allow_sandboxed";
        case RuleDecision::Prompt:         return "prompt";
        case RuleDecision::Forbidden:      return "forbidden";
    }
    return "no_match";
}

ParsedRulesFile parse_rules_text(const std::string& text, RuleScope scope,
                                 const std::string& source_file) {
    ParsedRulesFile out;
    if (text.size() > 1024 * 1024) { out.error = "rules file exceeds 1 MiB"; return out; }
    Parser parser(text);
    if (!parser.parse(out.rules, scope, source_file)) {
        out.rules.clear();
        out.error = parser.error();
    }
    return out;
}

bool prefix_rule_matches(const PrefixRule& rule, const CommandSegment& segment) {
    if (rule.pattern.empty() || segment.tokens.size() < rule.pattern.size()) return false;
    for (std::size_t i = 0; i < rule.pattern.size(); ++i) {
        const std::string& tok = segment.tokens[i];
        bool any = false;
        for (const auto& alt : rule.pattern[i]) {
            if (tok == alt) { any = true; break; }
            if (i == 0 && command_basename(tok) == command_basename(alt)) { any = true; break; }
        }
        if (!any) return false;
    }
    return true;
}

ExecRules ExecRules::load(const std::string& global_rules_dir,
                          const std::string& project_rules_dir) {
    ExecRules rules;
    load_dir(global_rules_dir, RuleScope::Global, rules.rules_, rules.skipped_files_);
    load_dir(project_rules_dir, RuleScope::Project, rules.rules_, rules.skipped_files_);
    return rules;
}

RuleEvaluation ExecRules::evaluate_segment(const CommandSegment& segment) const {
    RuleEvaluation out;
    for (const auto& rule : rules_) {
        if (!prefix_rule_matches(rule, segment)) continue;
        RuleMatch m;
        m.rule = &rule;
        m.decision = degrade_for_scope(rule.decision, rule.scope);
        if (severity(m.decision) > severity(out.decision)) {
            out.decision = m.decision;
            out.justification = rule.justification;
        }
        out.matches.push_back(m);
    }
    return out;
}

RuleEvaluation ExecRules::evaluate(const std::vector<CommandSegment>& segments) const {
    RuleEvaluation out;
    if (segments.empty()) return out;
    bool all_allow = true;
    bool any_sandboxed = false;
    for (const auto& seg : segments) {
        RuleEvaluation e = evaluate_segment(seg);
        for (const auto& m : e.matches) out.matches.push_back(m);
        if (e.decision == RuleDecision::Prompt || e.decision == RuleDecision::Forbidden) {
            if (severity(e.decision) > severity(out.decision)) {
                out.decision = e.decision;
                out.justification = e.justification;
            }
            all_allow = false;
        } else if (e.decision == RuleDecision::Allow) {
            if (out.justification.empty()) out.justification = e.justification;
        } else if (e.decision == RuleDecision::AllowSandboxed) {
            any_sandboxed = true;
            if (out.justification.empty()) out.justification = e.justification;
        } else {
            all_allow = false;
        }
    }
    if (out.decision == RuleDecision::Prompt || out.decision == RuleDecision::Forbidden) return out;
    if (all_allow) out.decision = any_sandboxed ? RuleDecision::AllowSandboxed : RuleDecision::Allow;
    return out;
}

RuleEvaluation ExecRules::evaluate(const CommandClassification& command) const {
    auto result = evaluate(command.segments);
    if (!command.split_safely &&
        (result.decision == RuleDecision::Allow || result.decision == RuleDecision::AllowSandboxed)) {
        result.decision = RuleDecision::NoMatch;
        result.justification.clear();
    }
    // 包装器不会让内层的 forbidden/prompt 规则失效,内层 allow 则不能自动授权包装器。
    for (const auto& segment : command.nested_segments) {
        auto nested = evaluate_segment(segment);
        if ((nested.decision == RuleDecision::Forbidden || nested.decision == RuleDecision::Prompt) &&
            severity(nested.decision) > severity(result.decision)) {
            result.decision = nested.decision;
            result.justification = nested.justification;
        }
    }
    return result;
}

bool is_banned_prefix(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return true;
    for (const auto& banned : banned_prefixes()) {
        if (banned.size() != tokens.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < banned.size() && same; ++i) {
            const std::string actual = i == 0 ? command_basename(tokens[i]) : lower(tokens[i]);
            const std::string expected = i == 0 ? command_basename(banned[i]) : banned[i];
            if (actual != expected) same = false;
        }
        if (same) return true;
    }
    return false;
}

std::vector<std::vector<std::string>> derive_remember_patterns(
    const CommandClassification& command, const std::vector<std::string>& proposed) {
    std::vector<std::vector<std::string>> out;
    if (!command.split_safely || command.segments.empty()) return out;
    if (!proposed.empty()) {
        if (is_banned_prefix(proposed)) return out;
        for (const auto& segment : command.segments) {
            if (segment.tokens.size() < proposed.size()) return out;
            for (std::size_t i = 0; i < proposed.size(); ++i) {
                const bool same = i == 0 ? command_basename(segment.tokens[i]) == command_basename(proposed[i])
                                         : segment.tokens[i] == proposed[i];
                if (!same) return out;
            }
        }
        out.push_back(proposed);
        return out;
    }
    for (const auto& segment : command.segments) {
        auto prefix = always_allow_prefix_tokens_for_segment(segment);
        if (prefix.empty() || is_banned_prefix(prefix)) return {};
        bool duplicate = false;
        for (const auto& existing : out) {
            if (existing == prefix) { duplicate = true; break; }
        }
        if (!duplicate) out.push_back(std::move(prefix));
    }
    return out;
}

std::string format_prefix_rule(const std::vector<std::string>& pattern) {
    std::string out = "prefix_rule(pattern=[";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (i) out += ", ";
        out += '"';
        for (char c : pattern[i]) {
            if (c == '\\' || c == '"') out += '\\';
            if (c == '\n') { out += "\\n"; continue; }
            out += c;
        }
        out += '"';
    }
    out += "], decision=\"allow\")";
    return out;
}

bool is_managed_rules_file(const std::string& file_name) {
    return file_name == kRememberedRulesFile || file_name == kRememberedSandboxedRulesFile;
}

RuleScope rules_file_scope(const std::string& file_name) {
    const std::string suffix = ".sandboxed.rules";
    if (file_name.size() > suffix.size() &&
        file_name.compare(file_name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return RuleScope::Sandboxed;
    }
    return RuleScope::Global;
}

namespace {

std::string starlark_string(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\r') { out += "\\r"; continue; }
        if (c == '\t') { out += "\\t"; continue; }
        out += c;
    }
    out += '"';
    return out;
}

} // namespace

std::string format_prefix_rule_full(const PrefixRule& rule) {
    std::string out = "prefix_rule(pattern=[";
    for (std::size_t i = 0; i < rule.pattern.size(); ++i) {
        if (i) out += ", ";
        const auto& alternatives = rule.pattern[i];
        if (alternatives.size() == 1) {
            out += starlark_string(alternatives[0]);
            continue;
        }
        out += '[';
        for (std::size_t j = 0; j < alternatives.size(); ++j) {
            if (j) out += ", ";
            out += starlark_string(alternatives[j]);
        }
        out += ']';
    }
    out += "], decision=";
    out += starlark_string(rule_decision_name(rule.decision == RuleDecision::AllowSandboxed
                                                  ? RuleDecision::Allow : rule.decision));
    if (!rule.justification.empty()) {
        out += ", justification=";
        out += starlark_string(rule.justification);
    }
    out += ')';
    return out;
}

std::string render_rules_file(const std::vector<PrefixRule>& rules) {
    std::string out =
        "# Managed by ACECode (Settings > Security Center). Edits here are overwritten\n"
        "# the next time the rules are saved from the UI or a command is remembered.\n";
    for (const auto& rule : rules) {
        out += format_prefix_rule_full(rule);
        out += '\n';
    }
    return out;
}

std::string write_rules_file(const std::string& file, const std::vector<PrefixRule>& rules) {
    if (file.empty()) return "rules file path is empty";
    for (const auto& rule : rules) {
        if (rule.pattern.empty()) return "a rule has an empty pattern";
        for (const auto& alternatives : rule.pattern) {
            if (alternatives.empty()) return "a rule has an empty token position";
            for (const auto& token : alternatives) {
                if (token.empty()) return "a rule has an empty token";
            }
        }
        if (rule.decision != RuleDecision::Allow && rule.decision != RuleDecision::AllowSandboxed &&
            rule.decision != RuleDecision::Prompt && rule.decision != RuleDecision::Forbidden) {
            return "a rule has an invalid decision";
        }
    }
    const std::string text = render_rules_file(rules);
    // 往返校验:渲染出来的文本必须能被自己的解析器读回同样多的规则,否则一次
    // 界面保存就能让整个文件被跳过(解析失败 = 整文件不生效)。
    const auto name = path_to_utf8(path_from_utf8(file).filename());
    const ParsedRulesFile parsed = parse_rules_text(text, rules_file_scope(name), name);
    if (!parsed.error.empty()) return "rendered rules do not parse: " + parsed.error;
    if (parsed.rules.size() != rules.size()) return "rendered rules do not round-trip";
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path = path_from_utf8(file);
    fs::create_directories(path.parent_path(), ec);
    if (ec) return "cannot create rules directory: " + ec.message();
    if (!atomic_write_file(file, text)) return "cannot write rules file: " + file;
    return {};
}

std::string append_prefix_rules(const std::string& file,
                                const std::vector<std::vector<std::string>>& patterns) {
    if (file.empty()) return "rules file path is empty";
    if (patterns.empty()) return "no prefix pattern to remember";
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path = path_from_utf8(file);
    fs::create_directories(path.parent_path(), ec);
    if (ec) return "cannot create rules directory: " + ec.message();
    std::string existing;
    if (fs::exists(path, ec) && !ec) {
        existing = read_text_file(path);
        if (!existing.empty() && existing[0] == '!') return "cannot read rules file: " + file;
    }
    // 去重:已有同 pattern 的 allow 规则就不再追加(文件解析失败时视为无规则,
    // 追加不会让它更坏 —— 整文件本来就会被跳过)。
    std::vector<std::vector<std::string>> pending;
    const ParsedRulesFile parsed = parse_rules_text(existing, RuleScope::Global, file);
    for (const auto& pattern : patterns) {
        bool duplicate = false;
        for (const auto& rule : parsed.rules) {
            if (rule.decision != RuleDecision::Allow || rule.pattern.size() != pattern.size()) continue;
            bool same = true;
            for (std::size_t i = 0; i < pattern.size() && same; ++i) {
                if (rule.pattern[i].size() != 1 || rule.pattern[i][0] != pattern[i]) same = false;
            }
            if (same) { duplicate = true; break; }
        }
        if (!duplicate) pending.push_back(pattern);
    }
    if (pending.empty()) return {};
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return "cannot open rules file for writing: " + file;
    if (!existing.empty() && existing.back() != '\n') out << '\n';
    for (const auto& pattern : pending) out << format_prefix_rule(pattern) << '\n';
    if (!out) return "cannot write rules file: " + file;
    return {};
}

} // namespace acecode::sandbox
