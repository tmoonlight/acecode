"""Domain objects shared by ingestion, storage, and API layers."""

from dataclasses import dataclass
from datetime import datetime


@dataclass(frozen=True, slots=True)
class Entry:
    guid: str
    title: str
    url: str
    summary: str
    content: str
    author: str | None
    published_at: datetime | None


@dataclass(frozen=True, slots=True)
class FeedDocument:
    title: str
    site_url: str | None
    entries: tuple[Entry, ...]


@dataclass(frozen=True, slots=True)
class SearchResult:
    guid: str
    title: str
    url: str
    snippet: str
    source: str
    author: str | None
    published_at: datetime | None
