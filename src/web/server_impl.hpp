// server_impl.hpp — Internal header exposing WebServer::Impl for route TUs.
// NOT installed; never included outside src/web/ and src/web/routes/.
#pragma once

#include "server.hpp"

#include "auth.hpp"
#include "origin.hpp"
#include "remote_web.hpp"
#include "static_assets.hpp"
#include "../config/config.hpp"
#include "../config/saved_models_editor.hpp"
#include "../config/request_headers.hpp"
#include "../desktop/workspace_registry.hpp"
#include "../hooks/hook_manager.hpp"
#include "../loop/loop_store.hpp"
#include "../provider/auth/github_auth.hpp"
#include "../provider/llm_provider.hpp"
#include "../provider/model_pool_status.hpp"
#include "../session/ask_user_question_prompter.hpp"
#include "../session/attachment_store.hpp"
#include "../session/local_session_client.hpp"
#include "../session/opencode_import.hpp"
#include "../session/session_attention.hpp"
#include "../session/session_client.hpp"
#include "../session/session_registry.hpp"
#include "../session/session_rewind.hpp"
#include "../session/session_serializer.hpp"
#include "../session/session_markdown_export.hpp"
#include "../session/session_storage.hpp"
#include "../session/todo_state.hpp"
#include "../session/session_usage_ledger.hpp"
#include "../session/session_writer_lease.hpp"
#include "../daemon/platform.hpp"
#include "../skills/skill_registry.hpp"
#include "../experts/expert_registry.hpp"
#include "../skills/skill_metadata.hpp"
#include "../tool/ace_browser_bridge/browser_tools.hpp"
#include "../tool/tool_executor.hpp"
#include "../upgrade/apply.hpp"
#include "../upgrade/check.hpp"
#include "../utils/logger.hpp"
#include "../utils/base64.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/terminal_title.hpp"
#include "handlers/files_handler.hpp"
#include "handlers/fork_handler.hpp"
#include "handlers/history_handler.hpp"
#include "handlers/models_handler.hpp"
#include "handlers/permission_mode_handler.hpp"
#include "handlers/pinned_sessions_handler.hpp"
#include "handlers/builtin_command_handler.hpp"
#include "handlers/commands_handler.hpp"
#include "handlers/opencode_command_expander.hpp"
#include "handlers/skill_command_expander.hpp"
#include "handlers/skills_handler.hpp"
#include "../skills/skill_init.hpp"
#include "message_payload.hpp"
#include "pty/pty_session_registry.hpp"
#include "version.hpp"

// Crow 头一定在 ASIO_STANDALONE PUBLIC 定义之后才 include。CMakeLists.txt 已
// 给 acecode_testable 加 PUBLIC 的 ASIO_STANDALONE,所以这里直接 include 即可。
#include <crow.h>

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "../network/proxy_resolver.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef DELETE
#undef DELETE
#endif
#ifdef GET
#undef GET
#endif
#ifdef POST
#undef POST
#endif
#ifdef PUT
#undef PUT
#endif

