#pragma once

// PA 内网服务端「随机报 400 请求上下文过大」的兜底策略。目录定位与收录标准
// 见 src/pa/README.md 第 4 条。
//
// 这里只做决策,不碰 IO、不碰会话:给定「这一轮兜底已经做到哪一步」与「这次
// 被拒的请求长什么样」,回答「下一步做什么、等多久、缩到多大」。真正的修剪、
// 等待、重发都在 AgentLoop 里执行(run_pa_overflow_rescue)。纯函数,
// tests/pa/ 直接单测。
//
// 策略按顺序走,每一步之后都原样发一次请求;被拒的报文是服务端在准入时就
// 拦下的,一次往返只要不到一秒,所以多试几档的代价很低:
//   1. 原样重发 —— 先当它是抽风。重发 PA_RESCUE_SAME_REQUEST_RETRIES 次,过了
//      就一点上下文都不丢,也不给学习器记账(抽风不是证据)。
//   2. 逐档收缩 —— 每次被拒就把请求缩到上一次的 PA_RESCUE_SHRINK_PERCENT%:
//      先丢最旧的整组回合,老回合丢完了清本回合里最旧的工具输出。没有次数
//      上限,直到一点空间都腾不出来。
//   3. 紧急档 —— 剥掉工具定义与注入上下文,只留核心工具再发一次。
//   4. 等待重发 —— 最小请求还被拒,就按退避间隔反复原样重发,直到
//      PA_RESCUE_MAX_WAIT_RETRIES 次耗尽才放弃。用户可随时中止。
//
// 「一个 episode」= 从某次请求被拒到它(或它的缩减版)被服务端接受为止。
// 请求一旦被接受,调用方就把 RescueState 清零 —— 同一回合里后面再被拒是新的
// 一轮,重新从原样重发开始(新的工具输出进来了,可缩的东西也是新的)。

#include <string>

namespace acecode::pa {

// 原样重发的次数。两次足够分辨「抽风」与「真的太大」:真太大的请求两次都会
// 被秒拒,总共只多花几秒。
constexpr int PA_RESCUE_SAME_REQUEST_RETRIES = 2;

// 每一档把请求缩到上一次被拒规模的百分比。取 85% 而不是原来恢复链的 2/3:
// 服务端实际上限随负载浮动,大刀阔斧地砍会把本来只差一点的上下文一起丢掉;
// 小步多试,每一步都是一次快速往返,丢的只是刚好够用的那一截。
constexpr int PA_RESCUE_SHRINK_PERCENT = 85;

// 缩到最小仍被拒时的等待重发:次数上限与退避参数。5s 起每次翻倍、封顶 60s,
// 12 次约 9.5 分钟 —— 这是「服务端连最小请求都不收」时给它恢复的时间,期间
// 界面持续显示等待状态,用户可以随时停止。
constexpr int PA_RESCUE_MAX_WAIT_RETRIES = 12;
constexpr int PA_RESCUE_WAIT_BASE_MS = 5000;
constexpr int PA_RESCUE_WAIT_MAX_MS = 60000;

// 原样重发第 attempt 次(从 0 起)之前等多久:2s、5s。
int same_request_retry_delay_ms(int attempt);
// 最小请求仍被拒后第 attempt 次(从 0 起)等多久:5s 起每次翻倍,封顶 60s。
int wait_retry_delay_ms(int attempt);

// 单测用:所有等待按比例缩放(0 = 完全不等)。进程级、不落盘。
void set_rescue_wait_scale_for_test(double scale);
int scaled_rescue_wait_ms(int ms);

enum class RescueAction {
    RetrySameRequest,  // 原样重发,先等 wait_ms
    ShrinkHistory,     // 把会话历史缩到 target_history_tokens 后重发
    EmergencyProfile,  // 改用紧急请求档(剥掉工具定义与注入上下文)重发
    WaitAndRetry,      // 最小请求仍被拒:等 wait_ms 后原样重发
    GiveUp,            // 等待次数耗尽,放弃本回合
};

const char* to_string(RescueAction action);

// 一个 episode 的进度。
struct RescueState {
    bool active = false;
    int same_request_retries = 0;
    int shrink_rounds = 0;
    int wait_retries = 0;
    // 已经试过收缩但一点空间都没腾出来:老回合只剩一组、工具输出也清完了。
    // 之后不再提议收缩,直到 episode 结束。
    bool shrink_exhausted = false;
    // 本 episode 是否已经给 ContextBudgetLearner 记过账。
    bool rejection_recorded = false;
};

struct RescueInputs {
    int request_tokens = 0;          // 被拒请求的本地估算规模
    int history_tokens = 0;          // 其中会话历史的部分
    bool emergency_profile = false;  // 被拒的这次请求是否已经是紧急档
};

struct RescuePlan {
    RescueAction action = RescueAction::GiveUp;
    int wait_ms = 0;                // RetrySameRequest / WaitAndRetry 的等待
    int target_history_tokens = 0;  // ShrinkHistory:历史应缩到的规模
    // 这次被拒是否应记入 ContextBudgetLearner。只在原样重发也失败之后记,
    // 每个 episode 记一次,而且被拒的必须是正常档请求:抽风不算证据;紧急档
    // 请求没有工具定义与注入上下文,它的规模不代表正常请求能过的上限,记进去
    // 会把压缩窗口砍到远小于真实能力,把会话拖进「永远在压缩」。
    bool record_rejection = false;
    std::string label;  // 进度 / 日志文案(中文,给用户看)
};

RescuePlan next_rescue_step(const RescueState& state,
                            const RescueInputs& inputs);

// 按 plan 推进 state(计数、记账标记)。调用方在执行 plan 之前调用。
void advance_rescue_state(RescueState& state, const RescuePlan& plan);

}  // namespace acecode::pa
