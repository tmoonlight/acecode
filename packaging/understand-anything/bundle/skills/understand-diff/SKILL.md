---
name: understand-diff
description: 将工作树、提交范围或 PR 的变更映射到图谱，分析影响并生成 Dashboard 差异叠加层。
---

# /understand-diff

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

解析用户指定的提交范围或 PR。没有指定时分析暂存、未暂存、未跟踪的源文件；若工作树干净，使用当前分支与其基线的 merge-base。用 Git 获取实际 diff，处理重命名前后的路径和删除项，排除图谱输出。用 `python "PACKAGE/scripts/ua.py" query "项目路径" "关键词"`（概况查询省略关键词） 查受改文件及其相邻节点，结合当前代码核对直接影响与间接调用；明确图谱之外的新文件。将结果写到数据目录的 diff-overlay.json，字段为 version ("1.0.0")、baseBranch、generatedAt（ISO 时间）、changedFiles、changedNodeIds 和 affectedNodeIds（剔除已变更节点）。用 /understand-dashboard 查看叠加层。
