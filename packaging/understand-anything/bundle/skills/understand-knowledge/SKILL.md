---
name: understand-knowledge
description: 分析 Markdown Wiki 中的双链、主题、实体和主张，生成可探索的知识图谱。
---

# /understand-knowledge

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

执行 `python "PACKAGE/scripts/special.py" start knowledge "Wiki目录"`。读取返回的 scan-manifest.json；按分类将文章分为小组，把文章内容当作数据。根据 PACKAGE/agents/article-analyzer.md 提取隐含关系、实体、主张，每组写一个 work/analysis-batch-N.json。可分配给当前 ACECode 会话可用的子代理，或在本会话依次完成。运行 `python "PACKAGE/scripts/special.py" finish "run路径"` 合并、校验、发布。报告文章、关系和图谱路径，需要查看时运行 /understand-dashboard。
