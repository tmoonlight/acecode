#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

// 会话级批注记录容器。
//
// 记录归属会话，不归属页签：页签只是记录的渲染/编辑端点。关掉页签不能把用户
// 写到一半的描述一起析构（Q33）。因此真正持有记录的应该是会话级的这个 store，
// 而 `PageAnnotationSession` 只做转发——它是「这个页签当前能对哪些记录下命令」
// 的视图，不是权威。
//
// 记录标识与文档无关（D9）：`<page_id>:annotation:<n>`。旧标识把 document_id
// 拼进主键，而 document_id 内含自增 epoch，页面一刷新记录就指向不存在的文档代
// （见 docs/specs/2026-11-22-html-page-annotations-t0-findings.md）。文档代降为
// 记录的一个属性，只用来判 live / replayable。
//
// 编号按 page_id 分组连续：页签 A 的 1、2 与页签 B 的 1、2 并存，与旧行为一致
// （旧实现每个 PageAnnotationSession 各有一个计数器）。
class SessionAnnotationStore {
public:
    using Json = nlohmann::json;

    std::uint64_t revision() const { return revision_; }

    // 记录 id 与文档无关。`number` 在同一 page_id 内唯一且连续。
    static std::string record_id(const std::string& page_id, std::uint64_t number) {
        return page_id + ":annotation:" + std::to_string(number);
    }

    // 只读视图：未知页签返回空数组，不会在 store 里凭空建桶。
    const Json& records(const std::string& page_id) const {
        static const Json empty = Json::array();
        const auto found = pages_.find(page_id);
        return found == pages_.end() ? empty : found->second.records;
    }

    // 可写视图。注意它会为该页签建桶。
    Json& mutable_records(const std::string& page_id) { return pages_[page_id].records; }

    std::uint64_t next_number(const std::string& page_id) const {
        const auto found = pages_.find(page_id);
        return found == pages_.end() ? 1 : found->second.next_number;
    }

    // 追加一条记录并补齐 id / number / page_id。返回指向新记录的引用，
    // 调用方可以继续往上写（例如异步补图回填 thumbnail）。
    Json& append(const std::string& page_id, Json record) {
        auto& bucket = pages_[page_id];
        const auto number = bucket.next_number++;
        record["id"] = record_id(page_id, number);
        record["number"] = number;
        record["page_id"] = page_id;
        bucket.records.push_back(std::move(record));
        ++revision_;
        return bucket.records.back();
    }

    // 丢弃该页签的全部记录并把编号归 1。别页签的记录不受影响。
    void discard(const std::string& page_id) {
        const auto found = pages_.find(page_id);
        if (found == pages_.end()) return;
        found->second.records = Json::array();
        found->second.next_number = 1;
        ++revision_;
    }

    bool erase(const std::string& page_id, const std::string& id) {
        auto found = pages_.find(page_id);
        if (found == pages_.end()) return false;
        auto& records = found->second.records;
        for (auto it = records.begin(); it != records.end(); ++it) {
            if ((*it)["id"] != id) continue;
            records.erase(it);
            ++revision_;
            return true;
        }
        return false;
    }

    // 页签关闭：记录不销毁，标记来源页签已关闭。状态位本身由上层维护，
    // 这里只保证记录还活着（P2 的 orphan 语义靠这个前提）。
    bool has_page(const std::string& page_id) const { return pages_.find(page_id) != pages_.end(); }

    // 该页最近一次的整屏图（已发消息卡片配图）。每页只留最新一张：不随记录
    // 增减而变化，所以删掉最后几条批注后配图仍是删除前的画面（PRD §11）。
    const std::string& page_shot(const std::string& page_id) const {
        static const std::string empty;
        const auto found = pages_.find(page_id);
        return found == pages_.end() ? empty : found->second.page_shot;
    }

    void set_page_shot(const std::string& page_id, std::string shot) {
        auto& bucket = pages_[page_id];
        if (bucket.page_shot == shot) return;
        bucket.page_shot = std::move(shot);
        ++revision_;
    }

    struct Page {
        Json records = Json::array();
        std::uint64_t next_number = 1;
        std::string page_shot;
    };

    // 按页签遍历（用于跨页签广播与发送时聚合）。
    const std::map<std::string, Page>& pages() const { return pages_; }

private:
    std::map<std::string, Page> pages_;
    std::uint64_t revision_ = 0;
};

// 宿主侧的会话级 store 索引：同一会话的所有页签共享一个容器，页签关掉后记录
// 仍留在这里（Q33）。只在 UI 线程上访问，所以不加锁。
class SessionAnnotationStoreRegistry {
public:
    std::shared_ptr<SessionAnnotationStore> for_session(
        const std::string& session_id) {
        if (session_id.empty()) return std::make_shared<SessionAnnotationStore>();
        auto& slot = stores_[session_id];
        if (!slot) slot = std::make_shared<SessionAnnotationStore>();
        return slot;
    }

private:
    std::map<std::string, std::shared_ptr<SessionAnnotationStore>> stores_;
};

} // namespace acecode::desktop
