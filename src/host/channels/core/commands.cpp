#include "commands.hpp"

#include "config/vocab/permission_mode.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::channels::core {
namespace {

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

bool all_digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

struct CommandSpec {
    CommandKind kind;
    const char* name;      // 全名(带前缀)
    const char* aliases;   // 空格分隔的其它写法,第一个是简写
};

// 唯一的命令表:全名、简写与兼容写法。简写不超过两个字母,彼此不重复。
constexpr CommandSpec kCommands[] = {
    {CommandKind::Help, "/help", "/h /start"},
    {CommandKind::Status, "/status", "/st"},
    {CommandKind::Stop, "/stop", "/x"},
    {CommandKind::New, "/new", "/n"},
    {CommandKind::Sessions, "/sessions", "/s /session"},
    {CommandKind::Resume, "/resume", "/r"},
    {CommandKind::Model, "/model", "/m /models"},
    {CommandKind::Mode, "/mode", "/pm"},
    {CommandKind::Yolo, "/yolo", "/y"},
    {CommandKind::Effort, "/effort", "/e"},
    {CommandKind::Approve, "/approve", "/a"},
    {CommandKind::Deny, "/deny", "/d"},
    {CommandKind::Question, "/aq", ""},
    {CommandKind::Compact, "/compact", "/c"},
    {CommandKind::Init, "/init", "/i"},
    {CommandKind::Goal, "/goal", "/g"},
    {CommandKind::Plan, "/plan", "/p"},
    {CommandKind::Turn, "/turn", "/t"},
    {CommandKind::Side, "/btw", "/b /side"},
    {CommandKind::Lsp, "/lsp", "/l"},
    {CommandKind::Sandbox, "/sandbox", "/sb"},
    {CommandKind::Memory, "/memory", "/me"},
    {CommandKind::Swarm, "/swarm", "/sw"},
    {CommandKind::Skills, "/skills", "/sk"},
    {CommandKind::More, "/more", "/mr"},
    {CommandKind::RefList, "@ref", "@r /ref"},
};

bool matches_alias(const char* aliases, const std::string& name) {
    const std::string list = aliases;
    std::size_t pos = 0;
    while (pos < list.size()) {
        const auto end = std::min(list.find(' ', pos), list.size());
        if (list.compare(pos, end - pos, name) == 0) return true;
        pos = end + 1;
    }
    return false;
}

const CommandSpec* find_spec(const std::string& name) {
    for (const auto& spec : kCommands)
        if (name == spec.name || matches_alias(spec.aliases, name)) return &spec;
    return nullptr;
}

const CommandSpec* spec_for(CommandKind kind) {
    for (const auto& spec : kCommands)
        if (spec.kind == kind) return &spec;
    return nullptr;
}

void validate(Command& command) {
    const auto& argument = command.argument;
    switch (command.kind) {
        case CommandKind::Resume:
            if (argument.empty()) command.error = "用法:/resume <编号或会话 ID>(简写 /r)。先发 /s 查看可切换的会话。";
            break;
        case CommandKind::Approve:
        case CommandKind::Deny:
            if (argument.empty() || argument.find(' ') != std::string::npos)
                command.error = "用法:/approve <编号>(/a)或 /deny <编号>(/d)。";
            break;
        case CommandKind::Mode:
            if (!argument.empty()) {
                auto mode = parse_permission_mode_name(argument);
                if (!mode) mode = parse_permission_mode_name(lower(argument));
                if (!mode || *mode == PermissionMode::Plan)
                    command.error = "用法:/mode [default|auto|yolo](简写 /pm)。分别为默认权限、自动模式、完全访问权限。";
                else
                    command.argument = permission_mode_name(*mode);
            }
            break;
        case CommandKind::Yolo:
            if (!argument.empty()) command.error = "用法:/yolo(/y)。直接切换到完全访问权限,无需参数。";
            break;
        case CommandKind::Effort:
            command.argument = lower(argument);
            if (command.argument.find_first_of(" \t\r\n") != std::string::npos)
                command.error = "用法:/effort [档位|default](简写 /e)。发送 /e 查看当前模型支持的档位。";
            break;
        case CommandKind::Compact:
        case CommandKind::Init:
            if (!argument.empty())
                command.error = std::string("用法:") + spec_for(command.kind)->name + ",不需要参数。";
            break;
        case CommandKind::Turn:
            if (argument.empty())
                command.error = "用法:/turn <补充说明>(简写 /t)。把补充说明加入正在执行的回合,不打断它。";
            break;
        case CommandKind::Side:
            if (argument.empty())
                command.error = "用法:/btw <问题>(简写 /b)。基于当前会话的上下文旁问一句,不影响正在执行的回合。";
            break;
        case CommandKind::Swarm: {
            command.argument = lower(argument);
            const auto& mode = command.argument;
            if (!mode.empty() && mode != "star" && mode != "mesh" && mode != "off")
                command.error = "用法:/swarm [star|mesh|off](简写 /sw)。不带参数查看当前蜂群模式。";
            break;
        }
        case CommandKind::More:
            if (!argument.empty() && (!all_digits(argument) || argument.size() > 4))
                command.error = "用法:/more [页码](简写 /mr)。不带页码时看下一页。";
            break;
        default:
            break;
    }
}

} // namespace

