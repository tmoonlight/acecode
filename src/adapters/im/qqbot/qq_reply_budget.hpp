#pragma once

// QQ 被动回复额度与补发队列(纯逻辑,design D10)。
//
// 官方规则:对一条用户消息,私聊 60 分钟内最多回 4 条、群聊 5 分钟内最多回 5 条
// (被动回复,带原消息 msg_id);超出后只能发主动消息,而主动消息可能被用户或群主关闭。
// 所有发送共用一个全进程单调递增的 msg_seq —— 平台按 (msg_id, msg_seq) 去重,
// 同一 msg_id 下重复的 msg_seq 会被判成重复消息(40054005)。

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace acecode::im::qqbot {

struct ReplyLimits {
    std::chrono::milliseconds c2c_window{std::chrono::minutes(60)};
    int c2c_max = 4;
    std::chrono::milliseconds group_window{std::chrono::minutes(5)};
    int group_max = 5;
};

struct ReplyPlan {
    bool passive = false;   // true:带 msg_id 被动回复;false:主动消息
    std::string msg_id;
    std::int64_t msg_seq = 0;
};

class ReplyBudget {
public:
    explicit ReplyBudget(ReplyLimits limits = {});
    // 为一次发送选择方式。被动回复会占用该消息的一次额度;额度用尽或窗口已过则改为主动。
    ReplyPlan plan(const nlohmann::json& reply_context, std::int64_t now_ms);
    // 平台拒绝了某条被动回复(窗口已过等):这条消息不再尝试被动回复。
    void exhaust(const std::string& msg_id);
    std::int64_t next_seq();

private:
    struct Entry {
        int used = 0;
        bool exhausted = false;
        std::int64_t received_at_ms = 0;
    };
    void prune_locked(std::int64_t now_ms);

    ReplyLimits limits_;
    std::mutex mu_;
    std::map<std::string, Entry> entries_;
    std::int64_t seq_ = 0;
};

// 主动消息也被拒时暂存的输出,在对方下一条消息到来时作为被动回复补发。
struct HeldItem {
    bool file = false;
    std::string text;                // 文本(Markdown)
    std::filesystem::path path;      // 文件
    std::string name;
    std::string mime_type;
};

class HeldQueue {
public:
    static constexpr std::size_t kMaxPerConversation = 20;
    // 超过上限时丢弃最旧的一条,返回被丢弃的条数。
    std::size_t push(const std::string& conversation_key, HeldItem item);
    std::deque<HeldItem> take(const std::string& conversation_key);
    std::size_t size(const std::string& conversation_key) const;
    std::size_t total() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, std::deque<HeldItem>> items_;
};

} // namespace acecode::im::qqbot
