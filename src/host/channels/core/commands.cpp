#include "commands.hpp"

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

} // namespace

Command parse_command(const std::string& raw) {
    Command command;
    const auto text = trim(raw);
    if (text.empty() || text[0] != '/') return command;
    const auto space = text.find_first_of(" \t\r\n");
    const auto name = lower(text.substr(0, space));
    command.argument = space == std::string::npos ? std::string{} : trim(text.substr(space));
    if (name == "/help" || name == "/start") command.kind = CommandKind::Help;
    else if (name == "/status") command.kind = CommandKind::Status;
    else if (name == "/stop") command.kind = CommandKind::Stop;
    else if (name == "/new") command.kind = CommandKind::New;
    else if (name == "/sessions" || name == "/session") command.kind = CommandKind::Sessions;
    else if (name == "/resume") command.kind = CommandKind::Resume;
    else if (name == "/model" || name == "/models") command.kind = CommandKind::Model;
    else if (name == "/approve") command.kind = CommandKind::Approve;
    else if (name == "/deny") command.kind = CommandKind::Deny;
    else if (name == "/aq") command.kind = CommandKind::Question;
    else return Command{};

    if (command.kind == CommandKind::Resume && command.argument.empty())
        command.error = "用法:/resume <编号或会话 ID>。先发 /sessions 查看可切换的会话。";
    if ((command.kind == CommandKind::Approve || command.kind == CommandKind::Deny) &&
        (command.argument.empty() || command.argument.find(' ') != std::string::npos))
        command.error = "用法:/approve <编号> 或 /deny <编号>。";
    if (command.kind == CommandKind::Sessions && !command.argument.empty()) {
        const auto lowered = lower(command.argument);
        if (lowered != "more" && lowered != "all" && lowered.rfind("search ", 0) != 0)
            command.error = "用法:/sessions、/sessions more 或 /sessions search <关键词>。";
    }
    return command;
}

bool runs_while_busy(CommandKind kind) { return kind != CommandKind::New && kind != CommandKind::Resume; }

namespace texts {

std::string help(bool owner) {
    std::string text =
        "可用命令:\n"
        "/status 查看当前会话、模型和待处理事项\n"
        "/stop 停止当前回合\n"
        "/new 新建会话\n"
        "/sessions 列出可切换的会话(more 查看更多,search <关键词> 搜索)\n"
        "/resume <编号或会话 ID> 切换到某个会话\n"
        "/model [名称] 查看或切换模型\n"
        "/approve <编号>、/deny <编号> 处理权限请求\n"
        "/aq 回答提问\n"
        "其它文字会直接发给当前会话。";
    if (!owner) text += "\n(你只能在自己创建的会话之间切换。)";
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
