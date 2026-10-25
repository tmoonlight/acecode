// SessionManager 的蜂群模式与网状 agent 路径。只改字段并在会话已落盘时写
// meta;单独成文件让 session_manager.cpp 保持在行数基线内。
#include "session_manager.hpp"

namespace acecode {

void SessionManager::set_swarm_mode(std::string swarm_mode) {
    std::lock_guard<std::mutex> lk(mu_);
    if (swarm_mode == "off") swarm_mode.clear();
    if (swarm_mode_ == swarm_mode) return;
    swarm_mode_ = std::move(swarm_mode);
    publish_display_snapshot_locked();
    if (created_) update_meta();
}

void SessionManager::set_mesh_agent_path(std::string agent_path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (agent_path_ == agent_path) return;
    agent_path_ = std::move(agent_path);
    publish_display_snapshot_locked();
    if (created_) update_meta();
}

std::string SessionManager::current_swarm_mode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return swarm_mode_.empty() ? std::string("off") : swarm_mode_;
}

std::string SessionManager::current_agent_path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return agent_path_;
}

std::string SessionManager::current_cwd() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cwd_;
}

std::optional<std::string> SessionManager::current_reasoning_effort() const {
    std::lock_guard<std::mutex> lk(mu_);
    return reasoning_effort_;
}

void SessionManager::set_channel_delegate(ChannelDelegate delegate) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!delegate.active()) delegate = ChannelDelegate{};
    if (channel_delegate_ == delegate) return;
    channel_delegate_ = std::move(delegate);
    if (created_) update_meta();
}

ChannelDelegate SessionManager::current_channel_delegate() const {
    std::lock_guard<std::mutex> lk(mu_);
    return channel_delegate_;
}

// 群聊代答身份与蜂群身份一样随 meta 写出、随 resume 读回,共用这两个写读点。
void SessionManager::write_swarm_identity_locked(SessionMeta& meta) const {
    meta.swarm_mode = swarm_mode_;
    meta.agent_path = agent_path_;
    meta.channel_delegate = channel_delegate_;
}

void SessionManager::adopt_swarm_identity_locked(const SessionMeta& meta) {
    swarm_mode_ = meta.swarm_mode == "off" ? std::string{} : meta.swarm_mode;
    agent_path_ = meta.agent_path;
    channel_delegate_ = meta.channel_delegate;
}

} // namespace acecode
