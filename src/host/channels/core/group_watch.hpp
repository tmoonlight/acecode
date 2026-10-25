#pragma once

// 群聊代答(openspec add-channel-platform-plugins D5):设置的校验与序列化、
// 群消息缓冲、到点汇总成一条用户消息的文本。只负责数据与文字,何时提交由 Conversations 决定。

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode::channels::core {

struct GroupWatchSettings {
    static constexpr int kDefaultPollIntervalS = 120;
    static constexpr int kMinPollIntervalS = 30;
    static constexpr int kMaxPollIntervalS = 86400;
    static constexpr std::size_t kMaxGroups = 20;
    static constexpr std::size_t kMaxPrefixChars = 32;
    static constexpr std::size_t kMaxPromptChars = 4000;

    std::vector<std::string> groups;  // 群聊 id,去重保序
    int poll_interval_s = kDefaultPollIntervalS;
    std::string prefix;  // 代发消息的前缀,默认无
    std::string prompt;  // 用户的代答约定

    bool is_default() const;
    bool watches(const std::string& chat) const;
};

// groups 可以是字符串数组,也可以是按换行 / 逗号 / 空白分隔的字符串;不合法时返回空并写 error。
std::optional<GroupWatchSettings> parse_group_watch_settings(const nlohmann::json& value, std::string* error);
nlohmann::json group_watch_settings_to_json(const GroupWatchSettings& settings);

struct GroupMessage {
    std::string message_id;
    std::int64_t at_ms = 0;
    std::string sender;
    std::string sender_name;
    std::string text;
    bool from_self = false;
    bool mentioned = false;  // 点名了用户本人
};

// 一个群自上次汇总以来的新消息。只保留最近 kMaxMessages 条,更早的计入 omitted。
class GroupBuffer {
public:
    static constexpr std::size_t kMaxMessages = 200;
    static constexpr std::size_t kMaxRememberedIds = 1000;

    // 重复的 message_id 忽略;返回是否收下。
    bool add(GroupMessage message);
    bool empty() const { return messages_.empty(); }
    bool has_others() const;
    std::size_t size() const { return messages_.size(); }
    std::size_t omitted() const { return omitted_; }
    const std::deque<GroupMessage>& messages() const { return messages_; }
    void clear();
    // 去掉已经汇总出去的最早 count 条(汇总期间新到的消息保留到下一次)。
    void consume(std::size_t count);

    std::string chat_name;  // 插件报告的群名,随消息更新
    std::chrono::steady_clock::time_point last_flush{};
    bool flushed_once = false;

private:
    std::deque<GroupMessage> messages_;
    std::size_t omitted_ = 0;
    std::deque<std::string> recent_ids_;
    std::set<std::string> recent_set_;
};

// 汇总文本:首行 `[群聊新消息] 群「群名」HH:MM–HH:MM,共 N 条`,随后每条一行。
// utc_offset_minutes 用于把时间戳转成本地时间(测试注入,生产取系统时区)。
std::string build_group_digest(const std::string& chat_name, const std::deque<GroupMessage>& messages,
                               std::size_t omitted, int utc_offset_minutes);
std::string group_digest_display(const std::string& chat_name, std::size_t count);
int local_utc_offset_minutes();

} // namespace acecode::channels::core
