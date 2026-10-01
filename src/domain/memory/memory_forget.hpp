#pragma once

#include <string>

namespace acecode {

class MemoryService;

struct MemoryForgetResult {
    int observations_removed = 0;  // inbox / archive 中删除的观察条数
    int entries_updated = 0;       // 只移除了该会话来源的摘要条目
    int entries_deleted = 0;       // 失去全部来源而删除(并记墓碑)的摘要条目
};

// 会话被永久删除后撤回它对记忆的贡献(openspec unify-memory-system D14):
// 删除该会话在全局与该工作区 inbox / archive 中的观察;source: summary 的条目
// 从 source_sessions 里移除该会话,没有来源了就删除并记墓碑;手写条目不动;
// 清掉该会话的提炼进度。project_dir 为空时只处理全局作用域。
MemoryForgetResult forget_memory_session(MemoryService& memory,
                                         const std::string& session_id,
                                         const std::string& project_dir);

} // namespace acecode
