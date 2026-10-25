#include "group_watch.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace acecode::channels::core {
namespace {

constexpr std::size_t kMaxMessageChars = 2000;
constexpr std::size_t kMaxDigestBytes = 48 * 1024;
constexpr std::size_t kMaxGroupIdBytes = 128;

std::size_t utf8_chars(const std::string& text) {
    std::size_t count = 0;
    for (const unsigned char c : text)
        if ((c & 0xC0) != 0x80) ++count;
    return count;
}

// 按码点截断,不产生半个字符。
std::string utf8_prefix(const std::string& text, std::size_t max_chars, bool* truncated) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0xC0) == 0x80) continue;
        if (chars == max_chars) {
            if (truncated) *truncated = true;
            return text.substr(0, i);
        }
        ++chars;
    }
    if (truncated) *truncated = false;
    return text;
}

bool group_id_ok(const std::string& id) {
    if (id.empty() || id.size() > kMaxGroupIdBytes) return false;
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_' || c == '.' || c == ':' || c == '@' || c == '+' || c == '/' || c == '=' || c == '$';
        if (!ok) return false;
    }
    return true;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

bool split_groups(const std::string& raw, std::vector<std::string>& out, std::string* error) {
    // 中文逗号、分号也当作分隔符,方便直接粘贴。
    auto text = replace_all(replace_all(raw, "\xEF\xBC\x8C", ","), "\xEF\xBC\x9B", ",");
    std::string current;
    const auto flush = [&]() -> bool {
        if (current.empty()) return true;
        if (!group_id_ok(current)) {
            if (error) *error = "群聊 ID 只能包含字母、数字和 - _ . : @ + / = $:" + current;
            return false;
        }
        out.push_back(current);
        current.clear();
        return true;
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!flush()) return false;
        } else {
            current.push_back(c);
        }
    }
    return flush();
}

std::string clock_text(std::int64_t at_ms, int utc_offset_minutes) {
    const std::int64_t minutes = (at_ms / 60000 + utc_offset_minutes) % (24 * 60);
    const std::int64_t normalized = minutes < 0 ? minutes + 24 * 60 : minutes;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(normalized / 60), static_cast<int>(normalized % 60));
    return buf;
}

std::string message_line(const GroupMessage& message, int utc_offset_minutes) {
    std::string who = message.from_self ? std::string("我")
                                        : (message.sender_name.empty() ? message.sender : message.sender_name);
    if (message.mentioned && !message.from_self) who += "(@我)";
    bool truncated = false;
    auto body = utf8_prefix(message.text, kMaxMessageChars, &truncated);
    if (truncated) body += "…(已截断)";
    body = replace_all(body, "\r\n", "\n");
    body = replace_all(body, "\n", "\n    ");
    return "[" + clock_text(message.at_ms, utc_offset_minutes) + "] " + who + ": " + body;
}

} // namespace

bool GroupWatchSettings::is_default() const {
    return groups.empty() && poll_interval_s == kDefaultPollIntervalS && prefix.empty() && prompt.empty();
}

bool GroupWatchSettings::watches(const std::string& chat) const {
    return std::find(groups.begin(), groups.end(), chat) != groups.end();
}

std::optional<GroupWatchSettings> parse_group_watch_settings(const nlohmann::json& value, std::string* error) {
    const auto reject = [error](const std::string& message) {
        if (error) *error = message;
        return std::optional<GroupWatchSettings>{};
    };
    if (!value.is_object()) return reject("群聊代答设置必须是 JSON 对象");
    GroupWatchSettings settings;
    if (const auto it = value.find("groups"); it != value.end() && !it->is_null()) {
        std::vector<std::string> raw;
        if (it->is_string()) {
            if (!split_groups(it->get<std::string>(), raw, error)) return std::nullopt;
        } else if (it->is_array()) {
            for (const auto& item : *it) {
                if (!item.is_string()) return reject("群聊 ID 必须是字符串");
                if (!split_groups(item.get<std::string>(), raw, error)) return std::nullopt;
            }
        } else {
            return reject("群聊 ID 必须是字符串或字符串数组");
        }
        for (auto& id : raw)
            if (std::find(settings.groups.begin(), settings.groups.end(), id) == settings.groups.end())
                settings.groups.push_back(std::move(id));
        if (settings.groups.size() > GroupWatchSettings::kMaxGroups) return reject("最多设置 20 个群聊");
    }
    if (const auto it = value.find("poll_interval_s"); it != value.end() && !it->is_null()) {
        long long seconds = 0;
        const std::string digits = it->is_string() ? it->get<std::string>() : std::string{};
        if (it->is_number_integer()) {
            seconds = it->get<long long>();
        } else if (!digits.empty() && digits.size() <= 6 &&
                   std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            seconds = std::stoll(digits);
        } else {
            return reject("轮询间隔必须是整数秒");
        }
        if (seconds < GroupWatchSettings::kMinPollIntervalS || seconds > GroupWatchSettings::kMaxPollIntervalS)
            return reject("轮询间隔需在 30 秒到 24 小时之间");
        settings.poll_interval_s = static_cast<int>(seconds);
    }
    if (const auto it = value.find("prefix"); it != value.end() && !it->is_null()) {
        if (!it->is_string()) return reject("消息前缀必须是字符串");
        auto prefix = it->get<std::string>();
        if (prefix.find_first_of("\r\n") != std::string::npos || prefix.find('\0') != std::string::npos)
            return reject("消息前缀不能换行");
        if (utf8_chars(prefix) > GroupWatchSettings::kMaxPrefixChars) return reject("消息前缀最多 32 个字");
        settings.prefix = std::move(prefix);
    }
    if (const auto it = value.find("prompt"); it != value.end() && !it->is_null()) {
        if (!it->is_string()) return reject("代答提示词必须是字符串");
        auto prompt = replace_all(it->get<std::string>(), "\r\n", "\n");
        if (prompt.find('\0') != std::string::npos) return reject("代答提示词包含非法字符");
        if (utf8_chars(prompt) > GroupWatchSettings::kMaxPromptChars) return reject("代答提示词最多 4000 个字");
        const auto begin = prompt.find_first_not_of(" \t\n");
        settings.prompt = begin == std::string::npos ? std::string{}
                                                     : prompt.substr(begin, prompt.find_last_not_of(" \t\n") - begin + 1);
    }
    return settings;
}

