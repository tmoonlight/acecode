// server_impl.hpp — Internal header exposing WebServer::Impl for route TUs.
// NOT installed; never included outside src/web/ and src/web/routes/.
#pragma once

#include "server.hpp"

#include "auth.hpp"
#include "origin.hpp"
#include "remote_web.hpp"
#include "remote_web_proxy.hpp"
#include "static_assets.hpp"
#include "config/config.hpp"
#include "config/saved_models_editor.hpp"
#include "saved_model_reasoning_sync.hpp"
#include "config/request_headers.hpp"
#include "workspace/workspace_registry.hpp"
#include "hooks/hook_manager.hpp"
#include "environment/data_dir_migration.hpp"
#include "loop/loop_store.hpp"
#include "provider/auth/github_auth.hpp"
#include "provider/auth/xai_auth.hpp"
#include "llm/llm_provider.hpp"
#include "provider/model_pool_status.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/attachment_store.hpp"
#include "session/global_session_search.hpp"
#include "session_host/local_session_client.hpp"
#include "session/opencode_import.hpp"
#include "session/session_attention.hpp"
#include "session/session_client.hpp"
#include "session_host/session_registry.hpp"
#include "session_host/task_suggestion_service.hpp"
#include "session/session_rewind.hpp"
#include "session/session_serializer.hpp"
#include "session/session_markdown_export.hpp"
#include "session/session_storage.hpp"
#include "session/todo_state.hpp"
#include "session/session_usage_ledger.hpp"
#include "session/session_writer_lease.hpp"
#include "platform/process/os_process.hpp"
#include "skills/skill_registry.hpp"
#include "experts/expert_registry.hpp"
#include "skills/skill_metadata.hpp"
#include "tool/tool_executor.hpp"
#include "upgrade/apply.hpp"
#include "upgrade/check.hpp"
#include "themes/theme_store.hpp"
#include "utils/logger.hpp"
#include "utils/lifetime_token.hpp"
#include "utils/base64.hpp"
#include "utils/cwd_hash.hpp"
#include "workspace/files_handler.hpp"
#include "web/handlers/fork_handler.hpp"
#include "web/handlers/history_handler.hpp"
#include "web/handlers/grok_auth_handler.hpp"
#include "web/handlers/models_handler.hpp"
#include "web/handlers/permission_mode_handler.hpp"
#include "web/handlers/pinned_sessions_handler.hpp"
#include "web/handlers/builtin_command_handler.hpp"
#include "web/handlers/commands_handler.hpp"
#include "web/handlers/opencode_command_expander.hpp"
#include "skills/skill_command_expander.hpp"
#include "web/handlers/session_list_handler.hpp"
#include "web/handlers/side_chat_handler.hpp"
#include "web/handlers/skills_handler.hpp"
#include "skills/skill_init.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "pty/pty_session_registry.hpp"
#include "version.hpp"

// Crow 头一定在 ASIO_STANDALONE PUBLIC 定义之后才 include。CMakeLists.txt 已
// 给 acecode_testable 加 PUBLIC 的 ASIO_STANDALONE,所以这里直接 include 即可。
#include <cstddef>
#include <crow.h>

#include "utils/utf8_path.hpp"

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
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "network/proxy_resolver.hpp"

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
struct SideChatWorker {
    std::shared_ptr<SideChatRequestState> request;
    std::atomic<bool> finished{false};
    std::thread thread;
};

struct WsConnState {
    // Nullable borrowed connection; access only under Impl::ws_mu while registered.
    crow::websocket::connection* connection = nullptr;
    std::string session_id;
    std::unordered_map<std::string, SessionClient::SubscriptionId> subscriptions;
    std::unordered_set<std::string> status_workspaces;
    std::unordered_set<std::string> status_sessions;
    SideChatConnectionState side_chat;
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
    std::string worktree_path;
    std::string worktree_name;
    std::string worktree_branch;
    std::string expected_turn_id;
    // 提问插话路由专用:body.request_id,对应 question_request.request_id。
    std::string question_request_id;
    // body.swarm_mode:"star" | "mesh" | "off" | false,旧客户端的 true 等价 star。
    // 缺省 = 沿用会话当前模式。
    std::optional<SwarmMode> swarm_mode;
};

