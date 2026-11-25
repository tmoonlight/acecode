#pragma once

// 自动模式（试验）的拦截计数(openspec add-auto-review-mode,design D4)。
//
// 会话级状态,由审批门在每次裁决后更新:
//   - 连续拦截:一次裁决放行就清零。
//   - 累计拦截:不因放行清零;用户在退回的弹窗上作答后清零。
//   - 任一计数达到上限 = limit_reached()。有人值守时,下一次被拦的调用改为弹窗;
//     无人可问时进入锁定,此后需要裁决的调用直接拒绝、不再调裁决模型。
// 「没有结果」(请求失败、超时、解析不出)不计数,调用方不要为它调 record_block。
//
// 线程安全:同一批工具可能并行触发多次裁决。内部一把叶子锁,不在锁内回调。

#include <cstdint>
#include <mutex>
#include <string>

namespace acecode::auto_review {

struct DenialLimits {
    // 与 Claude Code 相同的取值:连续 3 次、累计 20 次。
    int max_consecutive = 3;
    int max_total = 20;
};

struct DenialCounts {
    int consecutive = 0;
    int total = 0;
    bool locked = false;
};

class DenialTracker {
public:
    explicit DenialTracker(DenialLimits limits = {}) : limits_(limits) {}

    // 裁决放行:连续计数清零。
    void record_allow();
    // 裁决拦下:两个计数各加一,返回加完之后是否达到上限。
    bool record_block();
    // 任一计数已达到上限。
    bool limit_reached() const;
    // 用户在因计数退回的弹窗上作了答(允许或拒绝都算):两个计数清零。
    void record_human_answer();

    // 无人可问时锁定。anchor 是锁定那一刻「最后一条用户亲发消息」的标识;
    // 之后 unlock_if_new_user_turn 看到不同的标识,说明用户回来了。
    void lock(std::string anchor);
    bool locked() const;
    // 已锁定且 anchor 变了:解除锁定并清零计数,返回 true。未锁定时什么也不做。
    bool unlock_if_new_user_turn(const std::string& anchor);

    // 权限模式每变一次,PermissionManager 的 generation 加一。计数只在同一段
    // 「连续处于本模式」内有意义:generation 变了就整体清零(切出再切回也算)。
    void sync_mode_generation(std::uint64_t generation);

    void reset();
    DenialCounts counts() const;

private:
    void reset_locked();

    const DenialLimits limits_;
    mutable std::mutex mu_;   // 叶子锁:只保护下面的值。
    int consecutive_ = 0;
    int total_ = 0;
    bool locked_ = false;
    std::string lock_anchor_;
    std::uint64_t generation_ = 0;
    bool generation_known_ = false;
};

} // namespace acecode::auto_review
