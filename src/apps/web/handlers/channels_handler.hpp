#pragma once

// 消息通道(QQ / Telegram)REST 接口的纯函数部分:请求体校验、路径参数解码、
// 错误体与会话列表的 channel_bound 标注。路由见 routes/routes_channels.cpp。

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>

namespace acecode::web {

bool is_channel_platform(const std::string& platform);

// POST /api/channels/:platform/enabled  {"enabled": true|false}
bool parse_channel_enabled_request(const nlohmann::json& body, bool& enabled, std::string& error);

// PUT /api/channels/:platform/credentials
// 只接受该平台已知的字段(QQ:app_id / app_secret;Telegram:token),值必须是字符串,
// 去两端空白后不超过 512 字节。空字符串表示沿用已保存的值(设置页只拿得到尾号)。
// 至少要有一个非空字段,否则报错。
std::optional<nlohmann::json> parse_channel_credentials_request(const std::string& platform,
                                                                 const nlohmann::json& body,
                                                                 std::string& error);

// DELETE /api/channels/:platform/access/:principal 的路径段:百分号解码后必须是
// user: / group: / member: 前缀,不超过 300 字节。
std::optional<std::string> decode_channel_principal(const std::string& raw);

nlohmann::json channel_error_body(const std::string& code, const std::string& message);

// 会话列表行:被 IM 会话绑定的会话加 channel_bound:{"platform": ...};其余行不加该字段。
void annotate_channel_bound(nlohmann::json& rows, const std::map<std::string, std::string>& bound);

} // namespace acecode::web