// Where a home (workspace) draft lives. draft_dir holds input_draft.json;
// attachment_project_dir is the project dir whose attachments/.workspace-draft/
// holds the draft's pasted-text attachments. For a real workspace both are
// projects_dir()/<hash>. For "__no_workspace__" the draft stays in the
// no-workspace cache root, but the attachments go to the project dir of that
// root: every subdirectory of the cache root is treated as a no-workspace
// session cwd (list_no_workspace_session_cwds), so an attachments/ folder there
// would show up as a phantom session.
struct WorkspaceDraftLocation {
    std::filesystem::path draft_dir;
    std::filesystem::path attachment_project_dir;
    // Empty for "__no_workspace__" (the draft route reports it that way).
    std::string workspace_hash;
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
void add_loopback_cors_headers(const crow::request& req, crow::response& resp);
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
    std::string log_path;
    std::string log_error;
    bool restart_required = false;
    bool cancel_requested = false;
};

struct UpdateJobRuntime {
    std::mutex mu;
    std::optional<UpdateJobStatus> current;
    std::shared_ptr<acecode::upgrade::DiagnosticLog> diagnostics;
};

// 在 Crow 完成响应时补齐 CORS,覆盖绕过路由返回助手的全局异常路径。
struct ResponseCorsMiddleware {
    struct context {};

    void before_handle(crow::request&, crow::response&, context&) {}
    void after_handle(crow::request& req, crow::response& resp, context&) {
        add_loopback_cors_headers(req, resp);
    }
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
    crow::App<ResponseCorsMiddleware> app;

    // 静态资源 source(EmbeddedAssetSource / FileSystemAssetSource),按
    // web.static_dir 路径在 register_routes 前实例化。
    std::unique_ptr<AssetSource> assets;

    // ws 注册表: 把 listener / state 与 connection 绑定,断开时清理。
    std::mutex                                                      ws_mu;
    std::unordered_map<crow::websocket::connection*, std::shared_ptr<WsConnState>> ws_connections;
    // Protected by ws_mu. Finished workers are joined/reaped on the next
    // request; shutdown cancels and joins the remaining workers.
    std::vector<std::shared_ptr<SideChatWorker>> side_chat_workers;

    // Crow runs HTTP handlers on multiple worker threads. deps.app_config is a
    // shared mutable object, so every web-side read/write must go through this.
    //
    // 读写纪律(fix session-switch lock convoy):只读/快照路径使用
    // std::shared_lock<std::shared_mutex>,写路径使用
    // std::lock_guard<std::shared_mutex> 独占。两条 resume 路由会全量解析
    // jsonl + 扫 skill 目录 + 建 provider,实测单次 574~824ms;它们持共享锁
    // 时必须允许模型列表、健康状态等 config 只读请求并发通过。settings
    // 变更、refresh_default_session_preferences、saved_models 落盘等写方
    // 必须保持独占。
    mutable std::shared_mutex owned_app_config_mu;
    std::shared_mutex& app_config_mu;

    std::atomic<bool> shutdown_requested{false};
    mutable std::mutex listener_state_mu;
    std::string effective_bind;
    int effective_port = 0;
    std::mutex listener_stop_mu;
    // Serializes start/persist and persist/stop transactions from concurrent
    // settings requests without holding the broader app-config lock while a
    // child process reaches readiness.
    std::mutex remote_web_proxy_mu;

