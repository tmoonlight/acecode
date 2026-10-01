#pragma once

#include "memory_summary.hpp"

#include "memory/memory_service.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace acecode {

struct MemoryConsolidationApply {
    bool ok = false;
    std::string error;
    int changed_entries = 0;
};

// 在作用域写锁内应用已校验的整合计划(D13):写条目(source: summary,
// source_sessions 取所引用观察的来源会话)、重建索引、把本批观察文件整体移进
// archive/<archive_date>/、记录计划 hash。任一步失败,条目与收件箱回滚到应用前
// 状态。replay=true 用于崩溃后重放同一份待应用计划:已建好的条目按 upsert 处理、
// 已删掉的不再报错,保证不重复写入。
MemoryConsolidationApply apply_memory_plan(MemoryService& memory,
                                           MemoryRegistry& registry,
                                           const std::vector<MemoryPlanOperation>& operations,
                                           const std::vector<MemoryObservationFile>& batch,
                                           const std::string& plan_hash,
                                           const std::string& archive_date,
                                           std::int64_t now_ms,
                                           bool replay = false);

} // namespace acecode
