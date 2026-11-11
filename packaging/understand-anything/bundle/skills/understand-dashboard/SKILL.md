---
name: understand-dashboard
description: 启动本地交互式知识图谱 Dashboard，查看代码、领域、Wiki 或 Figma 图。
---

# /understand-dashboard

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

项目参数默认取当前工作目录。用 ACECode 的后台命令运行 `python "PACKAGE/scripts/ua.py" dashboard "项目" --no-open`，从该进程输出读取带 ?token= 的本地 URL，原样给用户。若用户请求打开浏览器，可省略 --no-open。保留服务进程，记录本次后台任务以便用户要求时关闭。复用给定项目的现有服务前先验证项目和 URL。没有图谱时提示选择 /understand、/understand-domain、/understand-knowledge 或 /understand-figma。