    // 从磁盘重读 saved_models 合并进内存 —— 连接器钩子(外部登录器)会直接
    // 改写 config.json;不重读的话,下一次任何 save_config 都会把新写入的
    // api_key 抹掉。  (defined in server_helpers.cpp)
    void refresh_saved_models_from_disk();
    std::unique_ptr<SavedModelReasoningSync> model_reasoning_sync;
    void initialize_model_reasoning_sync();
    void request_model_reasoning_sync(const std::string& name = {});
    void refresh_image_generation_tool_locked();
    void refresh_computer_use_tool_locked();
    std::mutex image_generation_test_mu;
    // 串行化 tool-rewrites.json 的读改写(它不在 config.json 里,不受 app_config_mu 管)。
    std::mutex tool_rewrites_mu;
    // 串行化 <data_dir>/rules/*.rules 托管文件的读改写(安全中心 > 命令安全)。
    std::mutex exec_rules_mu;

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

    struct OpencodeImportRuntime {
        std::mutex mu;
        std::unordered_map<std::string, OpencodeImportJobStatus> jobs;
    };
    // Shared with import jobs that can finish after this server is destroyed.
    std::shared_ptr<OpencodeImportRuntime> opencode_import_runtime =
        std::make_shared<OpencodeImportRuntime>();

    std::shared_ptr<UpdateJobRuntime> update_job_runtime =
        std::make_shared<UpdateJobRuntime>();
    std::unique_ptr<acecode::themes::ThemeStore> theme_store;

    // 数据目录迁移任务(openspec: data-directory-relocation):同一 daemon 内只允许
    // 一个;跑的期间消息发送路由返回 409(reject_if_migrating)。
    std::shared_ptr<acecode::environment::DataDirMigrationJob> data_dir_migration =
        std::make_shared<acecode::environment::DataDirMigrationJob>();

    // Daemon-lifetime global search state. The catalog prewarms independently
    // of HTTP requests; content jobs are short, request-scoped batches.
    // Import completion takes only a temporary lease from a weak reference.
    std::shared_ptr<GlobalSessionSearchService> global_session_search;

    explicit Impl(WebServerDeps d)
        : deps(std::move(d)),
          runtime_port(deps.web_cfg ? deps.web_cfg->port : 0),
          app_config_mu(deps.app_config_mutex
              ? *deps.app_config_mutex
              : owned_app_config_mu) {
        subagent_tracker_state->impl = this;
        start_attention_flusher();
        global_session_search = std::make_shared<GlobalSessionSearchService>(
            projects_dir(), [this] {
                return deps.session_client
                    ? deps.session_client->list_sessions()
                    : std::vector<SessionInfo>{};
            });
        global_session_search->start();
    }
    ~Impl();

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
    std::optional<WorkspaceDraftLocation> workspace_draft_location(const std::string& hash) const;
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
    crow::response handle_turn_input_request(const crow::request& req,
                                             const std::string& session_id,
                                             bool interrupting);
    // POST /api/sessions/:id/questions/interject:提问挂起时的用户插话。
    crow::response handle_question_interject_request(const crow::request& req,
                                                     const std::string& session_id);
    // parent_filter 语义:空 = 常规列表,排除所有 spawn_subagent 子会话;
    // 非空 = 后台任务查询,只返回 parent_session_id == parent_filter 的子会话
    // (active 部分不做 workspace 过滤,子会话跟随父会话归属)。
    // sessions_for_workspace 的分页回执。total 在 total_exact 为 false 时
    // 是个上界:磁盘侧提前停在了 limit 上,只能按目录里的候选文件数报量级。
    struct SessionListPage {
        std::size_t total = 0;
        bool total_exact = true;
        bool has_more = false;
    };

