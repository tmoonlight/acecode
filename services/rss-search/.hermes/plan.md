# Plan: RSS 搜索服务 MVP

## Goal
在 AceCode 仓库中交付一个可运行、可测试的 RSS 搜索服务，支持 RSS/Atom/JSON Feed 入库、SQLite FTS5 检索、健康检查与 HTTP 搜索 API。

## Context
- 项目位置：`services/rss-search/`
- Git 分支：`feat/rss-search-service`
- 服务端与后续 ACECode C++ 客户端适配器解耦。
- 第一阶段只实现可靠 MVP，不做公开 Feed 提交、向量库或浏览器抓取。
- 依赖方向：API → Application → Domain；基础设施通过清晰接口提供存储与抓取能力。

## Steps

### Step 1: 建立工程与测试骨架
- What: 创建 `pyproject.toml`、src layout、pytest 配置和 fixtures。
- Files: `pyproject.toml`, `src/rss_search_service/`, `tests/`
- Verify: pytest 能收集并执行测试。

### Step 2: 实现 Feed 标准化
- What: 解析 RSS 2.0、Atom、JSON Feed，输出统一 Feed/Entry 模型。
- Files: `domain/models.py`, `ingestion/parser.py`, parser tests
- Verify: 三类格式、缺失可选字段、无效输入测试通过。

### Step 3: 实现 SQLite 存储与搜索
- What: schema、upsert 去重、FTS5 trigram/unicode61 回退、时间筛选和 BM25 排序。
- Files: `infrastructure/database.py`, `search/service.py`, repository tests
- Verify: 中英文查询、重复写入、时间筛选测试通过。

### Step 4: 实现采集与 API
- What: 限制响应大小和超时的 HTTP 抓取；提供 `/health` 与 `/v1/search`。
- Files: `ingestion/fetcher.py`, `application/service.py`, `api/app.py`
- Verify: API 测试与真实 Feed 采集验证通过。

### Step 5: 文档、检查与提交
- What: README、示例配置、lint/test、自查、首个提交。
- Verify: 完整测试通过，Git 工作区仅含预期文件，提交成功。

## Risks / unknowns
- 系统 SQLite 可能不支持 trigram tokenizer：启动时探测并回退 unicode61。
- Feed 时间格式不一致：统一解析为 UTC，缺失时允许为空。
- 外部 Feed 不可信：限制协议、响应大小、超时，并拒绝私网地址。

## Done when
- 三类 Feed 均可解析并入库。
- 可通过 API 搜索中英文内容。
- `/health` 返回数据库和文章统计。
- 单元/API/真实抓取验证均有真实通过输出。
- 代码位于独立 Git 功能分支并有初始提交。
