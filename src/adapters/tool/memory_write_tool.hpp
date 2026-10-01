#pragma once

#include "tool_executor.hpp"

#include <memory>

namespace acecode {

class MemoryService;

// `memory_write` — create / update / upsert a memory entry in the global or the
// session's workspace scope. Writes are atomic, redact secrets first, record
// provenance (source: manual, the current session, timestamps) and update the
// scope's MEMORY.md under the cross-process write lock. NOT read-only, but
// auto-approved in non-Yolo modes because the target path is derived from a
// sanitized name inside a scope directory and anything resolving outside it
// (including through a symlink) is rejected — see permissions.hpp.
ToolImpl create_memory_write_tool(std::shared_ptr<MemoryService> memory);

} // namespace acecode