    nlohmann::json sessions_for_workspace(const acecode::desktop::WorkspaceMeta& ws,
                                          bool archived_only = false,
                                          bool include_no_workspace = false,
                                          const std::string& parent_filter = {},
                                          int limit = 0,
                                          SessionListPage* page_out = nullptr,
                                          bool no_workspace_only = false) const;
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
                                                 const std::string& text,
                                                 const nlohmann::json& composer_content = nullptr);
    crow::response session_todos_response(const crow::request& req,
                                           const acecode::desktop::WorkspaceMeta& ws,
                                           const std::string& id,
                                           const std::vector<TodoItem>& todos);
    std::optional<crow::response> parse_session_input_draft_request(const crow::request& req,
                                                                     std::string& text,
                                                                     nlohmann::json& composer_content);
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
    std::filesystem::path no_workspace_pinned_sessions_path() const;
    std::filesystem::path pinned_session_order_path() const;
    std::vector<std::string> session_ids_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws,
        const std::vector<std::string>& candidates) const;
    std::vector<std::string> session_ids_for_no_workspace(
        const std::vector<std::string>& candidates) const;
    nlohmann::json pinned_sessions_to_json(const acecode::desktop::WorkspaceMeta& ws,
                                            const std::vector<std::string>& session_ids) const;
    nlohmann::json no_workspace_pinned_sessions_to_json(
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
    void broadcast_remote_control_session_selected(const std::string& session_id,
                                                   const std::string& workspace_hash,
                                                   const std::string& cwd,
                                                   bool no_workspace,
                                                   const std::string& title,
                                                   const std::string& updated_at);
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
    // 会话右键「标记为未读」。与 mark_session_read_status 同一把锁、同样立即落盘并
    // 在状态变化时广播 session_status。
    nlohmann::json mark_session_unread_status(const std::string& session_id,
                                               const std::string& workspace_hash,
                                               const std::string& cwd);
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
    // 会话 create/resume 路由里逃逸的 std::exception 统一收口:记 ERROR 日志
    // (带 cwd 上下文)并返回 JSON 500 `{error, message, cwd}`。没有这层时异常
    // 交给 Crow 变成裸 "500 Internal Server Error",原因只会写到 stderr ——
    // Desktop 托管的 daemon stderr 指向 NUL,用户与日志两边都看不到任何线索。
    crow::response session_route_failure(const crow::request& req,
                                         const char* error_code,
                                         const std::string& cwd,
                                         const std::exception& e);

    // -----------------------------------------------------------------
    // 路由注册  (each defined in its own routes/routes_*.cpp)
    // -----------------------------------------------------------------
    void register_routes();
    void register_image_generation();
    void register_computer_use();
    void register_summary_generation();
    void register_tool_rewrites();
    void register_memory();
    void register_tool_preamble();
    void register_security();

    void register_health();
    void register_session_diagnostics();
    void register_usage();
    void register_workspaces();
    void register_pinned_sessions();
    void register_sessions();
    void register_task_suggestions();
    void register_models();
    void register_experts();
    void register_loops();
    void register_ui_preferences();
    void register_themes();
    void register_history();
    void register_files();
    void register_fs();
    void register_git();
    void register_lsp();
    void register_skills();
    void register_commands();
    std::mutex mcp_config_mu;
    void register_mcp();
    void register_hooks();
    void register_feedback();
    void register_pty();
    void register_environment();
    void register_websocket();
    void register_static();

    // Settings → 配置 环境端点的辅助(定义在 routes/routes_environment.cpp)。
    // *_locked 版本要求调用方已持有 app_config_mu(共享或独占)。
    nlohmann::json toolchains_payload_locked();
    nlohmann::json console_config_payload_locked();
    // 数据目录迁移进行中 → 409,消息发送路由在入队前调用。
    std::optional<crow::response> reject_if_migrating(const crow::request& req);

    // PTY helpers (defined in routes/routes_pty.cpp)
    std::optional<crow::response> require_pty_access(const crow::request& req);
    nlohmann::json console_shells_payload();

    // WebSocket message/close handlers (defined in routes/routes_ws.cpp)
    void handle_ws_message(crow::websocket::connection& conn, const std::string& data);
    void handle_ws_close(crow::websocket::connection& conn, const std::string& reason);
    void handle_side_chat_message(crow::websocket::connection& conn,
                                  const std::shared_ptr<WsConnState>& state,
                                  const std::string& type,
                                  const nlohmann::json& payload);
    void stop_side_chat_workers();
    LifetimeToken ws_listener_lifetime;
};

} // namespace acecode::web
