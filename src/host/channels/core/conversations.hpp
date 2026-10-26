#pragma once

// 一个平台的会话路由(im-channels spec「IM 会话与 ACECode 会话绑定」「IM 内命令」
// 「机主与会话切换范围」,design D6):
//   - 每个 IM 会话同一时刻绑定一个 ACECode 会话;第一条普通消息新建无项目会话
//     (default 权限,不继承 daemon 的危险标志);绑定持久化,重启后续接。
//   - /new 在当前位置新建;/sessions + /resume 切换(机主可选任意会话,其他人只能选
//     自己创建过的会话);会话忙碌时拒绝切换。
//   - 一个 ACECode 会话只绑定一个 IM 会话:被别处接管时通知原 IM 会话并解除绑定。
// handle() 只在该平台的工作线程上串行调用;查询接口可从任意线程调用。

#include "agent/side_question/side_chat.hpp"
#include "channels/core/access.hpp"
#include "channels/core/commands.hpp"
#include "channels/core/paging.hpp"
#include "channels/core/projection.hpp"
#include "channels/core/store.hpp"
#include "remote_control/rc_session_navigation.hpp"
#include "session/channel_delegate.hpp"
#include "session/session_client.hpp"
#include "utils/joining_thread.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode::channels::core {

// /skills 的一行:技能或项目命令(都以 /名称 调用)。
struct SkillListing {
    std::string name;
    std::string description;
};

// 消息里 @编号 引用会话后,交给模型的文本与随消息保存的引用信息(与 Web 的 session_references 同形)。
struct SessionReferenceExpansion {
    std::string text;
    nlohmann::json meta = nlohmann::json::array();
};

// 一次会话列表(/sessions 或 @ref):编号即下标 + 1,翻页与 /resume、@编号 都用它。
struct SessionList {
    std::string heading;
    std::vector<rc::RcSessionTarget> targets;
};

struct ConversationDeps {
    SessionClient* sessions = nullptr;
    std::function<std::vector<nlohmann::json>(const std::string&)> pending_permissions;
    std::function<std::string(const std::string&)> session_cwd;
    std::function<std::vector<rc::RcSessionTarget>(const std::optional<std::string>&)> catalog;
    std::function<bool(const rc::RcSessionTarget&)> resume_target;
    std::function<std::vector<std::string>()> model_names;
    std::function<bool(const std::string&, const std::string&, std::string*)> switch_model;
    std::function<bool(const std::string& session_id, const std::string& mode)> set_permission_mode;
    std::function<std::optional<SessionModelState>(const std::string& session_id)> model_state;
    std::function<std::optional<SessionModelState>(const std::string& session_id,
                                                 const std::optional<std::string>& effort,
                                                 std::string* error)> set_reasoning_effort;
    // 普通文本提交前做技能命令展开(与 Web 输入一致);返回展开后的文本。
    std::function<std::string(const std::string& session_id, const std::string& text)> expand_skill;
    // /btw:基于会话上下文旁问(只读工具,不改会话),耗时较长,在后台线程上调用;
    // 断开连接时经 cancellation 取消。
    std::function<SideQuestionResult(const std::string& session_id, const std::string& question,
                                     SideChatCancellation& cancellation)>
        side_question;
    // /skills:该会话当前可用的技能与项目命令。
    std::function<std::vector<SkillListing>(const std::string& session_id)> list_skills;
    // @编号:把被引用会话的近期对话拼进本次输入(有大小上限);失败时返回空并写 error。
    std::function<std::optional<SessionReferenceExpansion>(const std::string& session_id,
                                                           const std::vector<rc::RcSessionTarget>& targets,
                                                           const std::string& text, std::string* error)>
        reference_sessions;
    // 某会话即将被 taker 接管:host 让所有平台释放它(跨平台转移绑定)。
    std::function<void(const std::string& session_id, const std::string& taker_key,
                       const std::string& taker_label)> release_session;
    std::function<void()> on_bindings_changed;
    std::filesystem::path media_dir;  // 入站附件的临时下载目录
    // 群聊代答(D5):写入 / 清除会话的 channel_delegate meta(不活动的值 = 清除)。
    std::function<void(const std::string& session_id, const ChannelDelegate& delegate)> set_delegate;
    // 测试注入:轮询用的单调时钟与本地时区偏移(分钟);为空时取系统值。
    std::function<std::chrono::steady_clock::time_point()> clock;
    std::function<int()> utc_offset_minutes;
};

struct HandleOutcome {
    std::optional<PendingRequest> pending_created;
    bool owner_claimed = false;
};

struct GroupSendResult {
    bool ok = false;
    std::string error;
};

class Conversations {
public:
    Conversations(std::string platform, ChannelStore& store, AccessControl& access,
                  std::shared_ptr<TransportSlot> transport, ConversationDeps deps);
    ~Conversations();

    HandleOutcome handle(const im::Inbound& inbound);
    // 连接后为已有绑定恢复出站投影,使 Desktop 里的输入也能把回复发到 IM;
    // 会话已被删除的绑定保留,等对方下一条消息时提示。与 handle() 在同一线程串行调用。
    void restore();
    // 若本平台有 IM 会话(除 except_key 外)绑定着该会话:通知它、解除绑定。
    bool release_session(const std::string& session_id, const std::string& except_key,
                         const std::string& taker_label);
    void shutdown();
    // 撤销授权:中止该身份所有绑定会话正在进行的回合。
    void abort_principal(const std::string& principal);
    nlohmann::json bindings_json() const;
    std::vector<std::string> bound_session_ids() const;
    static std::string label_for(const im::Address& address);

