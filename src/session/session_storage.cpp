#include "session_storage.hpp"
#include "session_serializer.hpp"
#include "session_title_generator.hpp"
#include "../config/config.hpp"
#include "../prompt/context_usage_breakdown.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/utf8_path.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <functional>
#include <regex>
#include <mutex>
#include <iterator>
#include <string_view>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string normalize_permission_mode_name(std::string mode) {
    if (mode == "acceptEdits") mode = "accept-edits";
    if (mode == "accept-edits" || mode == "yolo" || mode == "plan") return mode;
    return "default";
}

std::string normalize_pre_plan_permission_mode_name(std::string mode) {
    if (mode.empty()) return {};
    mode = normalize_permission_mode_name(std::move(mode));
    return mode == "plan" ? std::string{"default"} : mode;
}

bool token_usage_has_values(const TokenUsage& usage) {
    return usage.has_data ||
           usage.prompt_tokens != 0 ||
           usage.completion_tokens != 0 ||
           usage.total_tokens != 0 ||
           usage.cache_read_tokens != 0 ||
           usage.cache_write_tokens != 0 ||
           usage.reasoning_tokens != 0 ||
           usage.context_breakdown.has_data;
}

nlohmann::json token_usage_to_json(const TokenUsage& usage) {
    nlohmann::json value = {
        {"prompt_tokens", usage.prompt_tokens},
        {"completion_tokens", usage.completion_tokens},
        {"total_tokens", usage.total_tokens},
        {"cache_read_tokens", usage.cache_read_tokens},
        {"cache_write_tokens", usage.cache_write_tokens},
        {"reasoning_tokens", usage.reasoning_tokens},
        {"has_data", usage.has_data},
    };
    if (usage.context_breakdown.has_data) {
        value["context_breakdown"] =
            context_usage_breakdown_to_json(usage.context_breakdown);
    }
    return value;
}

TokenUsage token_usage_from_json(const nlohmann::json& j) {
    TokenUsage usage;
    if (!j.is_object()) return usage;
    usage.prompt_tokens = j.value("prompt_tokens", 0);
    usage.completion_tokens = j.value("completion_tokens", 0);
    usage.total_tokens = j.value("total_tokens", 0);
    usage.cache_read_tokens = j.value("cache_read_tokens", 0);
    usage.cache_write_tokens = j.value("cache_write_tokens", 0);
    usage.reasoning_tokens = j.value("reasoning_tokens", 0);
    usage.has_data = j.value("has_data", false);
    if (auto it = j.find("context_breakdown"); it != j.end()) {
        usage.context_breakdown =
            context_usage_breakdown_from_json(*it);
    }
    return usage;
}

bool is_hidden_goal_context_message_storage(const ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("hidden_goal_context", false);
}

bool is_visible_user_turn(const ChatMessage& msg) {
    return msg.role == "user" &&
           !msg.is_meta &&
           !is_hidden_goal_context_message_storage(msg);
}

bool try_deserialize_session_record(const std::string& line,
                                    ChatMessage& message) {
    try {
        const auto json = nlohmann::json::parse(line);
        if (!json.is_object() ||
            !json.contains("role") ||
            !json["role"].is_string() ||
            json["role"].get_ref<const std::string&>().empty()) {
            return false;
        }
        message = deserialize_message(line);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::string SessionStorage::compute_project_hash(const std::string& cwd) {
    // 委托到 utils/cwd_hash.cpp 的共享实现 — desktop 的 WorkspaceRegistry 与
    // daemon 的 SessionStorage 必须用同一份算法,否则同一目录两边算出不同 hash。
    return acecode::compute_cwd_hash(cwd);
}

std::string SessionStorage::generate_session_id() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_buf.tm_year + 1900)
        << std::setw(2) << (tm_buf.tm_mon + 1)
        << std::setw(2) << tm_buf.tm_mday
        << '-'
        << std::setw(2) << tm_buf.tm_hour
        << std::setw(2) << tm_buf.tm_min
        << std::setw(2) << tm_buf.tm_sec
        << '-';

    // 4 hex random chars
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 0xFFFF);
    oss << std::hex << std::setw(4) << dist(gen);

    return oss.str();
}

