#include <gtest/gtest.h>

#include "pa/pa_adapter.hpp"
#include "pa/pa_context_budget.hpp"

namespace {

using acecode::pa::ContextBudgetLearner;
using acecode::pa::observation_is_credible;
using acecode::pa::suggest_effective_window;

constexpr const char* kProvider = "openai";
constexpr const char* kModel = "aicoder-pro";

} // namespace

// 触发场景:从未被服务端拒绝过的模型。
// 期望行为:原样返回声明窗口。适配层在没有证据的时候不猜 —— 绝大多数模型
// 走不到这条路径,任何「预防性收缩」都是在无故浪费用户买单的上下文。
TEST(PaContextBudget, NoObservationLeavesDeclaredWindowUntouched) {
    EXPECT_EQ(suggest_effective_window(128000, 0), 128000);

    ContextBudgetLearner learner;
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 128000);
    EXPECT_FALSE(learner.has_observation(kProvider, kModel));
}

// 触发场景:声明 128k,实测服务端在 70k 处就拒收。
// 期望行为:压缩窗口收敛到 70k 的 85%,即 59500。
// 15% 余量的来由:本地 token 估算是 ceil(bytes / 4),而中文在 UTF-8 下是
// 3 字节/字、约 1 token/字,估算值只有真实值的 ~0.75 倍。中文会话里本地估算
// 系统性偏低,不留这段余量,收敛后的阈值仍然会越过服务端真实上限。
TEST(PaContextBudget, RejectionConvergesWindowBelowTheObservedLine) {
    EXPECT_EQ(suggest_effective_window(128000, 70000), 59500);

    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 70000);
    EXPECT_TRUE(learner.has_observation(kProvider, kModel));
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 59500);
}

// 触发场景:服务端上限飘忽,先在 70k 被拒,后来又在 50k 被拒。
// 期望行为:取观测到的最小值收敛(只降不升)。波动环境里唯一安全的选择是相信
// 最严格的那次观测 —— 目标是少撞墙,而不是把窗口用满。
TEST(PaContextBudget, ObservationsAreMonotonicallyDecreasing) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 70000);
    learner.note_rejected(kProvider, kModel, 50000);
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 42500);

    // 之后又在更大的规模被拒,不该把已经收紧的上限放回去。
    learner.note_rejected(kProvider, kModel, 90000);
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 42500);
}

// 触发场景:某次成功的请求规模比已知的被拒规模还大。
// 期望行为:上限不因此放宽。让「成功」抬高上限会在限制波动时来回震荡,反而
// 更常撞墙;成功观测只留作诊断。
TEST(PaContextBudget, AcceptedRequestsDoNotWidenTheWindow) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 50000);
    learner.note_accepted(kProvider, kModel, 90000);
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 42500);
}

// 触发场景:先记下一次 40k 的成功,之后在 30k 处被拒。
// 期望行为:那条成功观测被作废。它比新的被拒线还高,说明「那次能过」已经是
// 历史;留着只会让诊断输出自相矛盾(显示成功规模大于被拒规模)。
TEST(PaContextBudget, StaleAcceptedObservationIsDroppedOnLowerRejection) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 60000);
    learner.note_accepted(kProvider, kModel, 40000);
    learner.note_rejected(kProvider, kModel, 30000);

    const auto snapshot = learner.snapshot();
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].lowest_rejected_tokens, 30000);
    EXPECT_EQ(snapshot[0].highest_accepted_tokens, 0);
    EXPECT_EQ(snapshot[0].rejections, 2);
}

// 触发场景:从未撞过墙的模型上报了一次成功。
// 期望行为:不建表。观测表只为真正出过问题的模型存在,否则每个用过的模型都会
// 在表里占一条,而 has_observation 也就失去了「跳过全量 token 估算」的意义。
TEST(PaContextBudget, AcceptedAloneDoesNotCreateAnEntry) {
    ContextBudgetLearner learner;
    learner.note_accepted(kProvider, kModel, 40000);
    EXPECT_TRUE(learner.snapshot().empty());
    EXPECT_FALSE(learner.has_observation(kProvider, kModel));
}

// 触发场景:服务端在一个小到不可能撑爆任何模型的规模上报「上下文过大」。
// 期望行为:整条观测丢弃,窗口不动。
// 回归背景:第一版把这个阈值当成「收敛的下界」而不是「观测的可信下限」——
// 一次 2726 token 的拒绝就把声明 128000 的窗口砍到了 8192(6%),之后每个回合
// 都在压缩。单测里表现为 AgentLoopCompactEvents(内含构造上下文溢出的用例)
// 跑完后,AgentLoopToolResultStorage 的一个无关用例开始失败:它断言的那条
// tool 消息被提前触发的自动压缩摘要掉了。
TEST(PaContextBudget, ImplausiblySmallRejectionsAreNotBelieved) {
    EXPECT_FALSE(observation_is_credible(2726));
    EXPECT_EQ(suggest_effective_window(128000, 2726), 128000);
    EXPECT_EQ(suggest_effective_window(128000, 100), 128000);

    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 2726);
    EXPECT_FALSE(learner.has_observation(kProvider, kModel));
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 128000);
}