nlohmann::json group_watch_settings_to_json(const GroupWatchSettings& settings) {
    return {{"groups", settings.groups},
            {"poll_interval_s", settings.poll_interval_s},
            {"prefix", settings.prefix},
            {"prompt", settings.prompt}};
}

bool GroupBuffer::add(GroupMessage message) {
    if (!message.message_id.empty()) {
        if (recent_set_.count(message.message_id)) return false;
        recent_ids_.push_back(message.message_id);
        recent_set_.insert(message.message_id);
        while (recent_ids_.size() > kMaxRememberedIds) {
            recent_set_.erase(recent_ids_.front());
            recent_ids_.pop_front();
        }
    }
    messages_.push_back(std::move(message));
    while (messages_.size() > kMaxMessages) {
        messages_.pop_front();
        ++omitted_;
    }
    return true;
}

bool GroupBuffer::has_others() const {
    return std::any_of(messages_.begin(), messages_.end(), [](const GroupMessage& m) { return !m.from_self; });
}

void GroupBuffer::clear() {
    messages_.clear();
    omitted_ = 0;
}

void GroupBuffer::consume(std::size_t count) {
    while (count > 0 && !messages_.empty()) {
        messages_.pop_front();
        --count;
    }
    omitted_ = 0;
}

std::string build_group_digest(const std::string& chat_name, const std::deque<GroupMessage>& messages,
                               std::size_t omitted, int utc_offset_minutes) {
    std::vector<std::string> lines;
    for (const auto& message : messages) lines.push_back(message_line(message, utc_offset_minutes));
    // 总长超限时从最早的消息开始省略,保留最新的。
    std::size_t first = 0;
    std::size_t total = 0;
    for (const auto& line : lines) total += line.size() + 1;
    while (total > kMaxDigestBytes && first + 1 < lines.size()) {
        total -= lines[first].size() + 1;
        ++first;
        ++omitted;
    }
    const auto name = chat_name.empty() ? std::string("未命名群聊") : chat_name;
    std::string head = "[群聊新消息] 群「" + name + "」";
    if (!messages.empty()) {
        head += clock_text(messages[first].at_ms, utc_offset_minutes) + "–" +
                clock_text(messages.back().at_ms, utc_offset_minutes);
    }
    head += ",共 " + std::to_string(messages.size() - first) + " 条";
    if (omitted > 0) head += "(更早的 " + std::to_string(omitted) + " 条已省略)";
    std::string text = head;
    for (std::size_t i = first; i < lines.size(); ++i) text += "\n" + lines[i];
    return text;
}

std::string group_digest_display(const std::string& chat_name, std::size_t count) {
    return "群聊「" + (chat_name.empty() ? std::string("未命名群聊") : chat_name) + "」" + std::to_string(count) +
           " 条新消息";
}

int local_utc_offset_minutes() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    std::tm utc{};
#ifdef _WIN32
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
#else
    localtime_r(&now, &local);
    gmtime_r(&now, &utc);
#endif
    const int day_diff = local.tm_yday - utc.tm_yday;
    const int wrapped = day_diff > 1 ? -1 : (day_diff < -1 ? 1 : day_diff);
    return wrapped * 24 * 60 + (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min);
}

} // namespace acecode::channels::core
