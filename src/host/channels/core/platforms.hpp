#pragma once

// 消息通道支持的平台清单。核心、宿主与 Web 接口都从这里取平台名、凭据字段与身份规则,
// 不再各自写死。平台的连接方式(传输层)在 platform_adapters.cpp 里按名字创建。

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace acecode::channels::core {

// 机主怎么产生:
//   Scan  扫码绑定时平台给出扫码人(QQ、微信);拿不到时退回“10 分钟内第一个私聊的人”
//   Link  一次性深链接(Telegram https://t.me/<机器人>?start=<码>)
//   Code  一次性 6 位绑定码,机主私聊机器人发送这串数字(飞书、钉钉、Discord、LINE)
//   Self  插件登录的就是用户本人的账号:只接受“自己发给自己”的私聊,命中即机主
// 前三种方式之外,平台还没有机主时批准第一个私聊请求,对方也会成为机主。
enum class OwnerBinding { Scan, Link, Code, Self };

// 外部平台插件(openspec add-channel-platform-plugins D1):清单所在目录、要启动的程序与参数。
struct PluginLaunch {
    std::string manifest_dir;       // UTF-8 绝对路径;子进程的工作目录
    std::string command;            // UTF-8 绝对路径
    std::vector<std::string> args;  // ${manifest_dir} 已替换
};

struct CredentialField {
    std::string key;
    bool secret = false;      // 快照只回尾号,日志一律不写
    bool required = true;     // 缺少时视为未配置
    bool user_input = true;   // 设置页可以提交;false = 由校验或扫码得出(机器人 id 等)
};

struct PlatformSpec {
    std::string name;   // 平台名:数据目录名、接口路径段、Address::platform
    std::string label;  // 展示名:IM 里的提示文字与日志
    std::vector<CredentialField> fields;
    // 绑定地址里的账号 id(Address::account)所在的凭据字段;为空时按 token 冒号前一段推出(Telegram)。
    std::string account_field;
    bool group_scoped_members = false;          // 群成员身份按群隔离(QQ 的群成员 openid)
    bool principals_scoped_to_account = false;  // 用户 id 按机器人隔离:换机器人时清空机主与授权名单
    OwnerBinding owner_binding = OwnerBinding::Code;
    bool scan_bind = false;                     // 有扫码绑定流程(QQ、微信)
    // 外部平台插件:由清单注册,传输层是插件子进程。内置平台为空。
    std::optional<PluginLaunch> plugin;
    bool watched_groups = false;  // 支持群聊代答(D5)
};

// 内置平台在前,插件平台在后;插件只在宿主构造前注册一次,之后不再变化。
const std::vector<PlatformSpec>& platform_specs();
// 替换已注册的插件平台。与内置平台或彼此重名的条目跳过(返回值不含它们)。
std::vector<std::string> register_plugin_platforms(std::vector<PlatformSpec> plugins);
// 平台名是否符合插件清单的命名规则(也是 Address::platform 的字符集)。
bool valid_platform_name(const std::string& name);
bool is_builtin_platform(const std::string& name);

// 测试用:构造时注册给定插件,析构时恢复之前的插件列表。
class ScopedPluginPlatforms {
public:
    explicit ScopedPluginPlatforms(std::vector<PlatformSpec> plugins);
    ~ScopedPluginPlatforms();
    ScopedPluginPlatforms(const ScopedPluginPlatforms&) = delete;
    ScopedPluginPlatforms& operator=(const ScopedPluginPlatforms&) = delete;

private:
    std::vector<PlatformSpec> previous_;
};
// 未知平台返回 nullptr。
const PlatformSpec* find_platform_spec(const std::string& name);
std::vector<std::string> platform_names();
// 展示名;未知平台原样返回。
std::string platform_label(const std::string& name);

// 已保存凭据里必填字段是否都有非空字符串值。
bool credentials_complete(const PlatformSpec& spec, const nlohmann::json& credentials);
// 绑定地址里的账号 id;凭据不全时可能为空。
std::string account_of(const PlatformSpec& spec, const nlohmann::json& credentials);

} // namespace acecode::channels::core
