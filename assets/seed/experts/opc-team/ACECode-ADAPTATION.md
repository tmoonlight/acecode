# ACECode 适配说明

本组件从本机 WorkBuddy 专家市场中的 `opc-team` 1.0.0 包转换而来。

保留内容：

- 一人企业方法论的阶段划分、角色职责和交互协议
- 1 名主理人和 8 名领域成员的提示词
- 9 个配套 Skill 及其参考资料
- `opc-doc/` 成果目录和会话恢复契约
- 原始头像、中文名称和快捷提问

运行时适配：

- WorkBuddy 的 `TeamCreate` 不再执行；在 ACECode 中，选择本专家团即完成团队绑定。
- WorkBuddy 的 Agent 调度改为
  `spawn_subagent(expert_member="<成员专家 ID>", prompt="...", wait=true)`。
- ACECode 的成员子会话不能继续派生下级子会话；跨成员信息由主理人通过
  子会话返回值和 `opc-doc/` 中转。

方法论主页：https://github.com/easychen/opc-methodology

许可：CC-BY-NC-SA-4.0
