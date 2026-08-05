"""FastAPI HTTP entry point."""

from __future__ import annotations

import os
import time
from datetime import datetime
from pathlib import Path

from fastapi import FastAPI, Query
from pydantic import BaseModel

from rss_search_service.infrastructure.database import Database


class SearchResultResponse(BaseModel):
    title: str
    url: str
    snippet: str
    source: str
    author: str | None
    published_at: datetime | None


class SearchResponse(BaseModel):
    query: str
    backend: str = "rss"
    took_ms: float
    results: list[SearchResultResponse]


def create_app(db_path: str | Path | None = None) -> FastAPI:
    resolved_path = db_path or os.environ.get("RSS_SEARCH_DB_PATH", "rss-search.db")
    database = Database(resolved_path)
    database.initialize()

    app = FastAPI(title="AceCode RSS Search", version="0.1.0")
    app.state.database = database

    @app.get("/health")
    def health() -> dict[str, object]:
        return {"status": "ok", "tokenizer": database.tokenizer, **database.stats()}

    @app.get("/v1/search", response_model=SearchResponse)
    def search(
        q: str = Query(min_length=1, max_length=200),
        limit: int = Query(default=10, ge=1, le=100),
        since_days: int | None = Query(default=None, ge=1, le=3650),
    ) -> SearchResponse:
        started = time.perf_counter()
        results = database.search(q, limit=limit, since_days=since_days)
        took_ms = round((time.perf_counter() - started) * 1000, 3)
        return SearchResponse(
            query=q,
            took_ms=took_ms,
            results=[
                SearchResultResponse(
                    title=item.title,
                    url=item.url,
                    snippet=item.snippet,
                    source=item.source,
                    author=item.author,
                    published_at=item.published_at,
                )
                for item in results
            ],
        )

    return app


app = create_app()
