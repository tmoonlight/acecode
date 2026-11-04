// Conversations 的后台会话:IM 会话换到别的 ACECode 会话时,原会话若仍在执行就转到后台,
// 结束或停下等确认时在原 IM 会话里提示一次。会话路由与绑定本身在 conversations.cpp。

#include "conversations.hpp"

#include <algorithm>

namespace acecode::channels::core {
namespace {

constexpr std::size_t kBackgroundTitleChars = 40;
// 结束提示经发送队列异步发出;发出后再留一会儿才停掉投影,免得停掉时把它从队列里丢掉。
constexpr auto kBackgroundLinger = std::chrono::minutes(2);

} // namespace

std::string Conversations::retire_projection(const std::shared_ptr<Projection>& projection) {
    const auto session_id = projection->record().session_id;
    const auto info = session_info(session_id);
    if (!info || !info->busy) {
        projection->stop();
        return {};
    }
    std::string title = info->title.empty() ? info->summary : info->title;
    if (title.empty()) title = session_id;
    title = one_line(title, kBackgroundTitleChars);
    std::size_t waiting = projection->pending_permission_ids().size();
    if (const auto questions = deps_.sessions->snapshot_pending_questions(session_id)) waiting += questions->size();
    projection->move_to_background(title);
    // 回合可能恰好在转入后台之前结束(结束事件已按前台处理):再查一次,没等到结束提示就直接停掉。
    const auto after = session_info(session_id);
    const bool finished = projection->background_finished_at().has_value();
    if ((!after || !after->busy) && !finished) {
        projection->stop();
        return {};
    }
    std::vector<std::shared_ptr<Projection>> evicted;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& slot = background_[session_id];
        if (slot.projection) evicted.push_back(slot.projection);
        slot = {projection, std::chrono::steady_clock::now()};
        while (background_.size() > kMaxBackgroundSessions) {
            const auto oldest = std::min_element(background_.begin(), background_.end(),
                                                 [](const auto& a, const auto& b) { return a.second.since < b.second.since; });
            evicted.push_back(oldest->second.projection);
            background_.erase(oldest);
        }
    }
    for (const auto& item : evicted) item->stop();
    return finished ? std::string{} : texts::left_running(title, session_id, waiting);
}

void Conversations::stop_background(const std::string& session_id) {
    std::shared_ptr<Projection> projection;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = background_.find(session_id);
        if (it == background_.end()) return;
        projection = std::move(it->second.projection);
        background_.erase(it);
    }
    projection->stop();
}

void Conversations::sweep_background() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Projection>> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = background_.begin(); it != background_.end();) {
            const auto at = it->second.projection->background_finished_at();
            if (at && now - *at >= kBackgroundLinger) {
                done.push_back(std::move(it->second.projection));
                it = background_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& projection : done) projection->stop();
}

} // namespace acecode::channels::core