namespace acecode::web {

// =====================================================================
// WsConnState — per-WebSocket-connection state
// =====================================================================
struct WsConnState {
    std::string session_id;
    std::unordered_map<std::string, SessionClient::SubscriptionId> subscriptions;
    std::unordered_set<std::string> status_workspaces;
    std::unordered_set<std::string> status_sessions;
};

// =====================================================================
// SelectionPromptContext — used by session send handler
// =====================================================================
struct SelectionPromptContext {
    nlohmann::json meta = nlohmann::json::array();
    std::string prompt;
};

struct ParsedSessionUserInputRequest {
    bool ok = false;
    int status = 400;
    std::string error;
    UserInput input;
    bool worktree_create = false;
    std::string worktree_base;
    std::string expected_turn_id;
};

// =====================================================================
// Anonymous-namespace free functions shared across route TUs
// (defined in server_helpers.cpp, declared here so routes can use them)
// =====================================================================

nlohmann::json session_event_to_json(const SessionEvent& evt,
                                      const std::string& session_id = {},
                                      const std::string& workspace_hash = {},
                                      const std::string& cwd = {});

nlohmann::json chat_message_to_json(const ChatMessage& m);
nlohmann::json ui_preferences_to_json(const WebUiPreferencesConfig& cfg);
nlohmann::json custom_instructions_to_json(const CustomInstructionsConfig& cfg);
nlohmann::json upgrade_config_to_json(const UpgradeConfig& cfg);
nlohmann::json update_check_to_json(const acecode::upgrade::UpdateCheckResult& result);
nlohmann::json ace_browser_bridge_settings_to_json(const AceBrowserBridgeConfig& cfg);

bool cwd_is_directory(const std::string& cwd);
bool has_non_whitespace(const std::string& value);
std::string json_string_field(const nlohmann::json& object, const char* key);
int json_positive_int_field(const nlohmann::json& object, const char* key);
std::string truncate_selection_context_text(std::string text);
std::string selection_line_suffix(const nlohmann::json& source);
std::optional<nlohmann::json> sanitized_selection_context_meta(const nlohmann::json& ctx);
SelectionPromptContext build_selection_prompt_context(const nlohmann::json& contexts);
std::string build_selection_augmented_prompt(const SelectionPromptContext& selection,
                                              const std::string& original_text);

std::uint64_t parse_seq(const std::string& s);
std::string trim_trailing_slash(std::string value);
std::optional<std::string> preview_blob_mime(const std::string& path);
std::int64_t now_unix_ms();
std::string ascii_lower(std::string s);
bool is_loopback_origin(const std::string& origin);
bool is_loopback_host(const std::string& host);
bool is_same_request_origin(const crow::request& req, const std::string& origin);
void log_unauthorized(const std::string& path, const std::string& client_ip, const char* reason);
AuthResult check_explicit_token(std::string_view server_token,
                                 std::string_view header_token,
                                 std::string_view query_token);

constexpr std::size_t kMaxSelectionContextChars = 40000;
constexpr std::size_t kMaxSelectionAnnotationChars = 4000;
constexpr std::size_t kMaxSelectionAnnotations = 64;

struct UpdateJobStatus {
    std::string job_id;
    std::string state = "pending";
    std::string phase = "checking";
    std::string current_version;
    std::string target_version;
    std::uintmax_t bytes_downloaded = 0;
    std::optional<std::uintmax_t> bytes_total;
    std::string backup_dir;
    std::string error;
    bool restart_required = false;
};

struct UpdateJobRuntime {
    std::mutex mu;
    std::optional<UpdateJobStatus> current;
};

// =====================================================================
// WebServer::Impl — hidden pimpl implementation
// =====================================================================
struct WebServer::Impl {
    WebServerDeps              deps;
    // Captured after CLI/Desktop overrides are applied. Settings mutations
    // persist config-file values, but a live bind change must never move the
    // already-running daemon to a different port.
    const int                  runtime_port;
    crow::SimpleApp            app;

    // 静态资源 source(EmbeddedAssetSource / FileSystemAssetSource),按
    // web.static_dir 路径在 register_routes 前实例化。
    std::unique_ptr<AssetSource> assets;

    // ws 注册表: 把 listener / state 与 connection 绑定,断开时清理。
    std::mutex                                                      ws_mu;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<WsConnState>> ws_connections;

    // Crow runs HTTP handlers on multiple worker threads. deps.app_config is a
    // shared mutable object, so every web-side read/write must go through this.
    mutable std::mutex app_config_mu;

    // A remote-mode change restarts only Crow's listener. Terminal shutdown
    // always wins over a pending rebind, so the daemon cannot accidentally
    // come back after its owner has requested exit.
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> rebind_requested{false};
    mutable std::mutex listener_state_mu;
    std::string effective_bind;
    int effective_port = 0;
    std::mutex listener_stop_mu;
    std::mutex rebind_thread_mu;
    std::thread rebind_stop_thread;

    // 从磁盘重读 saved_models 合并进内存 —— 连接器钩子(外部登录器)会直接
    // 改写 config.json;不重读的话,下一次任何 save_config 都会把新写入的
    // api_key 抹掉。  (defined in server_helpers.cpp)
    void refresh_saved_models_from_disk();

