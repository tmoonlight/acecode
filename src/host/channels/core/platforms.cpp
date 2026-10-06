#include "channels/core/platforms.hpp"

namespace acecode::channels::core {
namespace {

std::vector<PlatformSpec> build_specs() {
    std::vector<PlatformSpec> specs;

    PlatformSpec qq;
    qq.name = "qq";
    qq.label = "QQ";
    qq.fields = {{"app_id"}, {"app_secret", true}};
    qq.account_field = "app_id";
    qq.group_scoped_members = true;          // 群成员 openid 按群隔离
    qq.principals_scoped_to_account = true;  // 用户 openid 按机器人隔离
    qq.owner_binding = OwnerBinding::Scan;
    qq.scan_bind = true;
    specs.push_back(qq);

    // 微信(iLink 机器人):凭据全部来自扫码登录,设置页不能手填。
    PlatformSpec weixin;
    weixin.name = "weixin";
    weixin.label = "微信";
    weixin.fields = {{"bot_token", true, true, false},
                     {"bot_id", false, true, false},
                     {"base_url", false, false, false}};
    weixin.account_field = "bot_id";
    weixin.principals_scoped_to_account = true;
    weixin.owner_binding = OwnerBinding::Scan;
    weixin.scan_bind = true;
    specs.push_back(weixin);

    PlatformSpec feishu;
    feishu.name = "feishu";
    feishu.label = "飞书";
    feishu.fields = {{"app_id"}, {"app_secret", true}, {"domain", false, false}};
    feishu.account_field = "app_id";
    feishu.principals_scoped_to_account = true;  // open_id 按应用隔离
    specs.push_back(feishu);

    PlatformSpec dingtalk;
    dingtalk.name = "dingtalk";
    dingtalk.label = "钉钉";
    dingtalk.fields = {{"client_id"}, {"client_secret", true}, {"robot_code", false, false}};
    dingtalk.account_field = "client_id";
    dingtalk.principals_scoped_to_account = true;
    specs.push_back(dingtalk);

    PlatformSpec telegram;
    telegram.name = "telegram";
    telegram.label = "Telegram";
    telegram.fields = {{"token", true}};
    telegram.owner_binding = OwnerBinding::Link;  // 账号 id 是 token 冒号前一段
    specs.push_back(telegram);

    PlatformSpec discord;
    discord.name = "discord";
    discord.label = "Discord";
    // bot_id 与 application_id 由校验得出;设置页用 application_id 拼邀请链接。
    discord.fields = {{"token", true}, {"bot_id", false, false, false}, {"application_id", false, false, false}};
    discord.account_field = "bot_id";  // 用户 id 全局唯一,换机器人不清名单
    specs.push_back(discord);

    PlatformSpec line;
    line.name = "line";
    line.label = "LINE";
    // Channel ID + Channel secret 换短期令牌;长期令牌是可选的替代。public_url 为空时自动起
    // Cloudflare 临时隧道。
    line.fields = {{"channel_id"},
                   {"channel_secret", true},
                   {"access_token", true, false},
                   {"bot_id", false, false, false},
                   {"public_url", false, false}};
    line.account_field = "bot_id";
    line.principals_scoped_to_account = true;  // userId 按 Provider 隔离
    specs.push_back(line);

    return specs;
}

} // namespace

const std::vector<PlatformSpec>& platform_specs() {
    static const std::vector<PlatformSpec> specs = build_specs();
    return specs;
}

const PlatformSpec* find_platform_spec(const std::string& name) {
    for (const auto& spec : platform_specs())
        if (spec.name == name) return &spec;
    return nullptr;
}

std::vector<std::string> platform_names() {
    std::vector<std::string> names;
    for (const auto& spec : platform_specs()) names.push_back(spec.name);
    return names;
}

std::string platform_label(const std::string& name) {
    const auto* spec = find_platform_spec(name);
    return spec ? spec->label : name;
}

bool credentials_complete(const PlatformSpec& spec, const nlohmann::json& credentials) {
    if (!credentials.is_object()) return false;
    for (const auto& field : spec.fields) {
        if (!field.required) continue;
        const auto it = credentials.find(field.key);
        if (it == credentials.end() || !it->is_string() || it->get<std::string>().empty()) return false;
    }
    return true;
}

std::string account_of(const PlatformSpec& spec, const nlohmann::json& credentials) {
    if (!credentials.is_object()) return {};
    if (!spec.account_field.empty()) return credentials.value(spec.account_field, std::string{});
    const auto token = credentials.value("token", std::string{});
    const auto colon = token.find(':');
    return colon == std::string::npos ? std::string{} : token.substr(0, colon);
}

} // namespace acecode::channels::core