std::string SessionStorage::get_project_dir(const std::string& cwd) {
    std::string acecode_dir = get_acecode_dir();
    std::string hash = compute_project_hash(cwd);
    return path_to_utf8(path_from_utf8(acecode_dir) / "projects" / hash);
}

bool SessionStorage::append_message(const std::string& session_path, const ChatMessage& msg) {
    std::string record;
    try {
        record = serialize_message(msg);
    } catch (...) {
        return false;
    }

    static std::mutex append_mu;
    std::lock_guard<std::mutex> lk(append_mu);

    const fs::path path = path_from_utf8(session_path);
    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return false;
    }

    bool isolate_existing_tail = false;
    const bool exists = fs::exists(path, ec);
    if (ec) return false;
    if (exists) {
        if (!fs::is_regular_file(path, ec) || ec) return false;
        const auto size = fs::file_size(path, ec);
        if (ec) return false;
        if (size > 0) {
            std::ifstream tail(path, std::ios::binary);
            if (!tail.is_open()) return false;
            tail.seekg(-1, std::ios::end);
            char last = '\0';
            tail.read(&last, 1);
            if (!tail.good()) return false;
            isolate_existing_tail = last != '\n';
        }
    }

    std::ofstream ofs(path, std::ios::binary | std::ios::app);
    if (!ofs.is_open()) return false;
    if (isolate_existing_tail) {
        ofs.put('\n');
    }
    ofs.write(record.data(), static_cast<std::streamsize>(record.size()));
    ofs.put('\n');
    ofs.flush();
    return ofs.good();
}

void SessionStorage::write_messages(const std::string& session_path,
                                    const std::vector<ChatMessage>& messages) {
    std::error_code ec;
    fs::create_directories(path_from_utf8(session_path).parent_path(), ec);

    std::string content;
    for (const auto& msg : messages) {
        content += serialize_message(msg);
        content.push_back('\n');
    }
    atomic_write_file(session_path, content);
}

SessionLoadResult SessionStorage::load_messages_with_diagnostics(
    const std::string& session_path) {
    SessionLoadResult result;
    std::ifstream ifs(path_from_utf8(session_path), std::ios::binary);
    if (!ifs.is_open()) return result;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < content.size()) {
        const size_t nl = content.find('\n', start);
        if (nl == std::string::npos) {
            std::string tail = content.substr(start);
            if (!tail.empty() && tail.back() == '\r') {
                tail.pop_back();
            }
            if (!tail.empty()) {
                ChatMessage message;
                if (try_deserialize_session_record(tail, message)) {
                    result.messages.push_back(std::move(message));
                    result.diagnostics.recovered_unterminated_record = true;
                } else {
                    result.diagnostics.ignored_partial_tail = true;
                }
            }
            break;
        }
        std::string line = content.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;
        ChatMessage message;
        if (try_deserialize_session_record(line, message)) {
            result.messages.push_back(std::move(message));
        } else {
            ++result.diagnostics.malformed_complete_records;
        }
    }
    return result;
}

std::vector<ChatMessage> SessionStorage::load_messages(const std::string& session_path) {
    return load_messages_with_diagnostics(session_path).messages;
}

