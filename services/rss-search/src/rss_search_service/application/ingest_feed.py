"""Application use cases for feed ingestion."""

from __future__ import annotations

from dataclasses import dataclass

from rss_search_service.infrastructure.database import Database
from rss_search_service.ingestion.fetcher import fetch_feed
from rss_search_service.ingestion.parser import parse_feed


@dataclass(frozen=True, slots=True)
class IngestResult:
    feed_id: int
    entries_written: int
    title: str


async def ingest_feed(database: Database, url: str) -> IngestResult:
    fetched = await fetch_feed(url)
    document = parse_feed(fetched.content, fetched.content_type, fetched.url)
    feed_id = database.upsert_feed(fetched.url, document.title, document.site_url)
    count = database.upsert_entries(feed_id, document.entries)
    return IngestResult(feed_id=feed_id, entries_written=count, title=document.title)
