// Conversations 的 IM 命令实现(与 Desktop 输入框同名的命令、分页列表、@编号 引用会话)。
// 会话路由与绑定本身在 conversations.cpp。

#include "conversations.hpp"

#include "channels/core/group_watch.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace acecode::channels::core {
namespace {

constexpr std::size_t kMaxSearchItems = 50;
constexpr std::size_t kMaxPageArgumentDigits = 4;
constexpr std::size_t kSessionTitleChars = 40;
constexpr std::size_t kSkillDescriptionChars = 60;
constexpr auto kNoticeRelayWindow = std::chrono::minutes(30);

std::string trim(const std::string& text) {
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    return text.substr(b, e - b);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// 页码参数:1–4 位数字。
bool page_argument(const std::string& text) {
    return !text.empty() && text.size() <= kMaxPageArgumentDigits &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool contains_folded(const std::string& haystack, const std::string& needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (m <= 2);
}

// 会话时间是 UTC 的 ISO 8601("2026-10-26T07:58:04Z"),列表里显示成本地 "10-26 15:58";
// 格式不符时原样返回。
std::string short_local_time(const std::string& iso, int offset_minutes) {
    int y = 0;
    unsigned mo = 0, d = 0, h = 0, mi = 0;
    if (iso.size() < 16 || iso[10] != 'T' ||
        std::sscanf(iso.c_str(), "%4d-%2u-%2uT%2u:%2u", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12 ||
        d < 1 || d > 31 || h > 23 || mi > 59)
        return iso;
    const std::int64_t minutes = days_from_civil(y, mo, d) * 1440 + h * 60 + mi + offset_minutes;
    const auto day = minutes >= 0 ? minutes / 1440 : (minutes - 1439) / 1440;
    const auto in_day = minutes - day * 1440;
    civil_from_days(day, y, mo, d);
    char out[32];
    std::snprintf(out, sizeof(out), "%02u-%02u %02d:%02d", mo, d, static_cast<int>(in_day / 60),
                  static_cast<int>(in_day % 60));
    return out;
}

std::string target_title(const rc::RcSessionTarget& target) {
    if (!target.title.empty()) return target.title;
    if (!target.summary.empty()) return target.summary;
    return target.session_id;
}

std::string target_location(const rc::RcSessionTarget& target) {
    if (target.no_workspace) return "无项目";
    return target.workspace_label.empty() ? target.workspace_hash : target.workspace_label;
}

nlohmann::json channel_metadata(const std::string& platform, const im::Inbound& inbound) {
    nlohmann::json channel{{"platform", platform},
                           {"address", inbound.address.to_json()},
                           {"message_id", inbound.message_id},
                           {"reply_context", inbound.reply_context}};
    if (!inbound.sender_name.empty()) channel["sender_name"] = inbound.sender_name;
    return channel;
}

std::string builtin_failure(const BuiltinCommandResult& result, CommandKind kind) {
    switch (result.status) {
        case BuiltinCommandStatus::UnknownSession: return texts::session_missing();
        case BuiltinCommandStatus::UnsupportedCommand:
            return std::string("当前 ACECode 不支持 ") + command_name(kind) + "。";
        default:
            return texts::turn_failed(result.message.empty() ? std::string("命令执行失败") : result.message);
    }
}

// 命令执行成功但没有同步输出时的回复。
std::string builtin_ack(CommandKind kind) {
    switch (kind) {
        case CommandKind::Compact: return "已开始压缩对话上下文,完成后会告诉你。";
        case CommandKind::Init: return "已开始分析项目并编写 AGENTS.md。";
        case CommandKind::Goal: return "已更新线程目标。";
        case CommandKind::Plan: return "已进入计划模式。";
        default: return std::string("已执行 ") + command_name(kind) + "。";
    }
}

std::string side_failure(const SideQuestionResult& result) {
    switch (result.status) {
        case SideQuestionStatus::InvalidQuestion: return "旁问内容为空或太长。";
        case SideQuestionStatus::UnknownSession: return texts::session_missing();
        case SideQuestionStatus::ContextNotReady: return "这个会话还没有可用的上下文,先正常发一条消息再旁问。";
        case SideQuestionStatus::ProviderUnavailable: return "当前会话的模型不可用,无法旁问。";
        default: return texts::turn_failed(result.error.empty() ? std::string("旁问失败") : result.error);
    }
}

} // namespace

// ---- 分页 ----

void Conversations::show_paged(const im::Inbound& inbound, PagedOutput output, std::size_t page) {
    const auto text = render_page(output, page);
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto key = inbound.address.key();
        if (output.pages.size() > 1 && page >= 1 && page <= output.pages.size())
            pagers_[key] = Pager{std::move(output), page};
        else
            pagers_.erase(key);
    }
    reply(inbound, text);
}

void Conversations::more(const im::Inbound& inbound, const std::string& argument) {
    const auto key = inbound.address.key();
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = pagers_.find(key);
        if (it == pagers_.end()) {
            text = "没有可以翻页的内容。";
        } else {
            auto& pager = it->second;
            const auto count = pager.output.pages.size();
            const auto page = argument.empty() ? pager.page + 1 : static_cast<std::size_t>(std::stoul(argument));
            if (page == 0 || page > count) {
                text = argument.empty() ? "已经是最后一页。" : "只有 " + std::to_string(count) + " 页。";
            } else {
                pager.page = page;
                text = render_page(pager.output, page);
            }
        }
    }
    reply(inbound, text);
}

// ---- /sessions 与 @ref ----

SessionList Conversations::build_session_list(const std::string& key, bool owner,
                                              const std::optional<std::string>& query, bool refs) const {
    SessionList list;
    list.targets = visible_sessions(key, owner, query);
    if (refs) {
        // 不能引用当前会话:它的上下文本来就在。
        if (const auto current = store_.binding(key)) {
            list.targets.erase(std::remove_if(list.targets.begin(), list.targets.end(),
                                              [&current](const rc::RcSessionTarget& target) {
                                                  return target.session_id == current->session_id;
                                              }),
                               list.targets.end());
        }
    }
    const auto cap = query ? kMaxSearchItems : kMaxListItems;
    if (list.targets.size() > cap) list.targets.resize(cap);
    const auto subject = query ? "匹配「" + one_line(*query, 20) + "」的会话" : std::string{};
    if (refs)
        list.heading = (query ? subject : std::string("可引用的会话")) + "(在消息里写 @编号 引用,一次最多 " +
                       std::to_string(kMaxSessionReferences) + " 个):";
    else
        list.heading = (query ? subject : std::string("可切换的会话")) + "(发送 /r <编号> 切换):";
    return list;
}

PagedOutput Conversations::session_pages(const SessionList& list, const std::string& current_session) const {
    const int offset = deps_.utc_offset_minutes ? deps_.utc_offset_minutes() : local_utc_offset_minutes();
    std::vector<std::string> rows;
    rows.reserve(list.targets.size());
    for (const auto& target : list.targets) {
        std::string row = one_line(target_title(target), kSessionTitleChars) + " · " +
                          one_line(target_location(target), 20);
        if (!target.updated_at.empty()) row += " · " + short_local_time(target.updated_at, offset);
        if (target.session_id == current_session) row += " ·(当前)";
        rows.push_back(std::move(row));
    }
    return paginate_rows(list.heading, rows);
}

void Conversations::show_session_list(const im::Inbound& inbound, bool owner, const std::string& argument,
                                      bool refs) {
    const auto key = inbound.address.key();
    const auto current = store_.binding(key);
    const auto current_id = current ? current->session_id : std::string{};
    auto& lists = refs ? ref_lists_ : session_lists_;
    std::size_t page = 1;
    std::optional<std::string> query;
    SessionList list;
    bool reuse = false;
    const auto lowered = lower(argument);
    if (page_argument(argument)) {
        // 翻看最近一次列表;还没有列表时先列最近的会话。
        page = static_cast<std::size_t>(std::stoul(argument));
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = lists.find(key);
        if (it != lists.end() && !it->second.targets.empty()) {
            list = it->second;
            reuse = true;
        }
    } else if (!refs && (lowered == "more" || lowered == "all")) {
        page = lowered == "more" ? 2 : 1;  // 旧写法:more 看第二页
    } else if (lowered.rfind("search ", 0) == 0) {
        query = trim(argument.substr(7));
    } else if (!argument.empty()) {
        query = argument;
    }
    if (!reuse) {
        list = build_session_list(key, owner, query, refs);
        std::lock_guard<std::mutex> lock(mu_);
        lists[key] = list;
    }
    if (list.targets.empty()) {
        if (query) reply(inbound, "没有找到匹配「" + one_line(*query, 20) + "」的会话。");
        else if (refs) reply(inbound, owner ? "没有可引用的其它会话。" : "你还没有通过这里创建过其它会话。");
        else reply(inbound, owner ? "没有可切换的会话。" : "你还没有通过这里创建过会话。");
        return;
    }
    auto output = session_pages(list, current_id);
    if (page > output.pages.size()) page = std::max<std::size_t>(output.pages.size(), 1);
    show_paged(inbound, std::move(output), page);
}

// ---- @编号 ----

bool Conversations::resolve_session_mentions(const im::Inbound& inbound, const std::string& session_id,
                                             std::string& text, std::vector<rc::RcSessionTarget>& referenced) {
    const auto mentions = find_session_ref_mentions(text);
    if (mentions.empty()) return true;
    std::vector<rc::RcSessionTarget> list;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = ref_lists_.find(inbound.address.key());
        if (it != ref_lists_.end()) list = it->second.targets;
    }
    // 没发过 @ref 时 @数字 只是普通文字(例如「@3 点开会」)。
    if (list.empty()) return true;
    if (!deps_.reference_sessions) {
        reply(inbound, "当前 ACECode 不支持引用会话。");
        return false;
    }
    std::string rewritten;
    std::size_t cursor = 0;
    std::set<std::string> seen;
    for (const auto& mention : mentions) {
        if (mention.index == 0 || mention.index > list.size()) {
            reply(inbound, "没有第 " + std::to_string(mention.index) + " 个会话。发送 @ref 重新查看可引用的会话。");
            return false;
        }
        const auto& target = list[mention.index - 1];
        if (target.session_id == session_id) {
            reply(inbound, "不能引用当前会话。");
            return false;
        }
        if (seen.insert(target.session_id).second) referenced.push_back(target);
        rewritten += text.substr(cursor, mention.offset - cursor);
        rewritten += "@「" + one_line(target_title(target), kSessionTitleChars) + "」";
        cursor = mention.offset + mention.length;
    }
    if (referenced.size() > kMaxSessionReferences) {
        reply(inbound, "一次最多引用 " + std::to_string(kMaxSessionReferences) + " 个会话。");
        return false;
    }
    rewritten += text.substr(cursor);
    text = std::move(rewritten);
    return true;
}

