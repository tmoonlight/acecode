#include "history_text.hpp"

#include "session/session_storage.hpp"

#include <algorithm>
#include <cstdio>

namespace acecode::channels::core {
namespace {

constexpr std::size_t kSystemLineChars = 200;

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

bool has_text(const std::string& text) { return text.find_first_not_of(" \t\r\n") != std::string::npos; }

std::string trim(const std::string& text) {
    const auto b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = text.find_last_not_of(" \t\r\n");
    return text.substr(b, e - b + 1);
}

bool flag(const nlohmann::json& metadata, const char* key) {
    if (!metadata.is_object()) return false;
    const auto it = metadata.find(key);
    return it != metadata.end() && it->is_boolean() && it->get<bool>();
}

// 隐藏的目标上下文、压缩摘要、蜂群内部信封:都不是给人看的对话内容。
bool internal(const ChatMessage& message) {
    if (message.is_meta || message.is_compact_summary || flag(message.metadata, "hidden_goal_context")) return true;
    return message.metadata.is_object() && message.metadata.contains("inter_agent");
}

// 按字节截到 max_bytes 以内,不切开 UTF-8 字符。
std::string utf8_cut(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

struct Turn {
    const ChatMessage* user = nullptr;
    std::vector<const ChatMessage*> rest;
};

std::string render_turn(const Turn& turn, std::size_t ordinal, int utc_offset_minutes, bool running) {
    std::string text = "【倒数第 " + std::to_string(ordinal) + " 轮";
    if (!turn.user->timestamp.empty()) text += " · " + short_local_time(turn.user->timestamp, utc_offset_minutes);
    if (running) text += " · 执行中";
    text += "】\n我:" + trim(SessionStorage::visible_user_message_text(*turn.user));
    std::size_t files = 0;
    if (turn.user->content_parts.is_array())
        for (const auto& part : turn.user->content_parts)
            if (part.is_object() && (part.value("type", "") == "image" || part.value("type", "") == "file")) ++files;
    if (files > 0) text += (text.back() == ':' ? "" : " ") + std::string("(附带 ") + std::to_string(files) + " 个图片或文件)";
    std::size_t tool_calls = 0;
    std::vector<std::string> replies;
    std::vector<std::string> notices;
    for (const auto* message : turn.rest) {
        if (message->role == "assistant") {
            if (message->tool_calls.is_array()) tool_calls += message->tool_calls.size();
            if (has_text(message->content)) replies.push_back(trim(message->content));
        } else if (message->role == "system" && has_text(message->content)) {
            notices.push_back("[系统] " + one_line(message->content, kSystemLineChars));
        }
    }
    if (!replies.empty()) {
        text += "\nACECode:";
        for (std::size_t i = 0; i < replies.size(); ++i) text += (i == 0 ? "" : "\n\n") + replies[i];
    } else {
        text += running ? "\n(还在执行,暂时没有文字回复)" : "\n(这一轮没有文字回复)";
    }
    for (const auto& notice : notices) text += "\n" + notice;
    if (tool_calls > 0) text += "\n(这一轮调用了 " + std::to_string(tool_calls) + " 次工具)";
    return text;
}

} // namespace

bool starts_history_turn(const ChatMessage& message) { return message.role == "user" && !internal(message); }

PagedOutput render_recent_turns(const std::vector<ChatMessage>& messages, std::size_t turns,
                                int utc_offset_minutes, bool busy) {
    turns = std::clamp(turns, std::size_t{1}, kMaxHistoryTurns);
    std::vector<Turn> all;
    for (const auto& message : messages) {
        if (starts_history_turn(message)) {
            all.push_back({&message, {}});
        } else if (!all.empty() && !internal(message)) {
            all.back().rest.push_back(&message);
        }
    }
    if (all.empty()) return {"这个会话还没有对话内容。", {}};
    const auto shown = std::min(turns, all.size());
    std::string heading = shown == 1 ? "最近一轮对话:" : "最近 " + std::to_string(shown) + " 轮对话(最新的在前):";
    if (shown < turns) heading = "这个会话只有 " + std::to_string(shown) + " 轮对话(最新的在前):";
    std::string text;
    std::size_t rendered = 0;
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& turn = all[all.size() - 1 - i];
        auto block = render_turn(turn, i + 1, utc_offset_minutes, busy && i == 0);
        const auto budget = kMaxHistoryBytes - std::min(text.size(), kMaxHistoryBytes);
        if (rendered > 0 && block.size() + 2 > budget) break;
        if (block.size() > budget) block = utf8_cut(block, budget) + "…(太长,已截断)";
        if (!text.empty()) text += "\n\n";
        text += block;
        ++rendered;
    }
    if (rendered < shown)
        text += "\n\n(更早的 " + std::to_string(shown - rendered) + " 轮内容太长,已省略;可以在 Desktop 里查看。)";
    return paginate_text(std::move(heading), text);
}

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

} // namespace acecode::channels::core