Command parse_command(const std::string& raw) {
    Command command;
    const auto text = trim(raw);
    if (text.empty() || (text[0] != '/' && text[0] != '@')) return command;
    const auto space = text.find_first_of(" \t\r\n");
    const auto name = lower(text.substr(0, space));
    const auto* spec = find_spec(name);
    if (!spec) return command;
    command.kind = spec->kind;
    command.argument = space == std::string::npos ? std::string{} : trim(text.substr(space));
    validate(command);
    return command;
}

bool runs_while_busy(CommandKind kind) { return kind != CommandKind::New && kind != CommandKind::Resume; }

const char* command_name(CommandKind kind) {
    const auto* spec = spec_for(kind);
    return spec ? spec->name : "(text)";
}

const char* builtin_command_name(CommandKind kind) {
    switch (kind) {
        case CommandKind::Compact: return "compact";
        case CommandKind::Init: return "init";
        case CommandKind::Goal: return "goal";
        case CommandKind::Plan: return "plan";
        case CommandKind::Lsp: return "lsp";
        case CommandKind::Sandbox: return "sandbox";
        case CommandKind::Memory: return "memory";
        case CommandKind::Swarm: return "swarm";
        default: return "";
    }
}

std::vector<SessionRefMention> find_session_ref_mentions(const std::string& text) {
    // 中文里常不加空格(「总结@1和@2」),所以 @ 前只排除 ASCII 字母数字与 . _ - @,
    // 邮箱 a@163.com 这类不会被当成引用。
    const auto word_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-' || c == '@';
    };
    std::vector<SessionRefMention> mentions;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '@' || (i > 0 && word_char(text[i - 1]))) continue;
        std::size_t end = i + 1;
        while (end < text.size() && end - i <= 3 && text[end] >= '0' && text[end] <= '9') ++end;
        const auto digits = end - i - 1;
        if (digits == 0 || digits > 3) continue;
        // 后面紧跟 ASCII 字母数字(@12ab、@1234)不算引用;中文、标点、空白都可以。
        if (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end]))) continue;
        mentions.push_back({i, end - i, static_cast<std::size_t>(std::stoul(text.substr(i + 1, digits)))});
        i = end - 1;
    }
    return mentions;
}

