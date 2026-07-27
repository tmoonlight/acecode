# ACECode Team 型专家团规范

## 结构原则

专家团是现有专家的引用集合：

```text
Team
├── leadExpert      -> 一个现有 Agent 专家
└── memberExperts  -> 一个或多个现有 Agent 专家
```

Team 包不复制成员的 Agent 文档、Skill 或头像。这样修改成员专家后，所有引用它的
专家团会自动使用最新定义。

## 选择成员

创建前读取 `~/.acecode/experts/*/expert.json`，只展示 `expertType: "agent"` 的
现有专家，让用户选择：

- 一位主理专家
- 至少一位成员专家

用户选择的是现成专家，不需要“搬入团队”。若某个角色尚不存在，先创建独立专家。

判断是否值得独立成专家：有没有用户会直接向它提出的问题。有就独立创建；没有就
作为主理人的内部步骤，不额外制造角色。

## 主理人职责

主理人负责：

1. 判断请求应由谁处理
2. 为成员提供完整任务、上下文和预期输出
3. 接收成员最终回复
4. 串联前后阶段
5. 汇总并对用户交付

主理人不应模拟成员完成专业产出，也不能调度未列入 Team 的专家。

## ACECode 调度协议

调用一个成员并等待：

```text
spawn_subagent(
  expert_member="<成员专家 ID>",
  prompt="<完整任务说明>",
  wait=true
)
```

并行调用互不依赖的成员：

1. 分别使用 `wait=false` 启动。
2. 保存每次返回的子会话 ID。
3. 使用 `wait_subagent(session_id="...")` 逐个收集结果。
4. 全部返回后再汇总。

串行阶段使用 `wait=true`，把上一阶段结论或明确文件路径写进下一阶段 Prompt。

### Prompt 必备内容

- 当前任务和阶段目标
- 用户已经确认的信息
- 需要读取的文件或前序结果
- 本成员负责与不负责的范围
- 期望输出格式
- 是否需要真实写入工作区文件

成员通过子会话最终回复返回结果，不使用 WorkBuddy 的 `SendMessage`。

## 约束

- 不调用 `TeamCreate`；用户选择 Team 时 ACECode 已完成绑定。
- `expert_member` 必须使用 `memberExperts` 中的专家 ID。
- 不 spawn 主理人自己。
- 成员子会话不能继续派生下级子会话。
- 不让成员互相直连；跨成员交接由主理人中转。
- 不把 Team 作为另一个 Team 的成员。
- 普通独立专家会话不能使用 `expert_member`。

## SOP 设计

为高频综合问题设计少量 Workflow。每个 Workflow 写明：

- 触发条件
- 每阶段调用谁
- 阶段之间是否存在输入依赖
- 哪些步骤可以并行
- 主理人最后如何汇总

没有依赖的研究或评审可并行；需要前序结论的设计、裁决或实施必须串行。

## 清单示例

```json
{
  "name": "delivery-team",
  "version": "1.0.0",
  "expertType": "team",
  "displayName": "交付专家团",
  "profession": "协作交付团队",
  "displayDescription": "由产品、开发和测试专家协作完成需求澄清、实现与验收。",
  "quickPrompts": [
    "让团队评估并实现这个需求",
    "让团队检查当前改动",
    "让团队分析这次交付为什么卡住"
  ],
  "defaultInitPrompt": "让团队评估并实现这个需求",
  "teamInfo": {
    "leadExpert": "delivery-lead",
    "memberExperts": [
      "product-manager",
      "developer",
      "tester"
    ]
  }
}
```