    // 群聊代答(D5)。poll_groups 与 handle() 在同一线程串行调用:到点且有他人新消息、
    // 会话空闲时把缓冲汇总成一条用户消息提交。
    void poll_groups();
    // 设置变化后:移除不再关注的群的绑定,刷新其余代答会话的 meta。
    void apply_group_settings(const GroupWatchSettings& previous, const GroupWatchSettings& next);
    // 发群工具的入口;session_id 不是本平台的代答会话时返回空。
    std::optional<GroupSendResult> send_to_group(const std::string& session_id, const std::string& text);
    static constexpr std::size_t kMaxGroupSendChars = 2000;
    static constexpr int kMaxGroupSendsPerDigest = 5;

    static constexpr std::size_t kMaxSessionReferences = 5;
    static constexpr std::size_t kMaxQueuedSideQuestions = 4;

private:
    void dispatch(const im::Inbound& inbound, bool owner);
    void reply(const im::Inbound& inbound, const std::string& text);
    void submit(const im::Inbound& inbound);
    void handle_bound_command(const im::Inbound& inbound, const Command& command, bool owner);
    void list_sessions(const im::Inbound& inbound, bool owner, const std::string& argument);
    void resume(const im::Inbound& inbound, bool owner, const std::string& argument);

    // ---- conversation_commands.cpp ----
    // 记下这次分页输出(/more 翻页用)并回复第 page 页。
    void show_paged(const im::Inbound& inbound, PagedOutput output, std::size_t page = 1);
    void more(const im::Inbound& inbound, const std::string& argument);
    // /sessions 与 @ref 共用:按参数(空 / 关键词 / 页码)建立或翻看列表。
    void show_session_list(const im::Inbound& inbound, bool owner, const std::string& argument, bool refs);
    SessionList build_session_list(const std::string& key, bool owner, const std::optional<std::string>& query,
                                   bool refs) const;
    PagedOutput session_pages(const SessionList& list, const std::string& current_session) const;
    void list_skills(const im::Inbound& inbound, const std::string& session_id, const std::string& argument);
    void run_builtin(const im::Inbound& inbound, const std::string& session_id,
                     const std::shared_ptr<Projection>& projection, const Command& command);
    void steer(const im::Inbound& inbound, const std::string& session_id, const std::string& text);
    void ask_side(const im::Inbound& inbound, const std::string& session_id, const std::string& question);
    // 把正文里的 @编号 换成「@会话标题」并取出被引用的会话;返回 false 表示已回复错误、不提交。
    bool resolve_session_mentions(const im::Inbound& inbound, const std::string& session_id, std::string& text,
                                  std::vector<rc::RcSessionTarget>& referenced);
    // 把被引用会话的上下文拼到 body 前面;失败时已回复错误并返回空。
    std::optional<SessionReferenceExpansion> with_references(const im::Inbound& inbound,
                                                             const std::string& session_id,
                                                             const std::vector<rc::RcSessionTarget>& referenced,
                                                             const std::string& body);
    // 后台任务(/btw):串行执行,不阻塞平台工作线程上的入站处理。
    bool post_task(std::function<void(Conversations&)> task);
    void task_loop();
    void stop_tasks();
    void new_session(const im::Inbound& inbound);
    BindingRecord create_binding(const im::Address& address, const std::optional<BindingRecord>& near);
    std::shared_ptr<Projection> ensure_projection(const BindingRecord& record);
    std::shared_ptr<Projection> projection_for(const std::string& key) const;
    void switch_binding(const BindingRecord& record);
    std::optional<SessionInfo> session_info(const std::string& session_id) const;
    std::vector<rc::RcSessionTarget> visible_sessions(const std::string& key, bool owner,
                                                      const std::optional<std::string>& query) const;
    void ingest_group(const im::Inbound& inbound);
    std::chrono::steady_clock::time_point now() const;
    ChannelDelegate delegate_for(const BindingRecord& record, const GroupWatchSettings& settings) const;
    std::optional<BindingRecord> delegate_binding_for_session(const std::string& session_id) const;
    // 找到或新建该群的代答会话;会话已被删除时换一个新会话。
    BindingRecord ensure_delegate_binding(const std::string& account, const std::string& chat,
                                          const std::string& chat_name);
    void drop_delegate_binding(const BindingRecord& record);

    std::string platform_;
    ChannelStore& store_;
    AccessControl& access_;
    std::shared_ptr<TransportSlot> transport_;
    ConversationDeps deps_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Projection>> projections_;
    std::map<std::string, SessionList> session_lists_;  // IM 会话 → 最近一次 /sessions
    std::map<std::string, SessionList> ref_lists_;      // IM 会话 → 最近一次 @ref
    struct Pager {
        PagedOutput output;
        std::size_t page = 1;
    };
    std::map<std::string, Pager> pagers_;  // IM 会话 → 最近一次分页输出

    mutable std::mutex groups_mu_;
    std::map<std::string, GroupBuffer> groups_;           // 群 id → 自上次汇总以来的消息
    std::map<std::string, int> group_sends_;              // 代答会话 → 本次汇总后已发条数
    std::chrono::steady_clock::time_point next_poll_{};  // 两次检查之间至少隔几秒

    // 后台任务队列;工作线程最后声明,shutdown() 在其它成员析构前先停掉并等它退出。
    std::mutex tasks_mu_;
    std::condition_variable tasks_cv_;
    std::deque<std::function<void(Conversations&)>> tasks_;
    bool tasks_stopping_ = false;
    std::shared_ptr<SideChatCancellation> running_side_;  // 正在回答的旁问
    acecode::JoiningThread tasks_worker_;
};

} // namespace acecode::channels::core