namespace texts {

std::string help(bool owner) {
    std::string text =
        "可用命令(括号内是简写):\n"
        "【会话】\n"
        "/status(/st) 当前会话、模型和待处理事项\n"
        "/stop(/x) 停止当前回合\n"
        "/new(/n) 新建会话\n"
        "/sessions(/s) [关键词|页码] 列出或搜索可切换的会话\n"
        "/resume(/r) <编号|会话 ID> 切换会话\n"
        "@ref(@r) [关键词|页码] 列出可引用的会话;之后在消息里写 @编号 引用它的上下文\n"
        "/more(/mr) [页码] 翻看上一条列表或长回复的下一页\n"
        "【执行】\n"
        "/turn(/t) <补充说明> 把补充说明加入正在执行的回合\n"
        "/btw(/b) <问题> 基于当前上下文旁问,不影响回合\n"
        "/compact(/c) 压缩对话上下文\n"
        "/init(/i) 分析项目并生成 AGENTS.md\n"
        "/goal(/g) [目标|pause|resume|clear] 查看或设置线程目标\n"
        "/plan(/p) [任务] 进入计划模式\n"
        "/swarm(/sw) [star|mesh|off] 查看或切换蜂群模式\n"
        "/skills(/sk) [关键词|页码] 列出可用技能,发送 /技能名 使用\n"
        "【设置】\n"
        "/model(/m) [名称] 查看或切换模型\n"
        "/mode(/pm) [default|auto|yolo] 查看或切换权限模式\n"
        "/yolo(/y) 切换到完全访问权限\n"
        "/effort(/e) [档位|default] 查看或切换思考深度\n"
        "/sandbox(/sb) [on|off] 查看或开关命令沙盒\n"
        "/memory(/me) [list|view|forget|flush|on|off] 管理记忆\n"
        "/lsp(/l) 语言服务器状态\n"
        "【确认】\n"
        "/approve(/a) <编号>、/deny(/d) <编号> 处理权限请求\n"
        "/aq 回答提问\n"
        "其它文字会直接发给当前会话。";
    if (!owner) text += "\n(你只能在自己创建的会话之间切换和引用;/memory 只有机主可用。)";
    return text;
}

std::string pairing_notice() {
    return "你还没有被授权使用这个 ACECode 机器人。已向机主发送请求,机主在 ACECode 设置页批准后,"
           "请重新发送消息。请求 10 分钟后失效。";
}

std::string owner_bound() { return "已绑定为机主。现在可以直接发消息给 ACECode 了,发送 /help 查看命令。"; }

std::string busy_reject() { return "当前会话正在执行,请先 /stop 或等它完成再切换会话。"; }

std::string unsupported_media(const std::string& kind) { return "暂不支持" + kind + "消息,请发送文字、图片或文件。"; }

std::string session_missing() {
    return "无法打开当前绑定的会话(可能已被删除)。发送 /new 新建会话,或 /sessions 选择其它会话。";
}

std::string no_binding() { return "当前还没有会话。直接发消息即可开始,或发送 /sessions 选择已有会话。"; }

std::string resume_denied() { return "只能切换到你自己通过这里创建的会话。发送 /sessions 查看可切换的会话。"; }

std::string bound_elsewhere(const std::string& where) {
    return "当前会话已转到" + where + "。之后的回复会发到那里;在这里继续发消息会开一个新会话。";
}

std::string transferred_here(const std::string& title) { return "已切换到会话:" + title; }

std::string new_session(const std::string& location) { return "已新建会话(" + location + ")。"; }

std::string stop_requested() { return "已请求停止当前回合。"; }

std::string permission_prompt(const std::string& request_id, const std::string& tool, const std::string& args) {
    return "需要你确认一个操作:" + tool + "\n" + args + "\n回复 /approve " + request_id + " 允许(仅这一次),或 /deny " +
           request_id + " 拒绝。";
}

std::string permission_closed(const std::string& request_id, const std::string& choice) {
    return "权限请求 " + request_id + " 已处理(" + choice + ")。";
}

std::string permission_unknown() { return "该权限请求已处理或不存在。"; }

std::string permission_submitted(const std::string& request_id, bool approved) {
    return std::string(approved ? "已允许" : "已拒绝") + "权限请求 " + request_id + "。";
}

std::string turn_failed(const std::string& reason) { return "处理失败:" + reason; }

std::string input_rejected() { return "会话没有接受这条消息,请稍后重试。"; }

std::string model_switched(const std::string& name) { return "已切换模型为 " + name + ",从下一回合起生效。"; }

std::string model_unknown(const std::string& name, const std::vector<std::string>& names) {
    std::string text = "没有名为 " + name + " 的模型。可用模型:";
    for (const auto& item : names) text += "\n" + item;
    return text;
}

} // namespace texts

} // namespace acecode::channels::core
