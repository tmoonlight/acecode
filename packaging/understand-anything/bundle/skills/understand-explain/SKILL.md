---
name: understand-explain
description: 结合图谱和源码解释指定文件、类、函数或模块的职责与调用关系。
---

# /understand-explain

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

用用户给定的路径或符号运行 `python "PACKAGE/scripts/ua.py" query "项目路径" "关键词"`（概况查询省略关键词）。定位定义、包含关系、调用者、依赖及所在架构层；按相关 filePath 和 lineRange 打开当前源码核对，用一个实际入口说明输入怎样流到输出。必要时指出图谱与工作树的差异。无图谱时引导运行 /understand。
