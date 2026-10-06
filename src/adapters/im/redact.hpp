#pragma once

// 通道凭据脱敏。凭据(QQ AppSecret、Telegram bot token、扫码临时密钥)只允许以
// 脱敏形式出现在接口返回、日志与错误文本里。

#include <string>
#include <string_view>
#include <vector>

namespace acecode::im {

// 展示值:保留末 4 位,前面一律 "****";少于 8 个字符时整体显示为 "****"。
std::string mask_secret(std::string_view secret);

// 从任意文本中抹掉:
//   - secrets 里列出的每个非空值(长度 >= 6 才处理,避免误伤普通词);
//   - 形如 <数字>:<30+ 位 token 字符> 的 Telegram bot token(含 URL 里的 bot<token>)。
// 被抹掉的部分替换为 "***"。手写扫描,不使用 std::regex。
std::string redact_secrets(std::string text, const std::vector<std::string>& secrets = {});

} // namespace acecode::im
