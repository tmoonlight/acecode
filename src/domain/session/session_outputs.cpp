#include "session_outputs.hpp"

#include "llm/message_predicates.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace acecode {

namespace {

// 与 Claude 桌面版 Outputs 面板同一批扩展名,另加 svg(脚本画的图表常用)。
constexpr std::array<std::string_view, 27> kDeliverableExtensions = {
    "md", "markdown", "txt", "rtf", "pdf", "doc", "docx", "xls", "xlsx",
    "ppt", "pptx", "csv", "tsv", "html", "htm", "png", "jpg", "jpeg", "gif",
    "webp", "svg", "mp4", "mov", "webm", "mp3", "wav", "zip",
};

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_separator(char c) { return c == '/' || c == '\\'; }

bool has_drive_prefix(std::string_view path) {
    return path.size() >= 2 && path[1] == ':' &&
           ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z'));
}

// 比较用的键:分隔符统一成 /,去掉结尾分隔符;带盘符的路径不分大小写。
// 只改 ASCII 字母,所以键与原路径逐字节等长,可以按同一个偏移切相对路径。
std::string path_key(std::string_view path) {
    std::string key(path);
    const bool fold_case = has_drive_prefix(path);
    for (char& c : key) {
        if (c == '\\') c = '/';
        else if (fold_case) c = ascii_lower(c);
    }
    while (key.size() > 1 && key.back() == '/' && !(key.size() == 3 && key[1] == ':')) key.pop_back();
    return key;
}

bool key_is_inside(const std::string& key, const std::string& folder_key) {
    if (folder_key.empty() || key.size() <= folder_key.size()) return false;
    if (key.compare(0, folder_key.size(), folder_key) != 0) return false;
    return folder_key.back() == '/' || key[folder_key.size()] == '/';
}

bool has_hidden_state_segment(std::string_view rest) {
    std::size_t start = 0;
    while (start <= rest.size()) {
        std::size_t end = rest.find('/', start);
        if (end == std::string_view::npos) end = rest.size();
        const std::string_view segment = rest.substr(start, end - start);
        if (segment == ".acecode" || segment == ".git") return true;
        start = end + 1;
    }
    return false;
}

void append_string_array(const nlohmann::json& value, SessionOutputSource source,
                         std::vector<SessionOutputCandidate>& out) {
    if (!value.is_array()) return;
    for (const auto& item : value) {
        if (!item.is_string()) continue;
        std::string path = item.get<std::string>();
        if (!path.empty()) out.push_back({std::move(path), source});
    }
}

} // namespace

const char* session_output_source_name(SessionOutputSource source) {
    switch (source) {
        case SessionOutputSource::Write: return "write";
        case SessionOutputSource::Patch: return "patch";
        case SessionOutputSource::Shell: return "shell";
    }
    return "write";
}

bool is_deliverable_file_name(std::string_view path) {
    std::size_t name_start = path.size();
    while (name_start > 0 && !is_separator(path[name_start - 1])) --name_start;
    const std::string_view name = path.substr(name_start);
    const std::size_t dot = name.rfind('.');
    // 没有扩展名,或整个名字就是「.md」这种点文件,都不算。
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) return false;
    std::string extension(name.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(), ascii_lower);
    return std::find(kDeliverableExtensions.begin(), kDeliverableExtensions.end(),
                     std::string_view(extension)) != kDeliverableExtensions.end();
}

bool jsonl_line_may_carry_outputs(std::string_view line) {
    // session_serializer 用 dump(-1) 落盘,键值之间没有空格;正文里出现的同样
    // 字样在 JSON 字符串里引号会带反斜杠,不会误中。
    static constexpr std::array<std::string_view, 5> kMarkers = {
        "\"output_files\"", "\"subagent_session_id\"", "\"verb\":\"Created\"",
        "\"verb\":\"Wrote\"", "\"type\":\"add\"",
    };
    for (const auto marker : kMarkers) {
        if (line.find(marker) != std::string_view::npos) return true;
    }
    return false;
}

