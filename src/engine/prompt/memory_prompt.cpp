#include "memory_prompt.hpp"

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_service.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace acecode {

namespace {

struct SnapshotLine {
    std::int64_t time = 0;     // 排序用:越新越靠前
    std::string name;
    std::string text;
};

std::string one_line(std::string text) {
    for (auto& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    return text;
}

std::string entry_time(const MemoryEntry& entry) {
    if (!entry.updated_at.empty()) return entry.updated_at;
    if (!entry.created_at.empty()) return entry.created_at;
    // 旧条目没有来源字段:文件修改时间就是它最后一次被改写的时间。
    return memory_file_mtime_iso8601(entry.path);
}

// 一个作用域的索引段;没有条目返回空串。
std::string render_scope(MemoryRegistry* registry, const char* title,
                         std::size_t budget, std::int64_t now_seconds) {
    if (!registry) return {};
    registry->reload();
    std::vector<SnapshotLine> lines;
    for (const auto& entry : registry->list()) {
        const std::string when = entry_time(entry);
        SnapshotLine line;
        line.time = parse_memory_iso8601(when).value_or(0);
        line.name = entry.name;
        line.text = "- [" + memory_type_to_string(entry.type) + "] " + entry.name +
                    " \xE2\x80\x94 " + one_line(entry.description);
        const std::string age = memory_age_label(when, now_seconds);
        if (!age.empty()) line.text += " (" + age + ")";
        lines.push_back(std::move(line));
    }
    if (lines.empty()) return {};
    std::sort(lines.begin(), lines.end(), [](const SnapshotLine& a, const SnapshotLine& b) {
        return a.time != b.time ? a.time > b.time : a.name < b.name;
    });

    std::ostringstream out;
    out << "## " << title << "\n";
    std::size_t used = 0;
    std::size_t shown = 0;
    for (const auto& line : lines) {
        if (used + line.text.size() + 1 > budget) break;
        out << line.text << "\n";
        used += line.text.size() + 1;
        ++shown;
    }
    if (shown < lines.size()) {
        const std::size_t omitted = lines.size() - shown;
        out << "(" << omitted << (omitted == 1 ? " older entry was" : " older entries were")
            << " omitted to stay within the memory budget; call memory_read to list or search them.)\n";
    }
    return out.str();
}

} // namespace

std::string memory_age_label(const std::string& iso_time, std::int64_t now_seconds) {
    const auto when = parse_memory_iso8601(iso_time);
    if (!when || now_seconds <= 0) return {};
    const std::int64_t diff = now_seconds - *when;
    const std::int64_t days = diff > 0 ? diff / 86400 : 0;
    if (days == 0) return "today";
    if (days == 1) return "1 day ago";
    return std::to_string(days) + " days ago";
}

PromptContextBlock build_memory_snapshot_prompt(const MemorySnapshotSource& source) {
    PromptContextBlock block;
    if (!source.memory) return block;
    const std::size_t budget = source.max_index_bytes > 0 ? source.max_index_bytes : 8 * 1024;
    const std::string global = render_scope(&source.memory->global(), "Global memory",
                                            budget, source.now_seconds);
    const auto workspace_registry = source.memory->workspace(source.project_dir);
    const std::string workspace = render_scope(workspace_registry.get(),
                                               "Workspace memory", budget, source.now_seconds);
    if (global.empty() && workspace.empty()) return block;

    std::ostringstream oss;
    oss << "# Memory\n\n"
        << "Persistent memory saved in earlier sessions. Memories are records of the past, "
        << "not live state: before relying on a file path, command or repository fact from a "
        << "memory, verify it with tools. When a memory conflicts with the user's current "
        << "instructions, follow the current instructions. Load an entry's full body with "
        << "memory_read(name=...); memory_read(query=...) also searches entries omitted below.\n\n";
    if (!global.empty()) oss << global << "\n";
    if (!workspace.empty()) oss << workspace << "\n";
    block.content = oss.str();
    while (!block.content.empty() && block.content.back() == '\n') block.content.pop_back();
    block.content.push_back('\n');
    block.cache_key = "memory:" + prompt_component_hash(block.content);
    return block;
}

void append_memory_tool_guidance(std::ostream& oss) {
    // 「记住」要落到会被自动注入的地方。反馈 LINDANDAN069:用户让模型记住做法,
    // 模型把经验写进了子目录里的 CLAUDE.md / .acecode/MEMORY.md,ACECode 从不
    // 自动加载这些文件,下一轮压缩之后经验就「忘」了。
    oss << "# Memory\n\n"
        << "- When the user asks you to remember something for later (\"remember this\", "
        << "\"next time do X\", \"don't repeat this mistake\"), save it with `memory_write`. "
        << "Saved memories are shown to future sessions and survive context compaction. Use "
        << "the global scope for the user's personal preferences and the workspace scope for "
        << "knowledge about this workspace.\n"
        << "- Do not substitute ad-hoc notes files (MEMORY.md, LESSONS_LEARNED.md, or a "
        << "CLAUDE.md/AGENTS.md in a subdirectory): they are not loaded automatically, so the "
        << "lesson is lost once it leaves the context. Project instruction files are loaded "
        << "only from the working directory and its parent directories.\n"
        << "- Put the actionable rule in the memory description (what to do, what to reuse "
        << "and where it lives) so it is visible without opening the entry, and call "
        << "`memory_read` for the details before starting related work.\n\n";
}

} // namespace acecode