bool SessionStorage::write_meta(const std::string& meta_path, const SessionMeta& meta) {
    nlohmann::json j;
    j["id"] = meta.id;
    j["cwd"] = meta.cwd;
    j["created_at"] = meta.created_at;
    j["updated_at"] = meta.updated_at;
    j["message_count"] = meta.message_count;
    j["summary"] = meta.summary;
    j["provider"] = meta.provider;
    j["model"] = meta.model;
    if (!meta.model_preset.empty()) {
        j["model_preset"] = meta.model_preset;
    }
    if (!meta.title.empty()) {
        j["title"] = meta.title;
    }
    if (!meta.title_source.empty()) {
        j["title_source"] = meta.title_source;
    }
    if (!meta.input_draft.empty()) {
        j["input_draft"] = meta.input_draft;
    }
    j["permission_mode"] = normalize_permission_mode_name(meta.permission_mode);
    if (!meta.pre_plan_permission_mode.empty()) {
        j["pre_plan_permission_mode"] =
            normalize_pre_plan_permission_mode_name(meta.pre_plan_permission_mode);
    }
    j["turn_count"] = (std::max)(0, meta.turn_count);
    if (token_usage_has_values(meta.last_token_usage)) {
        j["last_token_usage"] = token_usage_to_json(meta.last_token_usage);
    }
    if (token_usage_has_values(meta.session_token_usage)) {
        j["session_token_usage"] = token_usage_to_json(meta.session_token_usage);
    }
    if (!meta.todos.empty()) {
        j["todos"] = todo_items_to_json(meta.todos);
    }
    // fork 元数据:空字符串时省略,保持老 meta 文件 byte-byte 不变。
    if (!meta.forked_from.empty()) {
        j["forked_from"] = meta.forked_from;
    }
    if (!meta.fork_message_id.empty()) {
        j["fork_message_id"] = meta.fork_message_id;
    }
    if (!meta.parent_session_id.empty()) {
        j["parent_session_id"] = meta.parent_session_id;
    }
    if (!meta.expert_id.empty()) {
        j["expert_id"] = meta.expert_id;
        if (!meta.expert_member_id.empty()) {
            j["expert_member_id"] = meta.expert_member_id;
        }
    }
    if (!meta.loop_id.empty()) {
        j["loop_execution"] = {
            {"loop_id", meta.loop_id},
            {"run_id", meta.loop_run_id},
        };
    }
    // worktree 会话状态:inactive 时省略,保持老 meta 文件 byte-byte 不变。
    if (meta.worktree.active()) {
        j["worktree_session"] = {
            {"original_cwd", meta.worktree.original_cwd},
            {"worktree_path", meta.worktree.worktree_path},
            {"worktree_name", meta.worktree.worktree_name},
            {"worktree_branch", meta.worktree.worktree_branch},
            {"original_head_commit", meta.worktree.original_head_commit},
        };
    }
    if (meta.archived) {
        j["archived"] = true;
    }
    if (meta.no_workspace) {
        j["no_workspace"] = true;
    }

    std::error_code ec;
    fs::create_directories(path_from_utf8(meta_path).parent_path(), ec);
    return atomic_write_file(meta_path, j.dump(2) + '\n');
}

