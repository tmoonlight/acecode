// confirm_question.cpp
// 见 confirm_question.hpp 的注释。本实现给三类常见工具(bash / file_write /
// file_edit)生成多行结构化的 confirm 标题:
//
//   bash:
//     Do you want to run this command?
//       $ <第 1 行命令(≤120 字符)>
//         <第 2 行命令>            (最多 3 行,溢出显示 "    ...")
//
//   file_write:
//     Do you want to write to <path>?
//       <N> line(s), <M> byte(s)
//
//   file_edit:
//     Do you want to edit <path>?
//       - <old_string 第一行,≤60 字符>
//       + <new_string 第一行,≤60 字符>
//
//   其他/异常:
//     Do you want to use <tool_name>?
//
// 路径截断阈值从早期的 40 放宽到 80,因为 confirm 弹窗比工具调用 preview
// 行宽,且用户决策时需要看到尽量完整的目标路径。命令截断阈值 120,允许多行
// 但只显示前 3 行,避免长 heredoc / pipeline 把 overlay 撑到屏幕外。

#include "tui/confirm_question.hpp"

#include "tool/apply_patch_format.hpp"

#include <nlohmann/json.hpp>

namespace acecode::tui {

namespace {

// 路径太长时头部省略,保留尾部文件名(用户最关心的部分)。
std::string truncate_path(std::string p) {
    if (p.size() > 80) p = "..." + p.substr(p.size() - 77);
    return p;
}

// 单行命令尾部省略。
std::string truncate_command_line(std::string line) {
    if (line.size() > 120) line = line.substr(0, 117) + "...";
    return line;
}

// 多行命令格式化为带前缀的 block:首行 "  $ ",后续行 "    "(4 空格对齐)。
// 最多 3 行,如还有更多行,在末尾追加 "    ...";单行命令直接返回 "  $ <line>"。
std::string format_command_block(const std::string& cmd) {
    std::string out;
    size_t pos = 0;
    int emitted = 0;
    bool more = false;
    while (pos < cmd.size()) {
        size_t nl = cmd.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? cmd.substr(pos) : cmd.substr(pos, nl - pos);
        if (emitted >= 3) { more = true; break; }
        out += (emitted == 0 ? "  $ " : "    ");
        out += truncate_command_line(line);
        ++emitted;
        if (nl == std::string::npos) break;
        out += "\n";
        pos = nl + 1;
    }
    if (more) out += "\n    ...";
    return out;
}

std::string format_plan_preview(const std::string& plan) {
    if (plan.empty()) return "  (plan file is empty)";
    std::string out;
    size_t pos = 0;
    int emitted = 0;
    bool more = false;
    while (pos <= plan.size()) {
        size_t nl = plan.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? plan.substr(pos)
            : plan.substr(pos, nl - pos);
        if (emitted >= 6) { more = true; break; }
        out += "  ";
        out += truncate_command_line(line);
        ++emitted;
        if (nl == std::string::npos) break;
        out += "\n";
        pos = nl + 1;
    }
    if (more) out += "\n  ...";
    return out;
}

// 取第一行并截断到 max 字符;若原字符串多行,在末尾加 "↵" 提示还有后续。
std::string truncate_first_line(const std::string& s, size_t max = 60) {
    size_t nl = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    bool truncated_chars = false;
    if (line.size() > max) {
        line = line.substr(0, max - 3) + "...";
        truncated_chars = true;
    }
    bool has_more_lines = (nl != std::string::npos);
    if (has_more_lines && !truncated_chars) {
        line += " \xE2\x86\xB5";  // U+21B5 ↵
    }
    return line;
}

// 返回 content 的字节数与逻辑行数(末行不带 \n 也算一行)。
struct ContentStats { size_t bytes; int lines; };
ContentStats count_content_stats(const std::string& content) {
    ContentStats s;
    s.bytes = content.size();
    s.lines = 0;
    for (char c : content) if (c == '\n') ++s.lines;
    if (!content.empty() && content.back() != '\n') ++s.lines;
    return s;
}

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}
std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

std::string build_confirm_question(const std::string& tool_name,
                                   const std::string& arguments_json) {
    try {
        auto j = nlohmann::json::parse(arguments_json);

        if (tool_name == "EnterPlanMode" ||
            j.value("kind", std::string{}) == "enter_plan_mode") {
            std::string out = "Enter plan mode?";
            if (j.contains("plan_file_path") && j["plan_file_path"].is_string()) {
                out += "\nPlan file: " + truncate_path(j["plan_file_path"].get<std::string>());
            }
            return out;
        }

        if (tool_name == "ExitPlanMode" ||
            j.value("kind", std::string{}) == "plan_approval") {
            std::string out = "Exit plan mode and approve this plan?";
            if (j.contains("plan_file_path") && j["plan_file_path"].is_string()) {
                out += "\nPlan file: " + truncate_path(j["plan_file_path"].get<std::string>());
            }
            if (j.contains("plan") && j["plan"].is_string()) {
                out += "\n" + format_plan_preview(j["plan"].get<std::string>());
            }
            return out;
        }

        if (tool_name == "bash") {
            std::string out = "Do you want to run this command?";
            if (j.contains("permission") && j["permission"].is_object()) {
                const auto& permission = j["permission"];
                const auto reason = permission.value("reason", std::string{});
                if (reason == "dangerous_command") out = "模型要执行一条危险命令";
                else if (reason == "escalation_requested") out = "模型申请在沙盒外执行";
                else if (reason == "additional_permissions_requested") out = "模型申请临时加宽沙盒权限";
                else if (reason == "unknown_command_without_sandbox") out = "本平台没有可用沙盒,未知命令需要确认";
                else if (reason == "rule_prompt") out = "执行规则要求确认这条命令";
                const auto request = permission.value("request", std::string{});
                if (request == "require_escalated" || j.value("with_escalated_permissions", false)) {
                    out += "\n执行范围:沙盒外";
                } else if (request == "with_additional_permissions") {
                    out += "\n执行范围:沙盒内 + 申请的额外权限";
                }
                const auto justification = j.value("justification", std::string{});
                if (!justification.empty()) out += "\n" + format_command_block(justification);
                if (permission.contains("additional_permissions") && permission["additional_permissions"].is_object()) {
                    const auto& ap = permission["additional_permissions"];
                    for (const char* field : {"write", "read"}) {
                        if (!ap.contains(field) || !ap[field].is_array()) continue;
                        for (const auto& item : ap[field]) {
                            if (item.is_string()) out += std::string("\n  + ") + field + " " + truncate_path(item.get<std::string>());
                        }
                    }
                    if (ap.value("network", false)) out += "\n  + network";
                }
                const auto denied_path = permission.value("denied_path", std::string{});
                if (!denied_path.empty()) out += "\n上一次被沙盒拒绝的路径: " + truncate_path(denied_path);
                const auto prefix = permission.value("always_allow_prefix", std::string{});
                if (!prefix.empty()) out += "\n本次会话允许的前缀: " + truncate_command_line(prefix);
            }
            if (j.contains("command") && j["command"].is_string()) {
                std::string cmd = j["command"].get<std::string>();
                if (!cmd.empty()) out += "\n" + format_command_block(cmd);
            }
            return out;
        }

        if (tool_name == "file_write") {
            if (j.contains("file_path") && j["file_path"].is_string()) {
                std::string p = j["file_path"].get<std::string>();
                std::string out = "Do you want to write to " + truncate_path(p) + "?";
                if (j.contains("content") && j["content"].is_string()) {
                    auto s = count_content_stats(j["content"].get<std::string>());
                    out += "\n  " + plural(s.lines, "line", "lines")
                        +  ", " + plural(s.bytes, "byte", "bytes");
                }
                return out;
            }
        }

        if (tool_name == "file_edit") {
            if (j.contains("file_path") && j["file_path"].is_string()) {
                std::string p = j["file_path"].get<std::string>();
                std::string out = "Do you want to edit " + truncate_path(p) + "?";
                if (j.contains("old_string") && j["old_string"].is_string() &&
                    j.contains("new_string") && j["new_string"].is_string()) {
                    out += "\n  - " + truncate_first_line(j["old_string"].get<std::string>());
                    out += "\n  + " + truncate_first_line(j["new_string"].get<std::string>());
                }
                return out;
            }
        }

        if (tool_name == "apply_patch") {
            // 一份补丁可能改多个文件:列出每个文件与操作(A 新建 / M 修改 /
            // D 删除,Move 单独标出),最多 6 行,其余折叠成计数。
            const auto headers = apply_patch::summarize_patch_headers(
                apply_patch::patch_text_from_arguments(j.dump()));
            if (!headers.empty()) {
                std::string out = "Do you want to apply a patch to " +
                                  std::to_string(headers.size()) +
                                  (headers.size() == 1 ? " file?" : " files?");
                size_t shown = 0;
                for (const auto& header : headers) {
                    if (shown >= 6) {
                        out += "\n  ... " + std::to_string(headers.size() - shown) + " more";
                        break;
                    }
                    const char* marker =
                        header.kind == apply_patch::HunkKind::Add ? "A" :
                        header.kind == apply_patch::HunkKind::Delete ? "D" : "M";
                    out += std::string("\n  ") + marker + " " + truncate_path(header.path);
                    if (!header.move_path.empty()) {
                        out += " -> " + truncate_path(header.move_path);
                    }
                    ++shown;
                }
                return out;
            }
        }
    } catch (...) {
        // 落到通用分支
    }

    return "Do you want to use " + tool_name + "?";
}

std::vector<ConfirmOption> build_confirm_options(const std::string& tool_name,
                                                 const std::string& arguments_json) {
    std::vector<ConfirmOption> options;
    std::string session_label = "Yes, allow all edits during this session (shift+tab)";
    std::string scoped_root;
    std::string remember_prefix;
    bool additional = false;
    try {
        auto j = nlohmann::json::parse(arguments_json);
        if (tool_name == "bash" && j.contains("permission") && j["permission"].is_object()) {
            const auto& permission = j["permission"];
            const auto prefix = permission.value("always_allow_prefix", std::string{});
            additional = permission.value("request", std::string{}) == "with_additional_permissions";
            if (additional) session_label = "Yes, and keep these extra permissions for this session (shift+tab)";
            else if (!prefix.empty()) session_label = "Yes, allow `" + truncate_command_line(prefix) + "` during this session (shift+tab)";
            else session_label.clear();   // 解释器 / 不透明脚本:没有可记的前缀
            scoped_root = permission.value("scoped_write_root", std::string{});
            remember_prefix = permission.value("proposed_prefix_rule", std::string{});
        }
    } catch (...) {
        // 坏 JSON:只给基础三项
    }
    int n = 1;
    auto add = [&](std::string label, PermissionResult result) {
        options.push_back({std::to_string(n++) + ". " + std::move(label), result});
    };
    add("Yes", PermissionResult::Allow);
    if (!session_label.empty()) add(session_label, PermissionResult::AlwaysAllow);
    if (!scoped_root.empty()) {
        add("Yes, but only grant write access to " + truncate_path(scoped_root) + " (stay sandboxed)",
            PermissionResult::AllowScoped);
    }
    if (!remember_prefix.empty() && !additional) {
        add("Yes, and always allow `" + truncate_command_line(remember_prefix) + "` (saved to rules file)",
            PermissionResult::AllowRemember);
    }
    add("No", PermissionResult::Deny);
    return options;
}

int confirm_default_focus(const std::string& tool_name, const std::string& arguments_json) {
    const auto options = build_confirm_options(tool_name, arguments_json);
    return options.empty() ? 0 : static_cast<int>(options.size()) - 1;
}

} // namespace acecode::tui
