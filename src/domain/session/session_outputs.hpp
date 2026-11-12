#pragma once

// 会话「产物」:这次对话生成、交付给用户看的文件(报告、表格、图片……),
// 摘要面板的「产物」分组显示它。代码改动不算产物,归「变更」。
//
// 来源全部取自会话记录里已有的工具结果元数据,不改写文件的工具:
//   - file_write:tool_summary 的动词是 Created / Wrote,object 是路径;
//   - apply_patch:metadata.files 里 type == "add" 的条目;
//   - bash:metadata.output_files(命令结束后按修改时间找到的新文件,
//     见 tool/shell_output_files.hpp;后台命令在完成通知的
//     metadata.background_task.output_files 里);
//   - 子代理:派生结果的 metadata.subagent_session_id 指向子会话,它的产物
//     并进主会话的列表(SessionOutputsIndex 负责跟过去)。
//
// 这里只有纯逻辑(无 IO):取候选、判扩展名、过滤去重。读记录与查磁盘在
// session_outputs_index.cpp。

#include "llm/llm_provider.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace acecode {

enum class SessionOutputSource { Write, Patch, Shell };

const char* session_output_source_name(SessionOutputSource source);

struct SessionOutputCandidate {
    std::string path;   // 记录里的原始路径,可能是相对路径
    SessionOutputSource source = SessionOutputSource::Write;
};

// 文档 / 表格 / 图片 / 音视频 / 压缩包这类「拿来看或交付」的扩展名(不分大小写)。
// 工作目录内只有这类文件算产物;bash 探测也只认这类文件。
bool is_deliverable_file_name(std::string_view path);

// 不解析 JSON 的预筛:这一行有没有可能带产物候选或子会话 id。
bool jsonl_line_may_carry_outputs(std::string_view line);

// 一条消息里的产物候选(按记录顺序)。无关消息、失败的写入返回空。
std::vector<SessionOutputCandidate> session_output_candidates(const ChatMessage& message);

// 派生子代理的工具结果里记的子会话 id;没有返回空串。
std::string session_output_child_session_id(const ChatMessage& message);

struct SessionOutputRoots {
    // 会话文件夹(工作目录、worktree、附加文件夹),已归一成同一种路径形态。
    std::vector<std::string> folders;
    // 系统临时目录,同样已归一。
    std::vector<std::string> temp_roots;
};

struct SessionOutputEntry {
    std::string path;       // 归一后的绝对路径
    std::string relative;   // 落在会话文件夹内时的相对路径(正斜杠),否则为空
    int folder_index = -1;  // 所在会话文件夹在 roots.folders 里的下标;不在任何文件夹内为 -1
    SessionOutputSource source = SessionOutputSource::Write;
};

// candidates 按旧 → 新排列,path 必须已是归一后的绝对路径。返回按新 → 旧排列、
// 按路径去重(同一文件取最近一次)的结果。规则:
//   - 路径里有 .acecode / .git 这一级的不算(会话文件夹自身那一段除外,
//     worktree 会话的工作目录就在 .acecode/worktrees 下面);
//   - 会话文件夹内只留 is_deliverable_file_name 的文件;
//   - 会话文件夹外:系统临时目录里的不算,其余任何扩展名都算。
std::vector<SessionOutputEntry> select_session_outputs(
    const std::vector<SessionOutputCandidate>& candidates,
    const SessionOutputRoots& roots);

} // namespace acecode