SessionMeta SessionStorage::read_meta(const std::string& meta_path) {
    SessionMeta meta;
    std::ifstream ifs(path_from_utf8(meta_path));
    if (!ifs.is_open()) return meta;

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("id"))            meta.id            = j["id"].get<std::string>();
        if (j.contains("cwd"))           meta.cwd           = j["cwd"].get<std::string>();
        if (j.contains("created_at"))    meta.created_at    = j["created_at"].get<std::string>();
        if (j.contains("updated_at"))    meta.updated_at    = j["updated_at"].get<std::string>();
        if (j.contains("message_count")) meta.message_count = j["message_count"].get<int>();
        if (j.contains("summary"))       meta.summary       = j["summary"].get<std::string>();
        if (j.contains("provider"))      meta.provider      = j["provider"].get<std::string>();
        if (j.contains("model"))         meta.model         = j["model"].get<std::string>();
        meta.model_preset    = j.value("model_preset",    std::string{});
        meta.title           = j.value("title",           std::string{});
        meta.title_source    = j.value("title_source",    std::string{});
        if (!meta.title.empty() && meta.title_source.empty()) {
            meta.title_source = "legacy";
        }
        if (meta.title_source == "generated") {
            meta.title = sanitize_generated_session_title(std::move(meta.title));
            if (meta.title.empty()) meta.title_source.clear();
        }
        meta.input_draft     = j.value("input_draft",     std::string{});
        meta.permission_mode = normalize_permission_mode_name(
            j.value("permission_mode", std::string{"default"}));
        meta.pre_plan_permission_mode = normalize_pre_plan_permission_mode_name(
            j.value("pre_plan_permission_mode", std::string{}));
        meta.turn_count      = (std::max)(0, j.value("turn_count", 0));
        if (j.contains("last_token_usage")) {
            meta.last_token_usage = token_usage_from_json(j["last_token_usage"]);
        }
        if (j.contains("session_token_usage")) {
            meta.session_token_usage = token_usage_from_json(j["session_token_usage"]);
        }
        if (j.contains("todos")) {
            meta.todos = todo_items_from_json(j["todos"]);
        }
        meta.forked_from     = j.value("forked_from",     std::string{});
        meta.fork_message_id = j.value("fork_message_id", std::string{});
        meta.parent_session_id = j.value("parent_session_id", std::string{});
        meta.expert_id = j.value("expert_id", std::string{});
        meta.expert_member_id = j.value("expert_member_id", std::string{});
        if (j.contains("loop_execution") && j["loop_execution"].is_object()) {
            const auto& loop = j["loop_execution"];
            meta.loop_id = loop.value("loop_id", std::string{});
            meta.loop_run_id = loop.value("run_id", std::string{});
        }
        if (j.contains("worktree_session") && j["worktree_session"].is_object()) {
            const auto& wt = j["worktree_session"];
            meta.worktree.original_cwd  = wt.value("original_cwd",  std::string{});
            meta.worktree.worktree_path = wt.value("worktree_path", std::string{});
            meta.worktree.worktree_name = wt.value("worktree_name", std::string{});
            meta.worktree.worktree_branch = wt.value("worktree_branch", std::string{});
            meta.worktree.original_head_commit =
                wt.value("original_head_commit", std::string{});
        }
        meta.archived        = j.value("archived",        false);
        meta.no_workspace    = j.value("no_workspace",    false);
    } catch (...) {
        // Return empty meta on parse failure
    }
    return meta;
}

// Canonical filename match:
//   group 1 = session_id (YYYYMMDD-HHMMSS-XXXX)
static const std::regex& session_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})\.jsonl$)");
    return re;
}

static const std::regex& pid_session_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})-(\d+)\.jsonl$)");
    return re;
}

static const std::regex& pid_meta_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})-(\d+)\.meta\.json$)");
    return re;
}

// 返回一个单调比较意义上的 mtime(单位是 file_clock tick;不是 unix epoch
// 也不需要是)。我们只用它做候选文件之间的排序,绝对时间没意义。失败返回 0。
static std::int64_t file_mtime_epoch(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(ftime.time_since_epoch().count());
}