    mutable std::mutex attention_mu;
    mutable std::unordered_set<std::string> loaded_attention_workspaces;
    mutable std::unordered_map<std::string, std::string> attention_workspace_cwds;
    mutable std::unordered_map<std::string, std::unordered_map<std::string, SessionAttentionRecord>> attention_by_workspace;

    // Attention 落盘节流。
    //
    // note_session_event_for_attention 跑在**发射事件的 AgentLoop worker 线程**
    // 上(EventDispatcher::emit 同步 drain 订阅者),而 Token / Reasoning /
    // Tool* 事件全都会推进 update_cursor + updated_at_ms。改造前每个这样的
    // 事件都会立刻整份重写 workspace 的 attention 文件(tmp + rename)——
    // 实测流式期间事件峰值约 500/s(feedback IQSZ-D0668,日志里相邻两条
    // lastSeq 差值 45 / 92ms),等于每秒几百次文件重写,还是在持 attention_mu
    // 的情况下,并且多会话并发时写的是同一个文件。
    //
    // 现在热路径只标脏,真正落盘由后台 flusher 线程按 kAttentionFlushIntervalMs
    // 合并;状态跃迁(read↔unread↔in_progress / busy 翻转)这种回合边界事件
    // 仍然立即落盘。最坏情况是异常退出丢掉最多一个 flush 周期的游标推进,
    // 下一个事件会重新标记,不影响正确性。
    static constexpr int kAttentionFlushIntervalMs = 1000;
    mutable std::unordered_set<std::string> attention_dirty_workspaces;
    std::condition_variable attention_flush_cv;
    bool attention_flush_stop = false;
    std::thread attention_flush_thread;

    struct SubagentTrackerState {
        std::mutex mu;
        Impl* impl = nullptr;
    };
    std::shared_ptr<SubagentTrackerState> subagent_tracker_state =
        std::make_shared<SubagentTrackerState>();
    mutable std::mutex tracked_subagents_mu;
    std::unordered_map<std::string, SessionClient::SubscriptionId> tracked_subagent_subscriptions;

    mutable std::mutex opencode_import_mu;
    mutable std::unordered_map<std::string, OpencodeImportJobStatus> opencode_import_jobs;

    std::shared_ptr<UpdateJobRuntime> update_job_runtime =
        std::make_shared<UpdateJobRuntime>();

    explicit Impl(WebServerDeps d)
        : deps(std::move(d)),
          runtime_port(deps.web_cfg ? deps.web_cfg->port : 0) {
        subagent_tracker_state->impl = this;
        start_attention_flusher();
    }
    ~Impl();

    void request_listener_rebind();
    void join_rebind_stop_thread();
    nlohmann::json remote_web_state_json(const crow::request& req) const;

    // -----------------------------------------------------------------
    // 鉴权 helper  (defined in server_helpers.cpp)
    // -----------------------------------------------------------------
    AuthResult auth_result_for_request(const crow::request& req,
                                       const std::string& header_token,
                                       const std::string& query_token) const;

    std::optional<crow::response> require_auth(const crow::request& req);
    void add_cors(const crow::request& req, crow::response& resp);
    crow::response with_cors(const crow::request& req, crow::response resp);
    crow::response cors_preflight(const crow::request& req);

    // -----------------------------------------------------------------
    // Workspace helper  (defined in server_helpers.cpp)
    // -----------------------------------------------------------------
    std::string projects_dir() const;
    acecode::desktop::WorkspaceMeta compatibility_workspace() const;
    std::optional<acecode::desktop::WorkspaceMeta> resolve_workspace(const std::string& hash) const;
    bool archived_query_requested(const crow::request& req) const;
    UsageLedgerQuery usage_query_from_request(const crow::request& req) const;
    std::vector<UsageLedgerScope> usage_scopes_for_request(const std::string& workspace_hash) const;
    std::vector<std::string> allowed_file_cwds() const;
    nlohmann::json workspace_to_json(const acecode::desktop::WorkspaceMeta& m) const;

