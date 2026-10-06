#pragma once

// Telegram 更新解析(纯逻辑,design D11)。
// 只处理 message 更新:私聊一律视为点名;群聊只有 @机器人、带机器人名的命令、
// 回复机器人消息三种情况算点名。entity 的 offset/length 以 UTF-16 单位计,
// 这里转换成 UTF-8 字节再切片。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace acecode::im::telegram {

struct BotIdentity {
    std::string id;        // 机器人用户 id
    std::string username;  // 不带 @
};

struct ParsedUpdate {
    std::int64_t update_id = -1;
    std::optional<Inbound> inbound;  // 非消息更新、频道帖子等为空
};

ParsedUpdate parse_update(const nlohmann::json& update, const BotIdentity& bot, const std::string& account);

// 按 UTF-16 单位的 offset/length 从 UTF-8 文本中取子串;越界时截到末尾。
std::string utf16_slice(const std::string& utf8, std::size_t offset, std::size_t length);

} // namespace acecode::im::telegram