// directory_entry 版本:属性由目录枚举(Windows 上是 FindNextFile 的
// WIN32_FIND_DATA)顺带带出并缓存,不像路径版 fs::last_write_time 那样
// 再去 open 一次文件。上千条目的目录靠这个差别把枚举保持在常数次 syscall。
static std::int64_t file_mtime_epoch(const fs::directory_entry& entry) {
    std::error_code ec;
    auto ftime = entry.last_write_time(ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(ftime.time_since_epoch().count());
}

// 自动生成的 id 是 YYYYMMDD-HHMMSS-XXXX,但 headless `-p --session-id`
// 允许调用方自定 [A-Za-z0-9_-]{1,64} 的 id(否则自定 id 会话在 TUI /resume、
// Web 列表与 -p -c 里全部隐身)。下面这组手写判定就是按这个宽字符集匹配的,
// 语义与原来的 meta_filename_regex + pid_meta_filename_regex 组合逐字符
// 等价 —— 换掉正则是因为列表路径要对每个目录项跑一次匹配,上千会话的目录
// 里 std::regex 的开销已经能在热缓存路径上量到。pid_* 两条正则仍保留给
// has_incompatible_pid_session_files() 这类低频调用方。
static bool is_session_id_charset(std::string_view text) {
    if (text.empty() || text.size() > 64) return false;
    for (char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool all_ascii_digits(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

static bool all_lower_hex(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// <8位数字>-<6位数字>-<4位小写hex>-<1位以上数字>,即 PID 后缀的旧实验数据。
// 这类 stem 同样落在 session id 的宽字符集里,必须单独排除,否则
// <canonical>-<pid> 会被当成一个独立会话。
static bool is_pid_suffixed_session_stem(std::string_view stem) {
    constexpr std::size_t kMinLength = 8 + 1 + 6 + 1 + 4 + 1 + 1;
    if (stem.size() < kMinLength) return false;
    if (stem[8] != '-' || stem[15] != '-' || stem[20] != '-') return false;
    if (!all_ascii_digits(stem.substr(0, 8))) return false;
    if (!all_ascii_digits(stem.substr(9, 6))) return false;
    if (!all_lower_hex(stem.substr(16, 4))) return false;
    return all_ascii_digits(stem.substr(21));
}

// 是不是一个 canonical 的 `<session-id>.meta.json`。workspace.json /
// model_override.json 等邻居文件没有这个后缀,不会被误认。
static bool is_canonical_meta_filename(std::string_view filename) {
    constexpr std::string_view kSuffix = ".meta.json";
    if (filename.size() <= kSuffix.size()) return false;
    if (filename.substr(filename.size() - kSuffix.size()) != kSuffix) return false;
    const auto stem = filename.substr(0, filename.size() - kSuffix.size());
    if (!is_session_id_charset(stem)) return false;
    return !is_pid_suffixed_session_stem(stem);
}

struct MetaFileCandidate {
    fs::path path;
    std::int64_t mtime = 0;
};

// 只枚举候选 .meta.json 的路径(need_mtime 时连 mtime 一起),不打开任何文件。
static std::vector<MetaFileCandidate> collect_meta_candidates(
    const fs::path& project_path,
    bool need_mtime,
    const std::function<bool()>& should_cancel,
    bool& cancelled) {
    std::vector<MetaFileCandidate> candidates;
    std::error_code ec;
    fs::directory_iterator it(project_path, ec);
    if (ec) return candidates;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        if (should_cancel && should_cancel()) {
            cancelled = true;
            break;
        }
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) continue;
        const std::string fname = path_to_utf8(it->path().filename());
        if (!is_canonical_meta_filename(fname)) continue;
        MetaFileCandidate candidate;
        candidate.path = it->path();
        if (need_mtime) candidate.mtime = file_mtime_epoch(*it);
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

// updated_at 降序;同一秒内按 id 降序,让同批写入的会话之间也有确定顺序
// (原来的裸 std::sort 在大量同秒 updated_at 下顺序不稳定)。
static bool session_meta_newer_first(const SessionMeta& a, const SessionMeta& b) {
    if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
    return a.id > b.id;
}

size_t utf8_safe_prefix_length_storage(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text.size();
    size_t i = max_bytes;
    while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

std::string extract_storage_summary(const std::string& content) {
    constexpr size_t max_summary_bytes = 80;
    constexpr size_t min_word_break_bytes = 60;
    if (content.size() <= max_summary_bytes) return content;

    const size_t safe_limit = utf8_safe_prefix_length_storage(content, max_summary_bytes);
    if (safe_limit == 0) return "...";
    size_t cut = safe_limit;
    while (cut > min_word_break_bytes && content[cut - 1] != ' ') {
        --cut;
    }
    if (cut <= min_word_break_bytes) cut = safe_limit;
    return content.substr(0, cut) + "...";
}

bool is_visible_history_message(const ChatMessage& msg) {
    if (msg.is_meta || msg.is_compact_summary) return false;
    if (is_hidden_goal_context_message_storage(msg)) return false;
    if (!msg.content.empty()) return true;
    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) return true;
    return false;
}

void enrich_meta_from_messages(const std::string& project_dir,
                               const std::string& session_id,
                               SessionMeta& meta) {
    if (meta.message_count > 0 && !meta.summary.empty() && meta.turn_count > 0) return;

    const auto path = SessionStorage::session_path(project_dir, session_id);
    auto messages = SessionStorage::load_messages(path);
    if (messages.empty()) return;

    int visible_count = 0;
    int visible_turn_count = 0;
    std::string latest_user;
    for (const auto& msg : messages) {
        if (!is_visible_history_message(msg)) continue;
        ++visible_count;
        if (is_visible_user_turn(msg)) {
            ++visible_turn_count;
        }
        if (is_visible_user_turn(msg) && !msg.content.empty()) {
            latest_user = msg.content;
        }
    }
    if (meta.message_count <= 0) meta.message_count = visible_count;
    if (meta.turn_count <= 0) meta.turn_count = visible_turn_count;
    if (meta.summary.empty() && !latest_user.empty()) {
        meta.summary = extract_storage_summary(latest_user);
    }
}

std::vector<SessionStorage::SessionFileCandidate>
SessionStorage::find_session_files(const std::string& project_dir,
                                    const std::string& session_id) {
    std::vector<SessionFileCandidate> result;
    fs::path project_path = path_from_utf8(project_dir);
    if (session_id.empty() || !fs::exists(project_path) || !fs::is_directory(project_path)) {
        return result;
    }

    const fs::path jsonl = path_from_utf8(SessionStorage::session_path(project_dir, session_id));
    std::error_code ec;
    if (!fs::is_regular_file(jsonl, ec)) {
        return result;
    }

    SessionFileCandidate c;
    c.jsonl_path = path_to_utf8(jsonl);
    c.meta_path = SessionStorage::meta_path(project_dir, session_id);
    c.pid = 0;
    c.mtime = file_mtime_epoch(jsonl);
    result.push_back(std::move(c));
    return result;
}

bool SessionStorage::has_incompatible_pid_session_files(
    const std::string& project_dir, const std::string& session_id) {
    fs::path project_path = path_from_utf8(project_dir);
    if (!fs::exists(project_path) || !fs::is_directory(project_path)) {
        return false;
    }

    const auto& jsonl_re = pid_session_filename_regex();
    const auto& meta_re = pid_meta_filename_regex();
    for (const auto& entry : fs::directory_iterator(project_path)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = path_to_utf8(entry.path().filename());
        std::smatch m;
        if (std::regex_match(fname, m, jsonl_re) ||
            std::regex_match(fname, m, meta_re)) {
            if (session_id.empty() || m[1].str() == session_id) {
                return true;
            }
        }
    }
    return false;
}

std::vector<SessionMeta> SessionStorage::list_session_metadata(
    const std::string& project_dir,
    const std::function<bool()>& should_cancel) {
    auto page = list_session_metadata_page(project_dir, /*limit=*/0, {}, should_cancel);
    return std::move(page.sessions);
}

SessionStorage::MetadataPage SessionStorage::list_session_metadata_page(
    const std::string& project_dir,
    int limit,
    const std::function<bool(const SessionMeta&)>& accept,
    const std::function<bool()>& should_cancel) {
    MetadataPage page;
    fs::path project_path = path_from_utf8(project_dir);
    std::error_code dir_ec;
    if (!fs::is_directory(project_path, dir_ec) || dir_ec) return page;

    const bool bounded = limit > 0;
    bool cancelled = false;
    auto candidates = collect_meta_candidates(
        project_path, /*need_mtime=*/bounded, should_cancel, cancelled);
    page.candidate_files = candidates.size();
    if (cancelled) {
        // 枚举阶段就被打断:一条也没读,但也不能声称读完了。
        page.exhausted = false;
        return page;
    }

    // 有 limit 时按 mtime 降序,让「最新的那几个」排在最前面先被打开。
    std::size_t scan_target = 0;
    if (bounded) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const MetaFileCandidate& a, const MetaFileCandidate& b) {
                      if (a.mtime != b.mtime) return a.mtime > b.mtime;
                      return a.path > b.path;
                  });
        // 多读一段余量,给 mtime 与文件内 updated_at 的细微偏差留容错:
        // 收集完仍按 updated_at 重排后才截断,所以边界几条不会错位。
        const auto want = static_cast<std::size_t>(limit);
        scan_target = want + std::max<std::size_t>(8, want);
    }

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (should_cancel && should_cancel()) {
            page.exhausted = false;
            break;
        }
        SessionMeta meta = read_meta(path_to_utf8(candidates[i].path));
        if (meta.id.empty()) continue;
        // 被 accept 拒掉的条目不占 limit 名额,否则一串归档会话就能把整页挤空。
        if (accept && !accept(meta)) continue;
        page.sessions.push_back(std::move(meta));
        ++page.accepted;
        if (scan_target > 0 && page.sessions.size() >= scan_target &&
            i + 1 < candidates.size()) {
            page.exhausted = false;
            break;
        }
    }

    std::sort(page.sessions.begin(), page.sessions.end(), session_meta_newer_first);
    if (bounded && page.sessions.size() > static_cast<std::size_t>(limit)) {
        page.sessions.resize(static_cast<std::size_t>(limit));
    }
    return page;
}

std::vector<SessionMeta> SessionStorage::list_sessions(
    const std::string& project_dir) {
    auto sessions = list_session_metadata(project_dir);
    for (auto& meta : sessions) {
        enrich_meta_from_messages(project_dir, meta.id, meta);
    }
    return sessions;
}

static std::string make_session_path(const std::string& project_dir,
                                     const std::string& session_id,
                                     const std::string& suffix,
                                     int pid) {
    std::string fname = session_id;
    if (pid > 0) {
        fname += '-';
        fname += std::to_string(pid);
    }
    fname += suffix;
    return path_to_utf8(path_from_utf8(project_dir) / fname);
}

std::string SessionStorage::session_path(const std::string& project_dir,
                                          const std::string& session_id,
                                          int pid) {
    return make_session_path(project_dir, session_id, ".jsonl", pid);
}

std::string SessionStorage::meta_path(const std::string& project_dir,
                                       const std::string& session_id,
                                       int pid) {
    return make_session_path(project_dir, session_id, ".meta.json", pid);
}

bool SessionStorage::purge_session_files(const std::string& project_dir,
                                         const std::string& session_id,
                                         std::string* error) {
    if (error) error->clear();
    if (project_dir.empty() || session_id.empty()) {
        if (error) *error = "project directory and session id are required";
        return false;
    }

    const auto remove_file = [error](const fs::path& path, const char* label) {
        std::error_code ec;
        fs::remove(path, ec);
        if (!ec) return true;
        if (error) {
            *error = std::string("failed to remove ") + label + ": " + ec.message();
        }
        return false;
    };

    if (!remove_file(path_from_utf8(session_path(project_dir, session_id)),
                     "session transcript")) {
        return false;
    }

    std::error_code ec;
    fs::remove_all(path_from_utf8(project_dir) / path_from_utf8(session_id), ec);
    if (ec) {
        if (error) {
            *error = std::string("failed to remove persisted session data: ") +
                     ec.message();
        }
        return false;
    }

    return remove_file(path_from_utf8(meta_path(project_dir, session_id)),
                       "session metadata");
}

std::string SessionStorage::now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_buf.tm_year + 1900)
        << '-' << std::setw(2) << (tm_buf.tm_mon + 1)
        << '-' << std::setw(2) << tm_buf.tm_mday
        << 'T' << std::setw(2) << tm_buf.tm_hour
        << ':' << std::setw(2) << tm_buf.tm_min
        << ':' << std::setw(2) << tm_buf.tm_sec
        << 'Z';
    return oss.str();
}

} // namespace acecode
