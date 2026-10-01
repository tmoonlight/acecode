#pragma once

#include "platform/unique_sqlite.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// 后台作业的两类租约:按会话提炼、按作用域整合。
enum class MemoryLeaseKind {
    Extraction,
    Consolidation,
};

struct MemoryExtractionProgress {
    std::string session_id;
    std::string project_dir;
    std::int64_t next_message_index = 0;  // 已提炼到的位置(下次从这条消息开始)
    int attempts = 0;                     // 当前范围连续失败次数
    std::string last_error;
    std::string failed_marker;            // 失败时的会话活动标记;活动变化后重新计数
    std::string seen_marker;              // 上次成功提炼时的会话活动标记
    std::string lease_owner;
    std::int64_t lease_expires_ms = 0;
    std::int64_t updated_ms = 0;
};

struct MemoryConsolidationProgress {
    std::string scope_key;
    int attempts = 0;
    std::string last_error;
    std::string failed_batch;             // 连续失败的批次签名;出现新观察后重新计数
    std::string pending_plan;             // 已校验、尚未完整应用的计划(崩溃后原样重放)
    std::string pending_plan_hash;
    std::string last_plan_hash;
    std::string lease_owner;
    std::int64_t lease_expires_ms = 0;
    std::int64_t last_success_ms = 0;
    std::int64_t updated_ms = 0;
};

struct MemoryTombstone {
    std::string scope_key;
    std::string name;
    std::string title_norm;
    std::int64_t deleted_ms = 0;
};

// 所有进程共用的记忆状态库 <data_dir>/memory/state.sqlite3(D5):提炼进度、
// 整合进度、墓碑、状态摘要四张表。连接开 WAL 与 busy_timeout;每个进程(测试里
// 每个实例)各持一个连接,进程内由递归锁串行化。BEGIN IMMEDIATE 事务兼作作用域
// 写锁(D6):写条目 / 重建索引 / 应用整合都在它里面完成,别的进程同一时刻只能等。
class MemoryStateStore {
public:
    static constexpr std::int64_t kTombstoneTtlMs = 90LL * 24 * 60 * 60 * 1000;

    explicit MemoryStateStore(std::filesystem::path db_path);
    ~MemoryStateStore();
    MemoryStateStore(const MemoryStateStore&) = delete;
    MemoryStateStore& operator=(const MemoryStateStore&) = delete;

    // 惰性打开;可重复调用。失败时返回 false 并写 error。
    bool open(std::string* error = nullptr);
    const std::filesystem::path& path() const { return db_path_; }

    // 租约:持有者令牌 + 到期时间;过期即可被别的持有者接管。
    bool try_acquire_lease(MemoryLeaseKind kind, const std::string& key,
                           const std::string& owner, std::int64_t ttl_ms,
                           std::int64_t now_ms);
    bool renew_lease(MemoryLeaseKind kind, const std::string& key,
                     const std::string& owner, std::int64_t ttl_ms, std::int64_t now_ms);
    void release_lease(MemoryLeaseKind kind, const std::string& key, const std::string& owner);

    std::optional<MemoryExtractionProgress> extraction(const std::string& session_id);
    // 提交提炼位置:只前进不后退(崩溃重放同一范围不会把位置拉回去),attempts 清零,
    // 记下本次看到的会话活动标记(没有新活动就不再挑中该会话)。
    bool commit_extraction(const std::string& session_id, const std::string& project_dir,
                           std::int64_t next_message_index, const std::string& seen_marker,
                           std::int64_t now_ms);
    bool record_extraction_failure(const std::string& session_id,
                                   const std::string& project_dir,
                                   const std::string& error, const std::string& marker,
                                   std::int64_t now_ms);
    bool forget_extraction(const std::string& session_id);

    std::optional<MemoryConsolidationProgress> consolidation(const std::string& scope_key);
    bool set_pending_plan(const std::string& scope_key, const std::string& plan,
                          const std::string& plan_hash, std::int64_t now_ms);
    bool commit_consolidation(const std::string& scope_key, const std::string& plan_hash,
                              std::int64_t now_ms);
    bool record_consolidation_failure(const std::string& scope_key, const std::string& error,
                                      const std::string& batch_signature, std::int64_t now_ms);
    bool clear_consolidation(const std::string& scope_key);

    bool add_tombstone(const std::string& scope_key, const std::string& name,
                       const std::string& title, std::int64_t now_ms);
    std::vector<MemoryTombstone> tombstones(const std::string& scope_key, std::int64_t now_ms);
    // 名字精确匹配或规范化标题相同(忽略大小写)且未过期即命中。
    bool is_tombstoned(const std::string& scope_key, const std::string& name,
                       const std::string& title, std::int64_t now_ms);
    int purge_expired_tombstones(std::int64_t now_ms);

    bool set_status(const std::string& key, const std::string& value, std::int64_t now_ms);
    std::optional<std::pair<std::string, std::int64_t>> status(const std::string& key);

private:
    friend class MemoryWriteLock;
    bool open_locked(std::string* error);
    bool exec_locked(const char* sql, std::string* error = nullptr);

    std::filesystem::path db_path_;
    std::recursive_mutex mu_;
    platform::UniqueSqlite db_;
    int tx_depth_ = 0;
};

// 跨进程写锁(D6)的 RAII 守卫:构造时在状态库上开 BEGIN IMMEDIATE,commit()
// 提交,未提交就析构则回滚。同一线程嵌套构造复用外层事务。store 为空或库不可用
// (打不开 / 等锁超时)时 acquired()=false,调用方仍持有进程内锁继续写 ——
// 记忆写入不能因为状态库出问题而失败。
class MemoryWriteLock {
public:
    explicit MemoryWriteLock(MemoryStateStore* store);
    ~MemoryWriteLock();
    MemoryWriteLock(const MemoryWriteLock&) = delete;
    MemoryWriteLock& operator=(const MemoryWriteLock&) = delete;

    bool acquired() const { return acquired_; }
    const std::string& error() const { return error_; }
    bool commit();

private:
    MemoryStateStore* store_ = nullptr;
    std::unique_lock<std::recursive_mutex> lock_;
    bool acquired_ = false;
    bool nested_ = false;
    bool finished_ = false;
    std::string error_;
};

// 标题规范化:去掉首尾空白、ASCII 转小写、连续空白折成一个空格。墓碑按它比较。
std::string normalize_memory_title(const std::string& title);

// 租约持有者令牌「pid-<pid>-<随机 hex>」,同进程内每次调用不同。
std::string make_memory_lease_owner();

std::int64_t memory_now_ms();

} // namespace acecode