std::optional<SessionReferenceExpansion> Conversations::with_references(
    const im::Inbound& inbound, const std::string& session_id, const std::vector<rc::RcSessionTarget>& referenced,
    const std::string& body) {
    std::string error;
    auto expanded = deps_.reference_sessions(session_id, referenced, body, &error);
    if (!expanded) reply(inbound, texts::turn_failed(error.empty() ? std::string("读取被引用的会话失败") : error));
    return expanded;
}

// ---- /skills ----

void Conversations::list_skills(const im::Inbound& inbound, const std::string& session_id,
                                const std::string& argument) {
    if (!deps_.list_skills) {
        reply(inbound, "当前 ACECode 不支持列出技能。");
        return;
    }
    std::size_t page = 1;
    std::string query;
    if (page_argument(argument)) page = static_cast<std::size_t>(std::stoul(argument));
    else query = argument;
    auto skills = deps_.list_skills(session_id);
    if (!query.empty()) {
        skills.erase(std::remove_if(skills.begin(), skills.end(),
                                    [&query](const SkillListing& skill) {
                                        return !contains_folded(skill.name, query) &&
                                               !contains_folded(skill.description, query);
                                    }),
                     skills.end());
    }
    std::sort(skills.begin(), skills.end(),
              [](const SkillListing& a, const SkillListing& b) { return a.name < b.name; });
    if (skills.empty()) {
        reply(inbound, query.empty() ? "当前会话没有可用的技能。" : "没有匹配「" + one_line(query, 20) + "」的技能。");
        return;
    }
    std::vector<std::string> rows;
    rows.reserve(skills.size());
    for (const auto& skill : skills) {
        auto row = "/" + one_line(skill.name, 64);
        if (!skill.description.empty()) row += " — " + one_line(skill.description, kSkillDescriptionChars);
        rows.push_back(std::move(row));
    }
    const auto heading = (query.empty() ? std::string("可用技能") : "匹配「" + one_line(query, 20) + "」的技能") +
                         "(发送 /技能名 [要求] 使用):";
    auto output = paginate_rows(heading, rows);
    if (page > output.pages.size()) {
        reply(inbound, "只有 " + std::to_string(output.pages.size()) + " 页。");
        return;
    }
    show_paged(inbound, std::move(output), page);
}

