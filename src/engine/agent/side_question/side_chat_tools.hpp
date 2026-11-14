#pragma once

#include "side_chat.hpp"
#include "tool/tool_executor.hpp"

namespace acecode { class PermissionManager; class SessionManager; }

namespace acecode::agent {

class WorkspaceBoundary;

// 侧边对话放出的只读内置工具。刻意用白名单而不是 ToolImpl::is_read_only:后者
// 是「免确认」标记,spawn_subagent / AskUserQuestion / goal 工具也带着它。
inline constexpr const char* kSideChatReadOnlyTools[] = {"file_read", "grep", "glob", "lsp"};

// 按会话当前的工具注册与专家能力策略组装侧边对话工具集(MCP 一律不放出)。
// 每次调用依次过:白名单 → 权限规则(Deny 命中即拒,侧边对话没有确认通道)→
// 与主会话同一套路径校验 → 危险路径拒绝,然后在旁路读取作用域里执行,不碰
// 主代理的读取基线与重复读取缓存。返回的回调借用全部引用,只能在调用方持有
// 会话期间同步使用。send_to_main 非空时另外放出「发到主会话」工具,调用直接
// 转给它(用户确认与送达都由它负责)。
SideChatToolset build_side_chat_toolset(ToolExecutor& tools,
                                        PermissionManager& permissions,
                                        WorkspaceBoundary& boundary,
                                        SessionManager* session,
                                        const ToolCapabilityPolicy& session_policy,
                                        const SideChatMainSessionSender& send_to_main = {},
                                        const std::function<bool(const std::string&)>& extra_read_denied = {});

} // namespace acecode::agent
