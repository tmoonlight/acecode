#pragma once

// 一个平台的会话路由(im-channels spec「IM 会话与 ACECode 会话绑定」「IM 内命令」
// 「机主与会话切换范围」,design D6):
//   - 每个 IM 会话同一时刻绑定一个 ACECode 会话;第一条普通消息新建无项目会话
//     (default 权限,不继承 daemon 的危险标志);绑定持久化,重启后续接。
//   - /new 在当前位置新建;/sessions + /resume 切换(机主可选任意会话,其他人只能选
//     自己创建过的会话);会话忙碌时拒绝切换。
//   - 一个 ACECode 会话只绑定一个 IM 会话:被别处接管时通知原 IM 会话并解除绑定。
// handle() 只在该平台的工作线程上串行调用;查询接口可从任意线程调用。

#include "channels/core/access.hpp"
#include "channels/core/commands.hpp"
#include "channels/core/projection.hpp"
#include "channels/core/store.hpp"
#include "remote_control/rc_session_navigation.hpp"
#include "session/session_client.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode::channels::core {

struct ConversationDeps {
    SessionClient* sessions = nullptr;
    std::function<std::vector<nlohmann::json>(const std::string&)> pending_permissions;
    std::function<std::string(const std::string&)> session_cwd;
    std::function<std::vector<rc::RcSessionTarget>(const std::optional<std::string>&)> catalog;
    std::function<bool(const rc::RcSessionTarget&)> resume_target;
    std::function<std::vector<std::string>()> model_names;
    std::function<bool(const std::string&, const std::string&, std::string*)> switch_model;
    // 普通文本提交前做技能命令展开(与 Web 输入一致);返回展开后的文本。
    std::function<std::string(const std::string& session_id, const std::string& text)> expand_skill;
    // 某会话即将被 taker 接管:host 让所有平台释放它(跨平台转移绑定)。
    std::function<void(const std::string& session_id, const std::string& taker_key,
                       const std::string& taker_label)> release_session;
    std::function<void()> on_bindings_changed;
    std::filesystem::path media_dir;  // 入站附件的临时下载目录
};

struct HandleOutcome {
    std::optional<PendingRequest> pending_created;
    bool owner_claimed = false;
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

private:
    void dispatch(const im::Inbound& inbound, bool owner);
    void reply(const im::Inbound& inbound, const std::string& text);
    void submit(const im::Inbound& inbound);
    void handle_bound_command(const im::Inbound& inbound, const Command& command);
    void list_sessions(const im::Inbound& inbound, bool owner, const std::string& argument);
    void resume(const im::Inbound& inbound, bool owner, const std::string& argument);
    void new_session(const im::Inbound& inbound);
    BindingRecord create_binding(const im::Address& address, const std::optional<BindingRecord>& near);
    std::shared_ptr<Projection> ensure_projection(const BindingRecord& record);
    std::shared_ptr<Projection> projection_for(const std::string& key) const;
    void switch_binding(const BindingRecord& record);
    std::optional<SessionInfo> session_info(const std::string& session_id) const;
    std::vector<rc::RcSessionTarget> visible_sessions(const std::string& key, bool owner,
                                                      const std::optional<std::string>& query) const;

    std::string platform_;
    ChannelStore& store_;
    AccessControl& access_;
    std::shared_ptr<TransportSlot> transport_;
    ConversationDeps deps_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<Projection>> projections_;
    std::map<std::string, std::vector<rc::RcSessionTarget>> snapshots_;
};

} // namespace acecode::channels::core
