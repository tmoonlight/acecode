#pragma once

// IM 内命令解析(纯逻辑)与中文提示文案(im-channels spec「IM 内命令」)。
// 识别的命令不会作为对话内容提交给模型;不认识的 /xxx 照普通文本处理(可能是技能命令)。

#include <string>
#include <vector>

namespace acecode::channels::core {

enum class CommandKind { None, Help, Status, Stop, New, Sessions, Resume, Model, Approve, Deny, Question };

struct Command {
    CommandKind kind = CommandKind::None;
    std::string argument;  // 去掉命令名后的参数(已去两端空白)
    std::string error;     // 非空 = 用法错误,直接回复给用户
};

Command parse_command(const std::string& text);

// 会话忙碌时能否立即执行;/new 与 /resume 需要先 /stop 或等待完成。
bool runs_while_busy(CommandKind kind);

namespace texts {

std::string help(bool owner);
std::string pairing_notice();
std::string owner_bound();
std::string busy_reject();
std::string unsupported_media(const std::string& kind);
std::string session_missing();
std::string no_binding();
std::string resume_denied();
std::string bound_elsewhere(const std::string& where);
std::string transferred_here(const std::string& title);
std::string new_session(const std::string& location);
std::string stop_requested();
std::string permission_prompt(const std::string& request_id, const std::string& tool, const std::string& args);
std::string permission_closed(const std::string& request_id, const std::string& choice);
std::string permission_unknown();
std::string permission_submitted(const std::string& request_id, bool approved);
std::string turn_failed(const std::string& reason);
std::string input_rejected();
std::string model_switched(const std::string& name);
std::string model_unknown(const std::string& name, const std::vector<std::string>& names);

} // namespace texts

} // namespace acecode::channels::core