// 触发场景:先来一条不可信的小规模拒绝,之后才来真实的大规模拒绝。
// 期望行为:小的那条不进表,不妨碍后面真实观测生效。
// 这是「入口丢弃」而不是「查询时过滤」的理由:observation 单调取最小值,
// 噪声一旦进表就会永久钉死 lowest_rejected,真实观测再也压不进来。
TEST(PaContextBudget, NoiseDoesNotBlockLaterCredibleObservations) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 2726);
    learner.note_rejected(kProvider, kModel, 70000);
    EXPECT_TRUE(learner.has_observation(kProvider, kModel));
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 59500);
}

// 触发场景:声明窗口本来就比收敛结果更小(用户已经手动配窄了)。
// 期望行为:取更小的那个。适配层只收不放,不能反过来把用户配置的窗口撑大。
TEST(PaContextBudget, DeclaredWindowStillCapsTheResult) {
    EXPECT_EQ(suggest_effective_window(30000, 70000), 30000);
}

// 触发场景:声明窗口未知(<= 0,例如模型目录里查不到)。
// 期望行为:观测值成为唯一信息源,直接采用。
TEST(PaContextBudget, UnknownDeclaredWindowFallsBackToObservation) {
    EXPECT_EQ(suggest_effective_window(0, 70000), 59500);
    EXPECT_EQ(suggest_effective_window(0, 0), 0);
}

// 触发场景:同一个 provider 下多个模型,只有其中一个撞过墙。
// 期望行为:互不干扰。服务端的限制是按模型来的 —— 把观测串到别的模型上会让
// 好模型无故被收窄。
TEST(PaContextBudget, ObservationsAreScopedPerModel) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 50000);
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 42500);
    EXPECT_EQ(learner.effective_window(kProvider, "other-model", 128000),
              128000);
    EXPECT_EQ(learner.effective_window("other-provider", kModel, 128000),
              128000);
}

// 触发场景:模型身份取不到(provider 快照缺席,active_model_identity 返回空)。
// 期望行为:整条观测丢弃,查表也不干预。
// 回归背景:最初的实现把空身份当成一个普通 key,结果所有身份不明的观测都落进
// 同一个匿名桶 —— 单测里表现为 AgentLoopCompactEvents 跑完(其中有构造上下文
// 溢出的用例)之后,AgentLoopToolResultStorage 的一个无关用例开始失败:匿名桶
// 被收窄导致自动压缩提前触发,把它要断言的那条 tool 消息摘要掉了。生产里同样
// 成立 —— A 模型的一次拒绝会拖累毫不相干的 B 模型。
TEST(PaContextBudget, UnknownIdentityIsIgnoredEntirely) {
    ContextBudgetLearner learner;
    learner.note_rejected("", "", 50000);
    learner.note_rejected(kProvider, "", 50000);
    EXPECT_TRUE(learner.snapshot().empty());
    EXPECT_FALSE(learner.has_observation("", ""));
    EXPECT_EQ(learner.effective_window("", "", 128000), 128000);
    EXPECT_EQ(learner.effective_window(kProvider, "", 128000), 128000);

    // 只有 provider 为空、model 明确时仍算已知 —— model id 足以定位模型。
    learner.note_rejected("", kModel, 50000);
    EXPECT_EQ(learner.effective_window("", kModel, 128000), 42500);
}

// 触发场景:关掉 PA 适配总开关。
// 期望行为:既不记录也不干预,整层退回适配前的行为 —— 开关必须同时覆盖判定
// 与自适应预算,只关一半会让「关掉验证一下」得出错误结论。
TEST(PaContextBudget, MasterSwitchDisablesLearningAndConvergence) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 50000);
    ASSERT_EQ(learner.effective_window(kProvider, kModel, 128000), 42500);

    acecode::pa::set_enabled(false);
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 128000);
    EXPECT_FALSE(learner.has_observation(kProvider, kModel));
    learner.note_rejected(kProvider, kModel, 10000);
    acecode::pa::set_enabled(true);

    // 关闭期间的那次拒绝没有被记下来,上限仍是关闭前学到的值。
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 42500);
}

// 触发场景:拿到的估算值是 0 或负数(估算失败)。
// 期望行为:整条观测丢弃 —— 同样走可信下限那道门,不需要单独的特判。
TEST(PaContextBudget, NonPositiveObservationsAreIgnored) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 0);
    learner.note_rejected(kProvider, kModel, -1);
    EXPECT_FALSE(learner.has_observation(kProvider, kModel));
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 128000);
}

// 触发场景:reset 之后重新开始学。
// 期望行为:回到「没有观测」的初始状态。服务端限制会变,用户需要一个不重启
// 进程就能重新学的入口。
TEST(PaContextBudget, ResetClearsAllObservations) {
    ContextBudgetLearner learner;
    learner.note_rejected(kProvider, kModel, 50000);
    learner.reset();
    EXPECT_TRUE(learner.snapshot().empty());
    EXPECT_EQ(learner.effective_window(kProvider, kModel, 128000), 128000);
}