    // -----------------------------------------------------------------
    // Session serialization helper  (defined in server_helpers.cpp)
    // -----------------------------------------------------------------
    static bool token_usage_has_values(const TokenUsage& usage);
    static nlohmann::json token_usage_to_json(const TokenUsage& usage);
    static nlohmann::json token_usage_or_null(const TokenUsage& usage);
    bool session_model_deleted(const std::string& model_name) const;
    nlohmann::json session_info_to_json(const SessionInfo& s, const SessionMeta* m) const;
    nlohmann::json session_meta_to_json(const SessionMeta& m, const std::string& workspace_hash) const;
    void append_session_runtime_snapshot(nlohmann::json& wrapper, const std::string& session_id) const;
    ParsedSessionUserInputRequest parse_session_user_input_request(
        const std::string& body,
        const std::string& session_id,
        bool allow_worktree);
    // parent_filter 语义:空 = 常规列表,排除所有 spawn_subagent 子会话;
    // 非空 = 后台任务查询,只返回 parent_session_id == parent_filter 的子会话
    // (active 部分不做 workspace 过滤,子会话跟随父会话归属)。
    nlohmann::json sessions_for_workspace(const acecode::desktop::WorkspaceMeta& ws,
                                          bool archived_only = false,
                                          bool include_no_workspace = false,
                                          const std::string& parent_filter = {}) const;
    bool session_entry_matches_workspace(const SessionEntry& entry,
                                          const acecode::desktop::WorkspaceMeta& ws) const;
    std::optional<SessionMeta> find_session_meta_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) const;
    std::string no_workspace_cache_root() const;
    std::vector<SessionMeta> no_workspace_disk_sessions() const;
    std::optional<SessionMeta> find_no_workspace_session_meta(const std::string& id) const;

