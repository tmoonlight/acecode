---
name: understand-domain
description: 从现有图谱或直接从源码提取领域、业务流程和步骤，生成领域流程图。
---

# /understand-domain

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

执行 `python "PACKAGE/scripts/special.py" start domain "项目"`；--full 强制从源码提取上下文。读取返回的 context 及相关业务入口。阅读 PACKAGE/agents/domain-analyzer.md，用它规定的图结构和 domainMeta 创建 work/domain-analysis.json；保留具体领域、流程、步骤及它们的关系。可把这个角色、上下文和准确输出路径交给 ACECode 可用的子代理，也可在本会话完成。执行 `python "PACKAGE/scripts/special.py" finish "run路径"`，修复校验错误后重试，报告 domain-graph.json 的路径。需要展示时运行 /understand-dashboard。
