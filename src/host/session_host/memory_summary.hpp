#pragma once

#include "llm/llm_provider.hpp"
#include "memory/memory_inbox.hpp"
#include "memory/memory_state_store.hpp"
#include "memory/memory_types.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode {

// ---- 提炼(openspec unify-memory-system D12)--------------------------------

enum class MemoryTranscriptKind {
    User,                   // 用户原话
    AssistantFinal,         // 助手最终结论(不带工具调用的回复)
    AssistantIntermediate,  // 助手中间说明(随工具调用一起出现的文字)
    Tool,                   // 工具输出
};

struct MemoryTranscriptItem {
    MemoryTranscriptKind kind = MemoryTranscriptKind::User;
    std::string text;
};

inline constexpr std::size_t kMemoryToolOutputCapBytes = 2048;

// 取规范会话记录 [from, end) 中用户可见的部分:真实用户消息(显示文本优先)、
// 助手正文与工具结果;隐藏的内部上下文、压缩摘要、技能展开片段、系统消息不进。
std::vector<MemoryTranscriptItem> collect_memory_transcript(
    const std::vector<ChatMessage>& messages, std::size_t from);

// 按「用户原话 > 助手最终结论 > 助手中间说明 > 工具输出」逐级裁剪到预算内
// (工具输出先截到单条 2 KB),再按原顺序渲染。
std::string render_memory_transcript(const std::vector<MemoryTranscriptItem>& items,
                                     std::size_t budget_bytes);

struct ParsedMemoryObservation {
    MemoryScope scope = MemoryScope::Global;
    MemoryType type = MemoryType::User;
    std::string title;
    std::string statement;
};

struct MemoryExtractionParse {
    bool ok = false;
    std::string error;
    bool noop = false;
    std::vector<ParsedMemoryObservation> observations;
};

// 严格校验模型输出:{"outcome":"noop"|"observations","observations":[...]},
// 每条观察恰好含 scope/type/title/statement,title ≤ 80 字符、statement ≤ 1000
// 字符,最多 20 条;多余字段、非法取值、超长都使整份输出作废。
MemoryExtractionParse parse_memory_extraction_output(const std::string& text);

std::string memory_extraction_system_prompt();
std::string memory_extraction_user_prompt(const std::string& session_id,
                                          std::int64_t from, std::int64_t to,
                                          const std::string& transcript);

// ---- 整合(D13)--------------------------------------------------------------

struct MemoryPlanOperation {
    std::string op;                    // create | update | merge | delete
    std::string name;
    std::optional<MemoryType> type;
    std::string description;
    std::string body;
    std::vector<std::string> sources;  // merge 的被合并条目
    std::vector<std::string> evidence; // 引用的观察 id
};

struct MemoryPlanParse {
    bool ok = false;
    std::string error;
    std::vector<MemoryPlanOperation> operations;
    std::string canonical;             // 规范化 JSON(计算计划 hash、持久化待应用计划)
};

MemoryPlanParse parse_memory_consolidation_plan(const std::string& text);

struct MemoryPlanContext {
    std::vector<MemoryEntry> entries;
    std::set<std::string> observation_ids;
    std::vector<MemoryTombstone> tombstones;
};

// 整体校验计划,返回空串表示通过,否则返回第一条违规原因。规则:每个操作引用
// 本批观察;名字合法;source: manual(含没有 source 的旧条目)只读;不得创建墓碑
// 中的名字或标题(忽略大小写);描述 ≤ 150 字符、正文 ≤ 4 KiB;同一条目只被一个
// 操作触及。
std::string validate_memory_plan(const std::vector<MemoryPlanOperation>& operations,
                                 const MemoryPlanContext& context);

std::string memory_consolidation_system_prompt();
std::string memory_consolidation_user_prompt(MemoryScope scope,
                                             const std::vector<MemoryEntry>& entries,
                                             const std::vector<MemoryObservationFile>& batch,
                                             const std::vector<MemoryTombstone>& tombstones);

std::string memory_plan_hash(const std::string& canonical_plan);

// UTF-8 码点数(标题 / 描述长度按字符计)。
std::size_t memory_utf8_length(const std::string& text);

} // namespace acecode