    // -----------------------------------------------------------------
    // Session draft/title/todo/response helper  (defined in server_helpers.cpp)
    // -----------------------------------------------------------------
    crow::response set_session_archive_state(const crow::request& req,
                                              const acecode::desktop::WorkspaceMeta& ws,
                                              const std::string& id,
                                              bool archived);
    crow::response purge_session_data(const crow::request& req,
                                      const acecode::desktop::WorkspaceMeta& ws,
                                      const std::string& id,
                                      bool require_archived);
    crow::response session_input_draft_response(const crow::request& req,
                                                 const std::string& id,
                                                 const std::string& text);
    crow::response session_todos_response(const crow::request& req,
                                           const acecode::desktop::WorkspaceMeta& ws,
                                           const std::string& id,
                                           const std::vector<TodoItem>& todos);
    std::optional<crow::response> parse_session_input_draft_request(const crow::request& req,
                                                                     std::string& text);
    std::shared_ptr<SessionEntry> active_session_entry_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) const;
    void emit_session_title_update(SessionEntry& entry) const;
    std::optional<crow::response> parse_session_title_request(const crow::request& req,
                                                               std::string& title);
    crow::response set_session_title_response(const crow::request& req,
                                               const acecode::desktop::WorkspaceMeta& ws,
                                               const std::string& id);
    crow::response get_session_input_draft(const crow::request& req,
                                            const acecode::desktop::WorkspaceMeta& ws,
                                            const std::string& id);
    crow::response set_session_input_draft(const crow::request& req,
                                            const acecode::desktop::WorkspaceMeta& ws,
                                            const std::string& id);
    crow::response clear_session_todos(const crow::request& req,
                                        const acecode::desktop::WorkspaceMeta& ws,
                                        const std::string& id);
    std::filesystem::path pinned_sessions_path_for_cwd(const std::string& cwd) const;
    std::filesystem::path pinned_session_order_path() const;
    std::vector<std::string> session_ids_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws) const;
    nlohmann::json pinned_sessions_to_json(const acecode::desktop::WorkspaceMeta& ws,
                                            const std::vector<std::string>& session_ids) const;
    std::vector<PinnedSessionOrderItem> available_pinned_session_order_items() const;
    nlohmann::json pinned_session_order_to_json(
        const std::vector<PinnedSessionOrderItem>& items) const;

    // -----------------------------------------------------------------
    // Attention state helper  (defined in server_helpers.cpp)
    // -----------------------------------------------------------------
    std::string attention_store_path_for_cwd(const std::string& cwd) const;
    void load_attention_workspace_locked(const std::string& workspace_hash,
                                          const std::string& cwd) const;
    // 立即整份重写该 workspace 的 attention 文件。成功后清掉脏标记；
    // 写失败则保留脏标记，交给 flusher 下个周期重试。调用方必须持 attention_mu。
    void save_attention_workspace_locked(const std::string& workspace_hash) const;
    // 把当前所有脏 workspace 落盘。调用方必须持 attention_mu。
    void flush_dirty_attention_workspaces_locked() const;
    void start_attention_flusher();
    void stop_attention_flusher();
    SessionAttentionRecord attention_record_for_session(const std::string& workspace_hash,
                                                         const std::string& cwd,
                                                         const std::string& session_id,
                                                         bool busy) const;
    nlohmann::json attention_payload_for_record(const std::string& session_id,
                                                 const std::string& workspace_hash,
                                                 const std::string& cwd,
                                                 const SessionAttentionRecord& record) const;
    void append_attention_fields(nlohmann::json& o,
                                  const std::string& session_id,
                                  const std::string& workspace_hash,
                                  const std::string& cwd,
                                  bool busy) const;
    std::optional<acecode::desktop::WorkspaceMeta> resolve_session_workspace(
        const std::string& session_id,
        const std::string& workspace_hash_hint = {}) const;
    void broadcast_session_status(const nlohmann::json& payload);
    void note_session_event_for_attention(const std::string& session_id,
                                           const std::string& workspace_hash,
                                           const std::string& cwd,
                                           const SessionEvent& evt);
    // 见 WebServer::track_subagent。给子会话挂一个常驻(不随 WS 连接生灭)的
    // 事件监听器,把它的事件喂给 note_session_event_for_attention,从而在没有
    // 任何 WS 客户端订阅该子会话时也能广播其 session_status(打破「广播需订阅、
    // 订阅需发现、发现需广播」的死锁)。订阅 id 保存在 Impl 中并在析构时
    // 显式 unsubscribe,避免 WebServer 先于 SessionRegistry 析构时留下悬空回调。
    void track_subagent(const std::string& child_session_id);
    nlohmann::json mark_session_read_status(const std::string& session_id,
                                             const std::string& workspace_hash,
                                             const std::string& cwd,
                                             std::uint64_t cursor);
    void send_status_snapshot(crow::websocket::connection& conn,
                               const acecode::desktop::WorkspaceMeta& ws);

    // -----------------------------------------------------------------
    // Session options helper  (defined in server_helpers.cpp)
    // -----------------------------------------------------------------
    void refresh_default_session_preferences_for_new_session();
    void refresh_default_session_preferences_for_new_session_locked();
    std::optional<crow::response> parse_session_options(const crow::request& req,
                                                         const acecode::desktop::WorkspaceMeta& ws,
                                                         SessionOptions& opts);
    std::optional<SessionModelState> current_model_state_for_session(
        const std::string& session_id,
        const std::string& workspace_hash_hint = {}) const;

    // -----------------------------------------------------------------
    // 路由注册  (each defined in its own routes/routes_*.cpp)
    // -----------------------------------------------------------------
    void register_routes();

    void register_health();
    void register_usage();
    void register_workspaces();
    void register_pinned_sessions();
    void register_sessions();
    void register_models();
    void register_experts();
    void register_loops();
    void register_ui_preferences();
    void register_history();
    void register_files();
    void register_git();
    void register_lsp();
    void register_skills();
    void register_commands();
    void register_mcp();
    void register_hooks();
    void register_feedback();
    void register_pty();
    void register_websocket();
    void register_static();

    // PTY helpers (defined in routes/routes_pty.cpp)
    std::optional<crow::response> require_pty_access(const crow::request& req);
    nlohmann::json console_shells_payload();

    // WebSocket message/close handlers (defined in routes/routes_ws.cpp)
    void handle_ws_message(crow::websocket::connection& conn, const std::string& data);
    void handle_ws_close(crow::websocket::connection& conn, const std::string& reason);
};

} // namespace acecode::web
