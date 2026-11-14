#pragma once
#include "peer_store.hpp"
#include "tool/tool_executor.hpp"
#include <memory>

namespace acecode::peers {
// The composition root owns the store; tool closures only retain weak references.
void register_peer_tools(ToolExecutor& tools, const std::shared_ptr<PeerStore>& store);
} // namespace acecode::peers
