#pragma once

// 记忆设置与条目管理(设置 > 个性化 > 记忆,openspec unify-memory-system)的 REST
// 纯函数层:请求解析与响应体构造。IO / 锁 / 下发在 routes_memory.cpp。

#include "config/config.hpp"
#include "memory/memory_service.hpp"
#include "memory/memory_types.hpp"
#include "session_host/memory_scheduler.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode::web {

// GET /api/config/memory 的响应体:
// {enabled, max_index_bytes, summary:{enabled, model_name, idle_minutes, max_session_age_days},
//  summary_available}
nlohmann::json memory_settings_json(const MemoryConfig& cfg, bool summary_available);

// 解析 PUT body(patch 语义:缺省键沿用 current)。类型不对返回 false 并写 error;
// 取值范围与模型名由 settings_mutations::set_memory_settings 统一校验。
bool parse_memory_settings_request(const nlohmann::json& body, const MemoryConfig& current,
                                   MemoryConfig& out, std::string& error);

// 单条条目:{scope, name, description, type, created_at, updated_at, source,
// source_sessions[, body, path]}
nlohmann::json memory_entry_json(MemoryScope scope, const MemoryEntry& entry, bool include_body);

// GET /api/memory:两个作用域的条目(读前重扫磁盘)与记忆摘要状态。
// project_dir 为空 = 没有选中工作区,workspace.available=false。
nlohmann::json memory_overview_json(MemoryService& memory, const std::string& project_dir,
                                    const MemorySummaryStatus& status);

struct MemoryEntryEdit {
    std::string description;
    std::string body;
    std::optional<MemoryType> type;
};

// PUT /api/memory/<scope>/<name> body {description, body, type?}。
bool parse_memory_entry_edit(const nlohmann::json& body, MemoryEntryEdit& out, std::string& error);

} // namespace acecode::web
