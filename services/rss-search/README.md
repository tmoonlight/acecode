# AceCode RSS Search

AceCode 的托管式 RSS/Atom/JSON Feed 搜索服务。MVP 目标是将精选技术源稳定采集到 SQLite，并通过 HTTP API 提供中英文全文搜索。

## Development

```bash
uv sync
uv run pytest
uv run uvicorn rss_search_service.api.app:create_app --factory --host 127.0.0.1 --port 47778
```

接口：

- `GET /health`
- `GET /v1/search?q=Claude&limit=10&since_days=30`

服务默认使用 `RSS_SEARCH_DB_PATH=./rss-search.db`。外部 Feed 数据始终视为不可信内容。
