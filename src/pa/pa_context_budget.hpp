#pragma once

// 自适应上下文预算:从「被服务端拒绝」这件事里反推该模型实际能吃多大的
// 请求,把压缩阈值收敛到那条线之下。目录定位见 src/pa/README.md。
//
// 存在的理由:自动压缩的触发点是「声明窗口 × 90%」。当 saved_models 里声明的
// context_window 远大于服务端真实上限时,这个阈值永远够不到 —— 每一轮都要先
// 撞一次墙、触发一次恢复重试才能继续。恢复链能救回来,但每次撞墙都实打实多花
// 一个完整请求的时间和 token。
//
// 只在撞过墙之后才生效,没撞过完全不干预。

#include "pa_adapter.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::pa {

// 观测是否可归属到一个明确的模型。model 为空即身份不明 —— 只知道 provider
// 不足以定位模型,一个 provider 下的多个模型限制各不相同。
//
// 身份不明时**既不记录也不查表**。观测的全部意义是「这个模型能吃多大」,
// 归属不清的观测不只是没用:它会落进同一个匿名桶,让 A 模型的一次拒绝拖累
// 毫不相干的 B 模型。fail-open(不干预)是适配层唯一正确的失败方向。
bool identity_is_known(const std::string& provider, const std::string& model);

// 被拒规模到建议上限之间留出的安全余量(百分比)。
//
// 15% 不是拍脑袋:ACECode 的 token 估算是 ceil(bytes / 4)(见
// compact.cpp::approx_token_count),而中文在 UTF-8 下是 3 字节/字、约 1
// token/字,也就是估算值只有真实值的 ~0.75 倍。中文为主的会话里本地估算
// 系统性偏低,余量必须覆盖这段偏差,否则收敛后的阈值仍然会越过真实上限。
constexpr int PA_CONTEXT_BUDGET_SAFETY_PERCENT = 85;

// 一条被拒观测可信的最小规模。低于这个值的请求不可能撑爆任何**能用**的模型
// —— 光是 system prompt、skill 索引和工具定义就有好几 k。这种拒绝几乎必然
// 另有原因(服务端自己算错、错误文案被复用到别的失败上),采信它会把窗口砍到
// 远小于模型真实能力,把会话拖进「永远在压缩」。
//
// 所以一条观测只有两种下场:**可信则采信并收敛,不可信则整条丢弃**,没有
// 「收敛但夹到下限」的中间态 —— 那种中间态正是本适配第一版的 bug:一次
// 2726 token 的拒绝把声明 128000 的窗口砍到了 8192。
constexpr int PA_CONTEXT_BUDGET_MIN_CREDIBLE_REJECTION_TOKENS = 8192;

// 这条被拒观测是否值得采信。
bool observation_is_credible(int rejected_tokens);

// 纯函数形式的收敛算法,便于单测。
//   declared_window        —— saved_models / 目录声明的窗口,<= 0 表示未知
//   lowest_rejected_tokens —— 观测到的最小被拒请求规模,0 表示没撞过墙
// 返回应当用于压缩决策的窗口。没有可信观测时原样返回 declared_window。
int suggest_effective_window(int declared_window, int lowest_rejected_tokens);

// 一个 (provider, model) 的观测快照,用于日志与状态展示。
struct ContextBudgetSnapshot {
    std::string provider;
    std::string model;
    int lowest_rejected_tokens = 0;
    int highest_accepted_tokens = 0;
    int rejections = 0;
};

// 进程级观测表。**刻意不落盘**:服务端的实际上限会变,把一次偶发的低水位
// 永久钉死比不学更糟 —— 用户重启一次就该重新学。
class ContextBudgetLearner {
public:
    // 记录一次被服务端以「上下文过大」拒绝的请求规模。不可信的规模在入口就
    // 丢弃 —— 让它进表会永久钉死 lowest_rejected,之后真实的观测再也压不进来。
    // 观测单调收敛(只降不升):目标是少撞墙,波动环境里取观测到的最小被拒
    // 规模作为上限是唯一安全的选择。
    void note_rejected(const std::string& provider,
                       const std::string& model,
                       int request_tokens);

    // 记录一次被服务端接受的请求规模。只作诊断用,不参与上限计算 ——
    // 让「成功」抬高上限会在限制波动时来回震荡,反而更常撞墙。
    void note_accepted(const std::string& provider,
                       const std::string& model,
                       int request_tokens);

    // 该模型应当用于压缩决策的窗口。无观测时原样返回 declared_window。
    int effective_window(const std::string& provider,
                         const std::string& model,
                         int declared_window) const;

    // 是否已经为该模型收缩过窗口(供调用方决定要不要提示用户)。
    bool has_observation(const std::string& provider,
                         const std::string& model) const;

    std::vector<ContextBudgetSnapshot> snapshot() const;

    // 供测试与「重新学一遍」使用。
    void reset();

private:
    struct Entry {
        int lowest_rejected_tokens = 0;
        int highest_accepted_tokens = 0;
        int rejections = 0;
    };

    mutable std::mutex mu_;
    std::map<std::string, Entry> entries_;
};

// 进程级单例。AgentLoop 在会话之间共享同一份观测 —— 服务端的限制是按模型
// 来的,不是按会话来的,每个会话各撞一次墙纯属浪费。
ContextBudgetLearner& context_budget();

} // namespace acecode::pa
