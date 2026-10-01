#include "file_state_restore.hpp"
#include "session/compact_checkpoint.hpp"
#include "utils/utf8_path.hpp"
#include <filesystem>
#include <algorithm>
#include <cctype>

#include "tool/apply_patch_format.hpp"
#include "tool/mtime_tracker.hpp"
#include "utils/text_file_buffer.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acecode {

namespace {

constexpr const char* kFileUnchangedStubPrefix = "File unchanged since last read.";

struct FileToolUse {
    std::string name;
    std::string path;
    std::string content;
    bool partial_read_request = false;
    // apply_patch:补丁涉及的全部路径(含 Move 目标),已按 cwd 解析。
    std::vector<std::string> patch_paths;
};

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool looks_like_failed_tool_result(const ChatMessage& msg) {
    return starts_with(msg.content, "[Error]") ||
           starts_with(msg.content, "[Interrupted]") ||
           starts_with(msg.content, "[Doom-loop guard]");
}

std::optional<std::string> json_string_field(const nlohmann::json& object,
                                             const char* name) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_string()) {
        return std::nullopt;
    }
    return object[name].get<std::string>();
}

std::optional<FileToolUse> parse_file_tool_use(const nlohmann::json& tool_call,
                                               const std::string& cwd) {
    if (!tool_call.is_object() ||
        !tool_call.contains("function") || !tool_call["function"].is_object()) {
        return std::nullopt;
    }

    const auto& fn = tool_call["function"];
    auto name = json_string_field(fn, "name");
    auto arguments_text = json_string_field(fn, "arguments");
    if (!name || !arguments_text) return std::nullopt;
    if (*name == "apply_patch") {
        FileToolUse use;
        use.name = *name;
        use.patch_paths = apply_patch::extract_target_paths(*arguments_text, cwd);
        if (use.patch_paths.empty()) return std::nullopt;
        return use;
    }
    if (*name != "file_read" && *name != "file_write" && *name != "file_edit") {
        return std::nullopt;
    }

    nlohmann::json args = nlohmann::json::parse(*arguments_text, nullptr, false);
    auto path = json_string_field(args, "file_path");
    if (!path || path->empty()) return std::nullopt;

    FileToolUse use;
    use.name = *name;
    use.path = *path;
    if (*name == "file_read") {
        use.partial_read_request =
            args.is_object() &&
            (args.contains("start_line") ||
             args.contains("end_line") ||
             args.contains("byte_offset") ||
             args.contains("max_bytes"));
    } else if (*name == "file_write") {
        auto content = json_string_field(args, "content");
        if (!content) return std::nullopt;
        use.content = normalize_text_to_lf(*content);
    }
    return use;
}

bool read_footer_is_lossy(const std::string& content) {
    return content.find("lossy=\"true\"") != std::string::npos ||
           content.find("editable=\"false\"") != std::string::npos;
}

bool read_footer_is_partial(const std::string& content) {
    const size_t footer = content.rfind("<acecode-read-metadata");
    if (footer == std::string::npos) return false;
    return content.find("partial=\"true\"", footer) != std::string::npos ||
           content.find("truncated=\"true\"", footer) != std::string::npos;
}

std::string strip_read_metadata_footer(std::string content) {
    const std::string marker = "\n<acecode-read-metadata";
    size_t marker_pos = content.rfind(marker);
    if (marker_pos == std::string::npos && starts_with(content, "<acecode-read-metadata")) {
        marker_pos = 0;
    }
    if (marker_pos != std::string::npos) {
        content.resize(marker_pos);
    }

    const std::string large_hint_prefix = "\n[hint: file is large (";
    size_t hint_pos = content.rfind(large_hint_prefix);
    if (hint_pos != std::string::npos &&
        content.find("). Consider using start_line / end_line to narrow the read next time.]",
                     hint_pos) != std::string::npos) {
        content.resize(hint_pos);
    }
    return content;
}

