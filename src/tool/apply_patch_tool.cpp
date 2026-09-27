#include "apply_patch_tool.hpp"
#include "config/mcp_config.hpp"

#include "apply_patch_format.hpp"
#include "utils/diff_utils.hpp"
#include "lsp/lsp_diagnostics.hpp"
#include "mtime_tracker.hpp"
#include "llm/tool_icons.hpp"
#include "tool/file_operations.hpp"
#include "utils/logger.hpp"
#include "tool/text_file_errors.hpp"
#include "tool/safe_text_write.hpp"
#include "tool/tool_errors.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace acecode {

namespace {

namespace fs = std::filesystem;

constexpr const char* kVerificationPrefix = "[Error] apply_patch verification failed: ";

// 一条已校验、待落盘的文件操作。old_content / new_content 都是 LF 文本。
struct PlannedChange {
    apply_patch::HunkKind kind = apply_patch::HunkKind::Update;
    std::string display_path;   // 补丁里写的原始路径(给模型看)
    std::string display_move_path;
    std::string path;           // 解析后的绝对路径(源文件)
    std::string move_path;      // Move 目标(解析后);空 = 非 Move
    std::string old_content;
    std::string new_content;
    TextFileMetadata metadata;
    bool file_existed = false;
};

// 同一份补丁里前面的操作对后面操作可见:两段 Update 同一个文件时,第二段
// 必须基于第一段的结果匹配,否则永远失败;Delete 之后再 Add 同名文件也要允许。
struct OverlayEntry {
    bool exists = false;
    // false = 文件存在但不是可编辑文本(二进制 / 不支持的编码 / 过大)。
    // Delete 仍可执行,Update / Add 覆盖则拒绝。
    bool decodable = true;
    std::string text;
    TextFileMetadata metadata;
};

OverlayEntry present_entry(const std::string& text, const TextFileMetadata& metadata) {
    OverlayEntry entry;
    entry.exists = true;
    entry.text = text;
    entry.metadata = metadata;
    return entry;
}

OverlayEntry absent_entry() {
    OverlayEntry entry;
    entry.exists = false;
    entry.metadata = default_new_file_text_metadata();
    return entry;
}

std::string resolve_hunk_path(const ToolContext& ctx,
                              const std::string& raw_path,
                              std::string* error) {
    const auto resolved = ctx.resolve_scratch_path_alias(raw_path);
    if (!resolved.success) {
        if (error) *error = resolved.error;
        return {};
    }
    const std::string cwd = ctx.cwd.empty() ? current_path_utf8() : ctx.cwd;
    return apply_patch::resolve_patch_path(resolved.used_alias ? resolved.path : raw_path,
                                          cwd);
}

bool is_blank(const std::string& text) {
    for (unsigned char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

ToolResult verification_error(const std::string& detail) {
    return ToolResult{kVerificationPrefix + detail, false};
}

struct DiffCounts {
    int additions = 0;
    int deletions = 0;
};

DiffCounts count_hunk_lines(const std::vector<DiffHunk>& hunks) {
    DiffCounts counts;
    for (const auto& hunk : hunks) {
        for (const auto& line : hunk.lines) {
            if (line.kind == DiffLineKind::Added) ++counts.additions;
            else if (line.kind == DiffLineKind::Removed) ++counts.deletions;
        }
    }
    return counts;
}

const char* change_type_label(const PlannedChange& change) {
    switch (change.kind) {
        case apply_patch::HunkKind::Add: return "add";
        case apply_patch::HunkKind::Delete: return "delete";
        case apply_patch::HunkKind::Update:
            return change.move_path.empty() ? "update" : "move";
    }
    return "update";
}

ToolResult execute_apply_patch(const std::string& arguments_json, const ToolContext& ctx) {
    const auto raw_args = nlohmann::json::parse(arguments_json, nullptr, false);
    if (!raw_args.is_object()) {
        return ToolResult{ToolErrors::parse_failed(), false};
    }
    const std::string patch_text = apply_patch::patch_text_from_arguments(arguments_json);
    if (patch_text.empty()) {
        return ToolResult{ToolErrors::missing_parameter("input"), false};
    }

    const apply_patch::ParseResult parsed = apply_patch::parse_patch(patch_text);
    if (!parsed.success) {
        return verification_error(parsed.error);
    }
    if (parsed.patch.hunks.empty()) {
        return verification_error(
            "empty patch (no *** Add File / *** Update File / *** Delete File sections)");
    }

    LOG_DEBUG("apply_patch: hunks=" + std::to_string(parsed.patch.hunks.size()) +
              " bytes=" + std::to_string(patch_text.size()));

    // ---- 阶段一:全量校验,不碰文件系统的写侧 ----
    std::vector<PlannedChange> plan;
    std::map<std::string, OverlayEntry> overlay;

    // for_delete:删除不要求文件可解码(二进制 / 过大照删),只要求它存在且是
    // 普通文件;Update / Add 覆盖则必须能安全解码。
    auto current_state = [&](const std::string& path, std::string* error,
                             bool for_delete) -> std::optional<OverlayEntry> {
        const auto it = overlay.find(path);
        if (it != overlay.end()) {
            if (it->second.exists && !it->second.decodable && !for_delete) {
                if (error) *error = ToolErrors::file_read_not_safe_for_edit(path);
                return std::nullopt;
            }
            return it->second;
        }
        OverlayEntry entry;
        std::error_code ec;
        const fs::path native = path_from_utf8(path);
        if (!fs::exists(native, ec)) {
            entry.exists = false;
            entry.metadata = default_new_file_text_metadata();
            return entry;
        }
        if (!fs::is_regular_file(native, ec)) {
            if (error) *error = ToolErrors::path_not_regular_file(path);
            return std::nullopt;
        }
        entry.exists = true;
        const auto size_check = FileOperations::check_edit_file_size(
            path, "Large-file patching is not supported.");
        if (!size_check.success) {
            if (for_delete) {
                entry.decodable = false;
                return entry;
            }
            if (error) *error = size_check.output;
            return std::nullopt;
        }
        auto read = with_text_file_tool_errors(read_text_file_buffer(path, false));
        if (!read.success) {
            if (for_delete) {
                entry.decodable = false;
                return entry;
            }
            if (read.buffer.metadata.lossy) {
                if (error) *error = ToolErrors::file_read_not_safe_for_edit(path);
            } else if (error) {
                *error = read.error;
            }
            return std::nullopt;
        }
        entry.text = read.buffer.text;
        entry.metadata = read.buffer.metadata;
        return entry;
    };

    for (const auto& hunk : parsed.patch.hunks) {
        std::string path_error;
        const std::string path = resolve_hunk_path(ctx, hunk.path, &path_error);
        if (path.empty()) {
            return ToolResult{path_error.empty()
                                  ? kVerificationPrefix + std::string("invalid path: ") + hunk.path
                                  : path_error,
                              false};
        }

        PlannedChange change;
        change.kind = hunk.kind;
        change.display_path = hunk.path;
        change.path = path;

        switch (hunk.kind) {
            case apply_patch::HunkKind::Add: {
                std::string error;
                const auto state = current_state(path, &error, /*for_delete=*/false);
                if (!state) {
                    // 已存在但读不出来(目录 / 二进制 / 过大):一律拒绝新建覆盖。
                    return ToolResult{error, false};
                }
                if (state->exists && !is_blank(state->text)) {
                    return verification_error(
                        "file already exists and is not empty: " + hunk.path +
                        ". Use '*** Update File: " + hunk.path +
                        "' to change it, or '*** Delete File:' it first.");
                }
                change.file_existed = state->exists;
                change.old_content = state->text;
                change.new_content = normalize_text_to_lf(hunk.contents);
                change.metadata = state->exists ? state->metadata
                                                : default_new_file_text_metadata();
                overlay[path] = present_entry(change.new_content, change.metadata);
                break;
            }
            case apply_patch::HunkKind::Delete: {
                std::string error;
                const auto state = current_state(path, &error, /*for_delete=*/true);
                if (!state) return ToolResult{error, false};
                if (!state->exists) {
                    return verification_error("failed to read file to delete: " + hunk.path);
                }
                change.file_existed = true;
                change.old_content = state->text;
                change.metadata = state->metadata;
                overlay[path] = absent_entry();
                break;
            }
            case apply_patch::HunkKind::Update: {
                std::string error;
                const auto state = current_state(path, &error, /*for_delete=*/false);
                if (!state) return ToolResult{error, false};
                if (!state->exists) {
                    return verification_error("failed to read file to update: " + hunk.path);
                }
                change.display_move_path = hunk.move_path;
                const auto derived = apply_patch::derive_new_contents(
                    hunk.path, hunk.chunks, state->text);
                if (!derived.success) {
                    return verification_error(derived.error);
                }
                change.file_existed = true;
                change.old_content = state->text;
                change.new_content = derived.content;
                change.metadata = state->metadata;
                if (!hunk.move_path.empty()) {
                    std::string move_error;
                    const std::string target = resolve_hunk_path(ctx, hunk.move_path, &move_error);
                    if (target.empty()) {
                        return ToolResult{move_error.empty()
                                              ? kVerificationPrefix + std::string("invalid move target: ") +
                                                    hunk.move_path
                                              : move_error,
                                          false};
                    }
                    if (target != path) {
                        std::string target_error;
                        const auto target_state =
                            current_state(target, &target_error, /*for_delete=*/true);
                        if (!target_state) return ToolResult{target_error, false};
                        if (target_state->exists) {
                            return verification_error(
                                "move target already exists: " + hunk.move_path);
                        }
                        change.move_path = target;
                        overlay[path] = absent_entry();
                        overlay[target] = present_entry(change.new_content, change.metadata);
                    } else {
                        overlay[path] = present_entry(change.new_content, change.metadata);
                    }
                } else {
                    overlay[path] = present_entry(change.new_content, change.metadata);
                }
                break;
            }
        }
        plan.push_back(std::move(change));
    }

    // Validate all MCP targets before the first file in this patch changes.
    try {
        for (const auto& change : plan) {
            if (change.kind == apply_patch::HunkKind::Delete) continue;
            (void)validate_mcp_file_edit(
                change.move_path.empty() ? change.path : change.move_path,
                change.new_content);
        }
    } catch (const std::exception& error) {
        return ToolResult{error.what(), false};
    }

    // ---- 阶段二:按补丁顺序落盘 ----
    auto before_write = [&](const std::string& path) {
        if (!ctx.track_file_write_before) return;
        try {
            ctx.track_file_write_before(path);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("apply_patch checkpoint hook failed: ") + e.what());
        } catch (...) {
            LOG_WARN("apply_patch checkpoint hook failed with unknown error");
        }
    };

    std::vector<std::string> applied_lines;
    for (const auto& change : plan) {
        auto guard = MtimeTracker::instance().acquire_write_guard(change.path);
        std::string failure;
        switch (change.kind) {
            case apply_patch::HunkKind::Add:
            case apply_patch::HunkKind::Update: {
                const std::string target = change.move_path.empty() ? change.path : change.move_path;
                if (!change.move_path.empty()) before_write(change.path);
                const auto written = safe_write_text_file(
                    target, change.new_content, change.metadata, before_write);
                if (!written.success) {
                    failure = written.error;
                    break;
                }
                MtimeTracker::instance().record_write(target, change.new_content);
                if (!change.move_path.empty()) {
                    std::error_code ec;
                    fs::remove(path_from_utf8(change.path), ec);
                    if (ec) {
                        failure = "[Error] Moved content was written to " + change.move_path +
                                  " but the original could not be removed: " + ec.message();
                        break;
                    }
                    MtimeTracker::instance().invalidate_agent_read_state(change.path);
                }
                break;
            }
            case apply_patch::HunkKind::Delete: {
                before_write(change.path);
                std::error_code ec;
                fs::remove(path_from_utf8(change.path), ec);
                if (ec) {
                    failure = "[Error] Cannot delete file: " + change.path + ": " + ec.message();
                    break;
                }
                MtimeTracker::instance().invalidate_agent_read_state(change.path);
                break;
            }
        }
        if (!failure.empty()) {
            std::ostringstream oss;
            oss << failure << "\nThe patch was applied partially. Files already changed:";
            if (applied_lines.empty()) oss << " (none)";
            for (const auto& line : applied_lines) oss << "\n" << line;
            oss << "\nRe-read the affected files before retrying.";
            return ToolResult{oss.str(), false};
        }
        switch (change.kind) {
            case apply_patch::HunkKind::Add:
                applied_lines.push_back("A " + change.display_path);
                break;
            case apply_patch::HunkKind::Delete:
                applied_lines.push_back("D " + change.display_path);
                break;
            case apply_patch::HunkKind::Update:
                if (change.move_path.empty()) {
                    applied_lines.push_back("M " + change.display_path);
                } else {
                    applied_lines.push_back("M " + change.display_move_path +
                                            " (from " + change.display_path + ")");
                }
                break;
        }
    }

    // ---- 结果:摘要 / hunks / metadata ----
    std::vector<DiffHunk> all_hunks;
    nlohmann::json files = nlohmann::json::array();
    DiffCounts totals;
    bool all_scratch = true;
    for (const auto& change : plan) {
        const std::string final_path = change.move_path.empty() ? change.path : change.move_path;
        auto hunks = generate_structured_diff(change.old_content, change.new_content, final_path);
        const DiffCounts counts = count_hunk_lines(hunks);
        totals.additions += counts.additions;
        totals.deletions += counts.deletions;
        for (auto& hunk : hunks) hunk.file = final_path;
        all_hunks.insert(all_hunks.end(), hunks.begin(), hunks.end());

        nlohmann::json entry;
        entry["path"] = final_path;
        entry["type"] = change_type_label(change);
        if (!change.move_path.empty()) entry["move_path"] = change.move_path;
        if (!change.move_path.empty()) entry["from_path"] = change.path;
        entry["additions"] = counts.additions;
        entry["deletions"] = counts.deletions;
        files.push_back(std::move(entry));

        if (!ctx.is_workspace_scratch_path(final_path)) all_scratch = false;
    }

    std::ostringstream output;
    output << "Success. Updated the following files:";
    for (const auto& line : applied_lines) output << "\n" << line;
    std::string output_text = output.str();

    for (const auto& change : plan) {
        if (change.kind == apply_patch::HunkKind::Delete) continue;
        const std::string final_path = change.move_path.empty() ? change.path : change.move_path;
        // LSP 编辑后诊断(openspec add-lsp-service);无匹配 server 零开销。
        lsp::append_diagnostics_block(output_text, final_path, ctx.abort_flag, ctx.cwd);
    }

    ToolSummary summary;
    if (plan.size() == 1) {
        const PlannedChange& only = plan.front();
        switch (only.kind) {
            case apply_patch::HunkKind::Add: summary.verb = "Created"; break;
            case apply_patch::HunkKind::Delete: summary.verb = "Deleted"; break;
            case apply_patch::HunkKind::Update: summary.verb = "Edited"; break;
        }
        summary.object = only.move_path.empty() ? only.path : only.move_path;
    } else {
        summary.verb = "Patched";
        summary.object = std::to_string(plan.size()) + " files";
    }
    summary.metrics.emplace_back("+", std::to_string(totals.additions));
    summary.metrics.emplace_back("-", std::to_string(totals.deletions));
    summary.icon = tool_icon("apply_patch");

    ToolResult result{output_text, true};
    result.summary = std::move(summary);
    result.hunks = std::move(all_hunks);
    result.metadata["files"] = std::move(files);
    if (all_scratch && !plan.empty()) {
        result.metadata[kExcludeFromTurnChangeSummaryMetadata] = true;
    }
    return result;
}

} // namespace

ToolImpl create_apply_patch_tool() {
    ToolDef def;
    def.name = "apply_patch";
    def.description =
        "Use the `apply_patch` tool to edit files. Your patch language is a stripped-down, "
        "file-oriented diff format designed to be easy to parse and safe to apply. You can "
        "think of it as a high-level envelope:\n\n"
        "*** Begin Patch\n"
        "[ one or more file sections ]\n"
        "*** End Patch\n\n"
        "Within that envelope, you get a sequence of file operations. You MUST include a "
        "header to specify the action you are taking. Each operation starts with one of "
        "three headers:\n\n"
        "*** Add File: <path> - create a new file. Every following line is a + line (the "
        "initial contents).\n"
        "*** Delete File: <path> - remove an existing file. Nothing follows.\n"
        "*** Update File: <path> - patch an existing file in place (optionally with a "
        "rename).\n\n"
        "May be immediately followed by *** Move to: <new path> if you want to rename the "
        "file. Then one or more hunks, each introduced by @@ (optionally followed by a hunk "
        "header). Within a hunk each line starts with:\n"
        "- ' ' (space) for context lines that stay unchanged\n"
        "- '-' for lines to remove\n"
        "- '+' for lines to add\n\n"
        "For instructions on [context_before] and [context_after]:\n"
        "- By default, show 3 lines of code immediately above and 3 lines immediately below "
        "each change. If a change is within 3 lines of a previous change, do NOT duplicate "
        "the first change's [context_after] lines in the second change's [context_before] "
        "lines.\n"
        "- If 3 lines of context is insufficient to uniquely identify the snippet of code "
        "within the file, use the @@ operator to indicate the class or function to which "
        "the snippet belongs. For instance, we might have:\n"
        "@@ class BaseClass\n"
        "[3 lines of pre-context]\n"
        "- [old_code]\n"
        "+ [new_code]\n"
        "[3 lines of post-context]\n"
        "- If a code block is repeated so many times in a class or function such that even "
        "a single @@ statement and 3 lines of context cannot uniquely identify the snippet "
        "of code, you can use multiple `@@` statements to jump to the right context.\n"
        "- Use `*** End of File` after a hunk to anchor it at the end of the file.\n\n"
        "Line numbers are never used in this diff format; context lines identify the "
        "location. Example:\n\n"
        "*** Begin Patch\n"
        "*** Add File: hello.txt\n"
        "+Hello world\n"
        "*** Update File: src/app.py\n"
        "*** Move to: src/main.py\n"
        "@@ def greet():\n"
        "-    print(\"Hi\")\n"
        "+    print(\"Hello, world!\")\n"
        "*** Delete File: obsolete.txt\n"
        "*** End Patch\n\n"
        "Paths may be relative to the working directory or absolute. Context lines must "
        "match the current file content (exactly, or modulo leading/trailing whitespace). "
        "Every operation in the patch is verified before any file is written; if one "
        "section fails, nothing is changed and the error names the failing section. "
        "'*** Add File:' refuses to overwrite a non-empty existing file. Existing files keep "
        "their encoding and line endings; new files are UTF-8 with LF.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"input", {
                {"type", "string"},
                {"description", "The entire patch, from *** Begin Patch to *** End Patch"}
            }}
        }},
        {"required", nlohmann::json::array({"input"})}
    });

    return ToolImpl{def, execute_apply_patch, /*is_read_only=*/false};
}

} // namespace acecode
