#include "tool_preamble.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace acecode::tool_preamble {

namespace {

// 与 llm/text_preamble_tags.cpp 里的同名 helper 各留一份:只有这三个 5 行以内的字符串小函数,
// 不值得为它们开一个跨模块的工具头。
bool is_space_byte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_space_byte(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_space_byte(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

}  // namespace

std::string extract_first_bold_span(const std::string& text) {
    std::size_t i = 0;
    while (i + 1 < text.size()) {
        if (text[i] == '*' && text[i + 1] == '*') {
            const std::size_t start = i + 2;
            std::size_t j = start;
            bool closed = false;
            while (j + 1 < text.size()) {
                if (text[j] == '*' && text[j + 1] == '*') {
                    closed = true;
                    break;
                }
                ++j;
            }
            if (!closed) return {};
            const std::string inner = trim(std::string_view(text).substr(start, j - start));
            if (!inner.empty()) return inner;
            i = j + 2;
            continue;
        }
        ++i;
    }
    return {};
}

// ---- 具体进度提示:按工具拼现在进行时文案 ----

namespace {

struct ActivityCategory {
    const char* key;
    const char* phrase;         // 单个调用的动作短语(不带「正在」)
    const char* counted_phrase; // 同类多个调用时带数量的短语,"%d" 换成数量;空 = 不带数量
    const char* after;          // 这类工具跑完、模型在想下一步时的完整文案
    const char* kind;           // read / write / ""
};

// 顺序即查表顺序;最后一项是兜底(MCP 与没列出的工具)。
constexpr ActivityCategory kCategories[] = {
    {"read",       "\xE8\xAF\xBB\xE5\x8F\x96\xE6\x96\x87\xE4\xBB\xB6" /* 读取文件 */,
                   "\xE8\xAF\xBB\xE5\x8F\x96 %d \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6" /* 读取 %d 个文件 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE6\x96\x87\xE4\xBB\xB6\xE5\x86\x85\xE5\xAE\xB9" /* 正在分析文件内容 */,
                   kKindRead},
    {"search",     "\xE6\x90\x9C\xE7\xB4\xA2\xE4\xBB\xA3\xE7\xA0\x81" /* 搜索代码 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE6\x90\x9C\xE7\xB4\xA2\xE7\xBB\x93\xE6\x9E\x9C" /* 正在分析搜索结果 */,
                   kKindRead},
    {"find",       "\xE6\x9F\xA5\xE6\x89\xBE\xE6\x96\x87\xE4\xBB\xB6" /* 查找文件 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE6\x90\x9C\xE7\xB4\xA2\xE7\xBB\x93\xE6\x9E\x9C" /* 正在分析搜索结果 */,
                   kKindRead},
    {"command",    "\xE8\xBF\x90\xE8\xA1\x8C\xE5\x91\xBD\xE4\xBB\xA4" /* 运行命令 */,
                   "\xE8\xBF\x90\xE8\xA1\x8C %d \xE6\x9D\xA1\xE5\x91\xBD\xE4\xBB\xA4" /* 运行 %d 条命令 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE5\x91\xBD\xE4\xBB\xA4\xE8\xBE\x93\xE5\x87\xBA" /* 正在分析命令输出 */,
                   ""},
    {"edit",       "\xE4\xBF\xAE\xE6\x94\xB9\xE6\x96\x87\xE4\xBB\xB6" /* 修改文件 */,
                   "\xE4\xBF\xAE\xE6\x94\xB9 %d \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6" /* 修改 %d 个文件 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xA3\x80\xE6\x9F\xA5\xE4\xBF\xAE\xE6\x94\xB9\xE7\xBB\x93\xE6\x9E\x9C" /* 正在检查修改结果 */,
                   kKindWrite},
    {"write",      "\xE5\x86\x99\xE5\x85\xA5\xE6\x96\x87\xE4\xBB\xB6" /* 写入文件 */,
                   "\xE5\x86\x99\xE5\x85\xA5 %d \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6" /* 写入 %d 个文件 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xA3\x80\xE6\x9F\xA5\xE4\xBF\xAE\xE6\x94\xB9\xE7\xBB\x93\xE6\x9E\x9C" /* 正在检查修改结果 */,
                   kKindWrite},
    {"web_search", "\xE6\x90\x9C\xE7\xB4\xA2\xE7\xBD\x91\xE9\xA1\xB5" /* 搜索网页 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE6\x90\x9C\xE7\xB4\xA2\xE7\xBB\x93\xE6\x9E\x9C" /* 正在分析搜索结果 */,
                   kKindRead},
    {"web_fetch",  "\xE8\xAF\xBB\xE5\x8F\x96\xE7\xBD\x91\xE9\xA1\xB5" /* 读取网页 */,
                   "\xE8\xAF\xBB\xE5\x8F\x96 %d \xE4\xB8\xAA\xE7\xBD\x91\xE9\xA1\xB5" /* 读取 %d 个网页 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE7\xBD\x91\xE9\xA1\xB5\xE5\x86\x85\xE5\xAE\xB9" /* 正在分析网页内容 */,
                   kKindRead},
    {"browser",    "\xE6\x93\x8D\xE4\xBD\x9C\xE6\xB5\x8F\xE8\xA7\x88\xE5\x99\xA8" /* 操作浏览器 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE9\xA1\xB5\xE9\x9D\xA2\xE5\x86\x85\xE5\xAE\xB9" /* 正在分析页面内容 */,
                   ""},
    {"computer",   "\xE6\x93\x8D\xE4\xBD\x9C\xE7\x94\xB5\xE8\x84\x91" /* 操作电脑 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE5\xB1\x8F\xE5\xB9\x95\xE5\x86\x85\xE5\xAE\xB9" /* 正在分析屏幕内容 */,
                   ""},
    {"spawn",      "\xE6\xB4\xBE\xE5\x8F\x91\xE5\xAD\x90\xE4\xBB\xBB\xE5\x8A\xA1" /* 派发子任务 */,
                   "\xE6\xB4\xBE\xE5\x8F\x91 %d \xE4\xB8\xAA\xE5\xAD\x90\xE4\xBB\xBB\xE5\x8A\xA1" /* 派发 %d 个子任务 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x95\xB4\xE7\x90\x86\xE5\xAD\x90\xE4\xBB\xBB\xE5\x8A\xA1\xE7\xBB\x93\xE6\x9E\x9C" /* 正在整理子任务结果 */,
                   ""},
    {"wait",       "\xE7\xAD\x89\xE5\xBE\x85\xE5\xAD\x90\xE4\xBB\xBB\xE5\x8A\xA1\xE5\xAE\x8C\xE6\x88\x90" /* 等待子任务完成 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x95\xB4\xE7\x90\x86\xE5\xAD\x90\xE4\xBB\xBB\xE5\x8A\xA1\xE7\xBB\x93\xE6\x9E\x9C" /* 正在整理子任务结果 */,
                   ""},
    {"memory_read", "\xE8\xAF\xBB\xE5\x8F\x96\xE8\xAE\xB0\xE5\xBF\x86" /* 读取记忆 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   kKindRead},
    {"memory_write", "\xE6\x9B\xB4\xE6\x96\xB0\xE8\xAE\xB0\xE5\xBF\x86" /* 更新记忆 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   kKindWrite},
    {"skill",      "\xE5\x8A\xA0\xE8\xBD\xBD\xE6\x8A\x80\xE8\x83\xBD" /* 加载技能 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE9\x98\x85\xE8\xAF\xBB\xE6\x8A\x80\xE8\x83\xBD\xE8\xAF\xB4\xE6\x98\x8E" /* 正在阅读技能说明 */,
                   kKindRead},
    {"todo",       "\xE6\x9B\xB4\xE6\x96\xB0\xE5\xBE\x85\xE5\x8A\x9E\xE6\xB8\x85\xE5\x8D\x95" /* 更新待办清单 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   ""},
    {"image_gen",  "\xE7\x94\x9F\xE6\x88\x90\xE5\x9B\xBE\xE7\x89\x87" /* 生成图片 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x9F\xA5\xE7\x9C\x8B\xE7\x94\x9F\xE6\x88\x90\xE7\x9A\x84\xE5\x9B\xBE\xE7\x89\x87" /* 正在查看生成的图片 */,
                   ""},
    {"image_view", "\xE6\x9F\xA5\xE7\x9C\x8B\xE5\x9B\xBE\xE7\x89\x87" /* 查看图片 */,
                   "\xE6\x9F\xA5\xE7\x9C\x8B %d \xE5\xBC\xA0\xE5\x9B\xBE\xE7\x89\x87" /* 查看 %d 张图片 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x86\xE6\x9E\x90\xE5\x9B\xBE\xE7\x89\x87" /* 正在分析图片 */,
                   kKindRead},
    {"goal",       "\xE6\x9B\xB4\xE6\x96\xB0\xE7\x9B\xAE\xE6\xA0\x87" /* 更新目标 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   ""},
    {"question",   "\xE5\x90\x91\xE4\xBD\xA0\xE6\x8F\x90\xE9\x97\xAE" /* 向你提问 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xA0\xB9\xE6\x8D\xAE\xE4\xBD\xA0\xE7\x9A\x84\xE5\x9B\x9E\xE7\xAD\x94\xE7\xBB\xA7\xE7\xBB\xAD" /* 正在根据你的回答继续 */,
                   ""},
    {"plan",       "\xE5\x88\x87\xE6\x8D\xA2\xE8\xA7\x84\xE5\x88\x92\xE6\xA8\xA1\xE5\xBC\x8F" /* 切换规划模式 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   ""},
    {"worktree",   "\xE5\x88\x87\xE6\x8D\xA2\xE5\xB7\xA5\xE4\xBD\x9C\xE5\x8C\xBA" /* 切换工作区 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   ""},
    {"finish",     "\xE6\x94\xB6\xE5\xB0\xBE" /* 收尾 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE6\x95\xB4\xE7\x90\x86\xE7\xBB\x93\xE6\x9E\x9C" /* 正在整理结果 */,
                   ""},
    {"theme",      "\xE7\x94\x9F\xE6\x88\x90\xE4\xB8\xBB\xE9\xA2\x98" /* 生成主题 */, "",
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   ""},
    {"generic",    "\xE8\xB0\x83\xE7\x94\xA8\xE5\xB7\xA5\xE5\x85\xB7" /* 调用工具 */,
                   "\xE8\xB0\x83\xE7\x94\xA8 %d \xE4\xB8\xAA\xE5\xB7\xA5\xE5\x85\xB7" /* 调用 %d 个工具 */,
                   "\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xA7\x84\xE5\x88\x92\xE4\xB8\x8B\xE4\xB8\x80\xE6\xAD\xA5" /* 正在规划下一步 */,
                   ""},
};
constexpr std::size_t kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);

std::size_t category_index(const std::string& key) {
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        if (key == kCategories[i].key) return i;
    }
    return kCategoryCount - 1;
}

// 工具名只按原生名判(AgentLoop 传进来的已经过 resolve_model_tool_name_to_native)。
std::size_t category_of(const std::string& name) {
    if (name == "file_read") return category_index("read");
    if (name == "grep" || name == "lsp") return category_index("search");
    if (name == "glob") return category_index("find");
    if (name == "bash") return category_index("command");
    if (name == "file_edit" || name == "apply_patch") return category_index("edit");
    if (name == "file_write") return category_index("write");
    if (name == "web_search") return category_index("web_search");
    if (name == "web_fetch") return category_index("web_fetch");
    if (starts_with(name, "browser_") || name == "agent_browser") return category_index("browser");
    if (starts_with(name, "computer_")) return category_index("computer");
    if (name == "spawn_subagent") return category_index("spawn");
    if (name == "wait_subagent") return category_index("wait");
    if (name == "memory_read") return category_index("memory_read");
    if (name == "memory_write") return category_index("memory_write");
    if (name == "skill_view" || name == "skills_list") return category_index("skill");
    if (name == "TodoWrite") return category_index("todo");
    if (name == "image_generate") return category_index("image_gen");
    if (name == "vision_analyze" || name == "show_image") return category_index("image_view");
    if (name == "create_goal" || name == "update_goal" || name == "get_goal") return category_index("goal");
    if (name == "AskUserQuestion") return category_index("question");
    if (name == "EnterPlanMode" || name == "ExitPlanMode") return category_index("plan");
    if (name == "EnterWorktree" || name == "ExitWorktree") return category_index("worktree");
    if (name == "task_complete") return category_index("finish");
    if (name == "theme_create") return category_index("theme");
    return kCategoryCount - 1;
}

std::string category_phrase(std::size_t index, int count) {
    const ActivityCategory& c = kCategories[index];
    const std::string_view counted = c.counted_phrase;
    if (count > 1 && !counted.empty()) {
        std::string out(counted);
        const std::size_t at = out.find("%d");
        if (at != std::string::npos) out.replace(at, 2, std::to_string(count));
        return out;
    }
    return c.phrase;
}

// 按首次出现顺序分组计数。
std::vector<std::pair<std::size_t, int>> group_categories(const std::vector<std::string>& names) {
    std::vector<std::pair<std::size_t, int>> groups;
    for (const auto& name : names) {
        const std::size_t idx = category_of(name);
        auto it = std::find_if(groups.begin(), groups.end(),
                               [idx](const auto& g) { return g.first == idx; });
        if (it == groups.end()) groups.push_back({idx, 1});
        else ++it->second;
    }
    return groups;
}

}  // namespace