std::vector<SessionOutputCandidate> session_output_candidates(const ChatMessage& message) {
    std::vector<SessionOutputCandidate> out;
    if (!message.metadata.is_object()) return out;
    const auto& meta = message.metadata;

    if (message.role == "user") {
        // 后台命令的完成通知。
        const auto task = meta.find(kBackgroundTaskMetadataKey);
        if (task != meta.end() && task->is_object() && task->contains("output_files")) {
            append_string_array((*task)["output_files"], SessionOutputSource::Shell, out);
        }
        return out;
    }
    if (message.role != "tool") return out;

    // bash:命令失败(退出码非零)也可能已经生成了文件,不看成败。
    if (meta.contains("output_files")) {
        append_string_array(meta["output_files"], SessionOutputSource::Shell, out);
    }

    const auto success = meta.find("tool_success");
    if (success != meta.end() && success->is_boolean() && !success->get<bool>()) return out;

    // apply_patch:只算新建。改已有文件相当于编辑,归「变更」。
    const auto files = meta.find("files");
    if (files != meta.end() && files->is_array()) {
        for (const auto& entry : *files) {
            if (!entry.is_object()) continue;
            const auto type = entry.find("type");
            const auto path = entry.find("path");
            if (type == entry.end() || !type->is_string() || type->get<std::string>() != "add") continue;
            if (path == entry.end() || !path->is_string() || path->get<std::string>().empty()) continue;
            out.push_back({path->get<std::string>(), SessionOutputSource::Patch});
        }
        return out;
    }

    // file_write:新建是 Created,覆写是 Wrote,都算。
    const auto summary = meta.find("tool_summary");
    if (summary != meta.end() && summary->is_object()) {
        const auto verb = summary->find("verb");
        const auto object = summary->find("object");
        if (verb != summary->end() && verb->is_string() &&
            object != summary->end() && object->is_string()) {
            const std::string verb_text = verb->get<std::string>();
            std::string path = object->get<std::string>();
            if ((verb_text == "Created" || verb_text == "Wrote") && !path.empty()) {
                out.push_back({std::move(path), SessionOutputSource::Write});
            }
        }
    }
    return out;
}

std::string session_output_child_session_id(const ChatMessage& message) {
    if (message.role != "tool" || !message.metadata.is_object()) return {};
    const auto id = message.metadata.find("subagent_session_id");
    if (id == message.metadata.end() || !id->is_string()) return {};
    return id->get<std::string>();
}

std::vector<SessionOutputEntry> select_session_outputs(
    const std::vector<SessionOutputCandidate>& candidates,
    const SessionOutputRoots& roots) {
    // 与 roots.folders 逐项对应(空串占位),下标要原样交回调用方。
    std::vector<std::string> folder_keys;
    for (const auto& folder : roots.folders) {
        folder_keys.push_back(folder.empty() ? std::string{} : path_key(folder));
    }
    std::vector<std::string> temp_keys;
    for (const auto& temp : roots.temp_roots) {
        if (!temp.empty()) temp_keys.push_back(path_key(temp));
    }

    std::vector<SessionOutputEntry> out;
    std::unordered_set<std::string> seen;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (it->path.empty()) continue;
        const std::string key = path_key(it->path);
        if (!seen.insert(key).second) continue;

        // 嵌套的会话文件夹(仓库根 + 它下面的 worktree)取最长的那个。
        const std::string* folder = nullptr;
        int folder_index = -1;
        for (std::size_t i = 0; i < folder_keys.size(); ++i) {
            const std::string& candidate = folder_keys[i];
            if (!key_is_inside(key, candidate)) continue;
            if (!folder || candidate.size() > folder->size()) {
                folder = &candidate;
                folder_index = static_cast<int>(i);
            }
        }

        SessionOutputEntry entry;
        entry.path = it->path;
        entry.source = it->source;
        entry.folder_index = folder_index;
        if (folder) {
            const std::size_t offset = folder->size() + (folder->back() == '/' ? 0 : 1);
            if (has_hidden_state_segment(std::string_view(key).substr(offset))) continue;
            if (!is_deliverable_file_name(key)) continue;
            entry.relative = it->path.substr(offset);
            std::replace(entry.relative.begin(), entry.relative.end(), '\\', '/');
        } else {
            if (has_hidden_state_segment(key)) continue;
            const bool in_temp = std::any_of(temp_keys.begin(), temp_keys.end(),
                [&key](const std::string& temp) { return key_is_inside(key, temp); });
            if (in_temp) continue;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace acecode
