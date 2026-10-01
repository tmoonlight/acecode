#pragma once

#include "system_prompt.hpp"

#include <cstdint>
#include <ostream>
#include <string>

namespace acecode {

class MemoryService;

struct MemorySnapshotSource {
    MemoryService* memory = nullptr;   // nullable;为空不注入
    std::string project_dir;           // 会话项目目录;空 = 没有工作区,只有全局作用域
    std::size_t max_index_bytes = 8 * 1024;
    std::int64_t now_seconds = 0;      // 计算「N 天前」的基准(epoch 秒)
};

// 渲染一份记忆上下文快照(openspec unify-memory-system D3):按作用域分段
// (全局 / 当前工作区),每段条目按更新时间倒序、每行附距今天数,超过
// max_index_bytes 的条目不列出、改为一行省略说明;末尾无条目时返回空块。
// 渲染前会从磁盘重扫两个作用域。调用方负责按会话冻结结果。
PromptContextBlock build_memory_snapshot_prompt(const MemorySnapshotSource& source);

// 「N 天前」标签:0 → today,1 → 1 day ago,n → n days ago;时间未知返回空串。
std::string memory_age_label(const std::string& iso_time, std::int64_t now_seconds);

// 静态 system prompt 的 # Memory 一节(记住的东西要用 memory_write 存,别写
// 临时笔记文件)。只在 memory_write 可用且本会话开启记忆时输出。
void append_memory_tool_guidance(std::ostream& out);

} // namespace acecode