std::string batch_activity_label(const std::vector<std::string>& native_tool_names) {
    const auto groups = group_categories(native_tool_names);
    if (groups.empty()) return {};
    std::string out = kActivityPrefix;
    out += category_phrase(groups[0].first, groups[0].second);
    if (groups.size() == 2) {
        out += "\xE5\xB9\xB6";  // 并
        out += category_phrase(groups[1].first, groups[1].second);
    } else if (groups.size() > 2) {
        out += "\xE3\x80\x81";  // 、
        out += category_phrase(groups[1].first, groups[1].second);
        out += "\xE7\xAD\x89";  // 等
    }
    return out;
}

std::string after_batch_activity_label(const std::vector<std::string>& native_tool_names) {
    const auto groups = group_categories(native_tool_names);
    if (groups.empty()) return {};
    return kCategories[groups[0].first].after;
}

std::string batch_activity_kind(const std::vector<std::string>& native_tool_names) {
    if (native_tool_names.empty()) return {};
    bool all_read = true;
    for (const auto& name : native_tool_names) {
        const std::string_view kind = kCategories[category_of(name)].kind;
        if (kind == kKindWrite) return kKindWrite;
        if (kind != kKindRead) all_read = false;
    }
    return all_read ? kKindRead : std::string{};
}

} // namespace acecode::tool_preamble