std::size_t restore_file_tool_state(const FileToolUse& use, const ChatMessage& result) {
    if (looks_like_failed_tool_result(result)) return 0;

    if (use.name == "file_read") {
        if (use.partial_read_request) return 0;
        if (starts_with(result.content, kFileUnchangedStubPrefix)) return 0;
        if (read_footer_is_lossy(result.content)) return 0;
        if (read_footer_is_partial(result.content)) return 0;

        FileReadEditMetadata metadata;
        MtimeTracker::instance().seed_transcript_read_baseline(
            use.path,
            strip_read_metadata_footer(result.content),
            metadata);
        return 0;
    }

    if (use.name == "file_write") {
        FileReadEditMetadata metadata;
        MtimeTracker::instance().seed_transcript_read_baseline(
            use.path,
            use.content,
            metadata);
        return 0;
    }

    if (use.name == "file_edit") {
        auto read_result = read_text_file_buffer(use.path);
        if (read_result.success && !read_result.buffer.metadata.lossy) {
            MtimeTracker::instance().record_write(use.path, read_result.buffer.text);
        }
        return 1;
    }

    return 0;
}

} // namespace

std::size_t restore_file_tool_state_from_messages(const std::vector<ChatMessage>& messages,
                                                  const std::string& cwd) {
    std::size_t begin = 0;
    for (std::size_t i = messages.size(); i > 0; --i) {
        try {
            if (decode_compact_checkpoint(messages[i - 1])) {
                begin = i;
                break;
            }
        } catch (...) {
            // A damaged checkpoint cannot define the restored context boundary.
        }
    }
    struct Baseline {
        FileToolUse use;
        const ChatMessage* result; // Borrowed only during this synchronous call.
    };
    std::map<std::string, FileToolUse> calls;
    std::map<std::string, Baseline> baselines;
    std::map<std::string, std::string> paths;
    const auto normalize = [&](const std::string& raw) {
        if (const auto found = paths.find(raw); found != paths.end()) return found->second;
        auto path = path_from_utf8(raw);
        if (path.is_relative() && !cwd.empty()) path = path_from_utf8(cwd) / path;
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(path, error);
        auto key = path_to_utf8((error ? path : canonical).lexically_normal());
#ifdef _WIN32
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        paths.emplace(raw, key);
        return key;
    };
    for (std::size_t i = begin; i < messages.size(); ++i) {
        const auto& message = messages[i];
        if (message.role == "user" && !message.is_meta) calls.clear();
        if (message.role == "assistant" && message.tool_calls.is_array()) {
            for (const auto& call : message.tool_calls) {
                if (!call.is_object() || !call.contains("id") || !call["id"].is_string()) continue;
                if (auto use = parse_file_tool_use(call, cwd)) {
                    calls[call["id"].get<std::string>()] = std::move(*use);
                }
            }
        }
        if (message.role != "tool" || looks_like_failed_tool_result(message)) continue;
        const auto call = calls.find(message.tool_call_id);
        if (call == calls.end()) continue;
        auto use = call->second;
        calls.erase(call);
        if (use.name == "file_read" &&
            (use.partial_read_request || starts_with(message.content, kFileUnchangedStubPrefix) ||
             read_footer_is_lossy(message.content) || read_footer_is_partial(message.content))) continue;
        if (use.name == "apply_patch") {
            for (const auto& path : use.patch_paths) {
                FileToolUse edit;
                edit.name = "file_edit";
                edit.path = normalize(path);
                baselines[edit.path] = {edit, &message};
            }
        } else {
            use.path = normalize(use.path);
            const auto key = use.path;
            baselines[key] = {std::move(use), &message};
        }
    }
    std::size_t reads = 0;
    for (const auto& [path, baseline] : baselines) {
        reads += restore_file_tool_state(baseline.use, *baseline.result);
    }
    return reads;
}

} // namespace acecode
