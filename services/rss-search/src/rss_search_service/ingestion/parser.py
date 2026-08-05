"""Parse RSS, Atom, and JSON Feed into normalized domain objects."""

from __future__ import annotations

import calendar
import hashlib
import json
import re
from datetime import UTC, datetime
from html.parser import HTMLParser
from typing import Any

import feedparser

from rss_search_service.domain.models import Entry, FeedDocument


class FeedParseError(ValueError):
    pass


class _TextExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []

    def handle_data(self, data: str) -> None:
        self.parts.append(data)


def _plain_text(value: Any) -> str:
    if not value:
        return ""
    parser = _TextExtractor()
    parser.feed(str(value))
    return re.sub(r"\s+", " ", " ".join(parser.parts)).strip()


def _datetime_from_struct(value: Any) -> datetime | None:
    if not value:
        return None
    return datetime.fromtimestamp(calendar.timegm(value), tz=UTC)


def _datetime_from_iso(value: Any) -> datetime | None:
    if not value:
        return None
    try:
        parsed = datetime.fromisoformat(str(value))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=UTC)
    return parsed.astimezone(UTC)


def _fallback_guid(url: str, title: str, content: str) -> str:
    return hashlib.sha256(f"{url}\n{title}\n{content}".encode()).hexdigest()


def _parse_json_feed(payload: bytes, source_url: str) -> FeedDocument:
    try:
        data = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FeedParseError("invalid JSON Feed") from exc
    if not isinstance(data, dict) or not str(data.get("version", "")).startswith("https://jsonfeed.org/version/"):
        raise FeedParseError("document is not a JSON Feed")

    entries: list[Entry] = []
    for item in data.get("items", []):
        if not isinstance(item, dict):
            continue
        title = _plain_text(item.get("title"))
        url = str(item.get("url") or item.get("external_url") or "").strip()
        content = _plain_text(item.get("content_html") or item.get("content_text"))
        summary = _plain_text(item.get("summary")) or content
        guid = str(item.get("id") or _fallback_guid(url, title, content))
        authors = item.get("authors") or []
        author = None
        if authors and isinstance(authors[0], dict):
            author = _plain_text(authors[0].get("name")) or None
        entries.append(
            Entry(
                guid=guid,
                title=title or "(untitled)",
                url=url,
                summary=summary,
                content=content,
                author=author,
                published_at=_datetime_from_iso(item.get("date_published") or item.get("date_modified")),
            )
        )
    return FeedDocument(
        title=_plain_text(data.get("title")) or source_url,
        site_url=str(data.get("home_page_url") or "").strip() or None,
        entries=tuple(entries),
    )


def _parse_xml_feed(payload: bytes, source_url: str) -> FeedDocument:
    parsed = feedparser.parse(payload)
    if not parsed.feed and not parsed.entries:
        raise FeedParseError("document is not a valid RSS or Atom feed")

    entries: list[Entry] = []
    for item in parsed.entries:
        title = _plain_text(item.get("title"))
        url = str(item.get("link") or "").strip()
        raw_content = ""
        if item.get("content"):
            raw_content = item.content[0].get("value", "")
        content = _plain_text(raw_content)
        summary = _plain_text(item.get("summary") or item.get("description"))
        if not content:
            content = summary
        guid = str(item.get("id") or item.get("guid") or _fallback_guid(url, title, content))
        entries.append(
            Entry(
                guid=guid,
                title=title or "(untitled)",
                url=url,
                summary=summary or content,
                content=content,
                author=_plain_text(item.get("author")) or None,
                published_at=_datetime_from_struct(item.get("published_parsed") or item.get("updated_parsed")),
            )
        )

    site_url = str(parsed.feed.get("link") or "").strip() or None
    return FeedDocument(
        title=_plain_text(parsed.feed.get("title")) or source_url,
        site_url=site_url,
        entries=tuple(entries),
    )


def parse_feed(payload: bytes, content_type: str | None, source_url: str) -> FeedDocument:
    if not payload.strip():
        raise FeedParseError("empty feed")
    media_type = (content_type or "").lower()
    if "json" in media_type or payload.lstrip().startswith((b"{", b"[")):
        return _parse_json_feed(payload, source_url)
    return _parse_xml_feed(payload, source_url)