// ---- 与 Desktop 同名的内置命令 ----

void Conversations::run_builtin(const im::Inbound& inbound, const std::string& session_id,
                                const std::shared_ptr<Projection>& projection, const Command& command) {
    BuiltinCommandRequest request;
    request.name = builtin_command_name(command.kind);
    request.args = command.argument;
    request.display_text = trim(inbound.text);
    BuiltinCommandResult result;
    auto& sessions = *deps_.sessions;
    const auto captured = projection->capture_system_messages(
        [&sessions, &session_id, &request, &result] { result = sessions.execute_builtin_command(session_id, request); });
    std::string text;
    for (const auto& item : captured) text += (text.empty() ? "" : "\n\n") + item;
    if (result.status != BuiltinCommandStatus::Accepted) {
        // 参数不对等失败也会先发一条说明(例如 /swarm 的用法),有就用它。
        if (text.empty()) reply(inbound, builtin_failure(result, command.kind));
        else show_paged(inbound, paginate_text({}, text));
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kNoticeRelayWindow;
    if (command.kind == CommandKind::Compact) {
        projection->relay_next_notice({{"context_compacted", "已压缩对话上下文。"},
                                       {"context_compact_failed", "压缩对话上下文失败:{}"},
                                       {"context_compact_stopped", "压缩被钩子阻止。"}},
                                      deadline);
    } else if (command.kind == CommandKind::Memory && lower(command.argument).rfind("flush", 0) == 0) {
        projection->relay_next_notice({{"memory_flush_done", ""}}, deadline);
    }
    if (text.empty()) text = builtin_ack(command.kind);
    show_paged(inbound, paginate_text({}, text));
}

void Conversations::steer(const im::Inbound& inbound, const std::string& session_id, const std::string& text) {
    const auto info = session_info(session_id);
    const auto no_turn = "当前没有正在执行的回合,直接发消息即可。";
    if (!info || !info->busy || info->active_turn_id.empty()) {
        reply(inbound, no_turn);
        return;
    }
    UserInput input;
    input.text = text;
    input.display_text = text;
    input.metadata["channel"] = channel_metadata(platform_, inbound);
    const auto result = deps_.sessions->steer_input(session_id, info->active_turn_id, input);
    switch (result.status) {
        case TurnSteerStatus::Accepted: reply(inbound, "已把补充说明加入当前回合。"); return;
        case TurnSteerStatus::NoActiveTurn: reply(inbound, no_turn); return;
        case TurnSteerStatus::NonSteerable:
            reply(inbound, "当前回合不接受补充说明。直接发消息会排在这一回合之后。");
            return;
        default:
            reply(inbound, texts::turn_failed(result.message.empty() ? std::string("补充说明失败") : result.message));
            return;
    }
}

// ---- /btw ----

void Conversations::ask_side(const im::Inbound& inbound, const std::string& session_id, const std::string& question) {
    if (!deps_.side_question) {
        reply(inbound, "当前 ACECode 不支持旁问。");
        return;
    }
    if (question.size() > kMaxSideQuestionBytes) {
        reply(inbound, "旁问太长了,请缩短到 " + std::to_string(kMaxSideQuestionBytes / 3) + " 字以内。");
        return;
    }
    const bool queued = post_task([inbound, session_id, question](Conversations& self) {
        auto cancellation = std::make_shared<SideChatCancellation>();
        {
            std::lock_guard<std::mutex> lock(self.tasks_mu_);
            if (self.tasks_stopping_) return;
            self.running_side_ = cancellation;
        }
        const auto result = self.deps_.side_question(session_id, question, *cancellation);
        {
            std::lock_guard<std::mutex> lock(self.tasks_mu_);
            self.running_side_.reset();
        }
        if (cancellation->aborted.load()) return;
        if (result.status != SideQuestionStatus::Ok) {
            self.reply(inbound, side_failure(result));
            return;
        }
        std::string text = "旁问:" + one_line(question, 30) + "\n" + result.answer;
        if (!result.tools_used.empty()) {
            text += "\n(查看了 " + std::to_string(result.tools_used.size()) + " 处内容)";
        }
        self.show_paged(inbound, paginate_text({}, text));
    });
    if (!queued) reply(inbound, "还有旁问没有回答完,请稍后再问。");
}

bool Conversations::post_task(std::function<void(Conversations&)> task) {
    std::lock_guard<std::mutex> lock(tasks_mu_);
    if (tasks_stopping_ || tasks_.size() >= kMaxQueuedSideQuestions) return false;
    tasks_.push_back(std::move(task));
    if (!tasks_worker_.joinable()) tasks_worker_ = acecode::JoiningThread(&Conversations::task_loop, this);
    tasks_cv_.notify_one();
    return true;
}

void Conversations::task_loop() {
    for (;;) {
        std::function<void(Conversations&)> task;
        {
            std::unique_lock<std::mutex> lock(tasks_mu_);
            tasks_cv_.wait(lock, [this] { return tasks_stopping_ || !tasks_.empty(); });
            if (tasks_stopping_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task(*this);
        } catch (const std::exception& e) {
            LOG_WARN("[channels/" + platform_ + "] background command failed: " + e.what());
        }
    }
}

void Conversations::stop_tasks() {
    {
        std::lock_guard<std::mutex> lock(tasks_mu_);
        tasks_stopping_ = true;
        tasks_.clear();
        if (running_side_) running_side_->cancel();
    }
    tasks_cv_.notify_all();
    if (tasks_worker_.joinable()) tasks_worker_.join();
    // 断开后可能重新连接:允许之后再启动工作线程。
    std::lock_guard<std::mutex> lock(tasks_mu_);
    tasks_stopping_ = false;
}

} // namespace acecode::channels::core
