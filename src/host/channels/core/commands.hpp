#pragma once

// IM 内命令解析(纯逻辑)与中文提示文案(im-channels spec「IM 内命令」)。
// 识别的命令不会作为对话内容提交给模型;不认识的 /xxx 照普通文本处理(可能是技能命令)。
// 每个命令都有不超过两个字母的简写(/sessions = /s),简写与全名等价。

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::channels::core {

enum class CommandKind {
    None, Help, Status, Stop, New, Sessions, Resume, Model, Mode, Yolo, Effort, Approve, Deny, Question,
    // 与 Desktop 输入框相同的命令
    Compact, Init, Goal, Plan, Turn, Side, Lsp, Sandbox, Memory, Swarm,
    // IM 专有:技能列表、翻页、可引用会话列表(@ref)
    Skills, More, RefList
};

struct Command {
    CommandKind kind = CommandKind::None;
    std::string argument;  // 去掉命令名后的参数(已去两端空白)
    std::string error;     // 非空 = 用法错误,直接回复给用户
};

Command parse_command(const std::string& text);

// 会话忙碌时能否立即执行;/new 与 /resume 需要先 /stop 或等待完成。
bool runs_while_busy(CommandKind kind);

// 日志与提示里用的全名(带斜杠,@ref 为 "@ref")。
const char* command_name(CommandKind kind);
// 与 Desktop 同名、交给 daemon 内置命令执行的命令名(不带斜杠);其它命令返回空。
const char* builtin_command_name(CommandKind kind);

// 消息正文里的会话引用「@编号」:@ 前不是 ASCII 字母数字(或 . _ - @),后面是 1–3 位数字,
// 再往后不是 ASCII 字母数字。
struct SessionRefMention {
    std::size_t offset = 0;  // '@' 的字节位置
    std::size_t length = 0;  // 含 '@'
    std::size_t index = 0;   // 编号(1 起)
};
std::vector<SessionRefMention> find_session_ref_mentions(const std::string& text);

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
