"""SQLite repositories and full-text index."""

from __future__ import annotations

import hashlib
import sqlite3
from collections.abc import Iterable
from datetime import UTC, datetime, timedelta
from pathlib import Path

from rss_search_service.domain.models import Entry, SearchResult


class Database:
    def __init__(self, path: str | Path) -> None:
        self.path = str(path)
        self.tokenizer = "trigram"

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA journal_mode = WAL")
        return connection

    def initialize(self) -> None:
        with self._connect() as connection:
            connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS feeds (
                    id INTEGER PRIMARY KEY,
                    url TEXT NOT NULL UNIQUE,
                    title TEXT NOT NULL,
                    site_url TEXT,
                    etag TEXT,
                    last_modified TEXT,
                    last_fetched_at TEXT,
                    consecutive_errors INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS entries (
                    id INTEGER PRIMARY KEY,
                    feed_id INTEGER NOT NULL REFERENCES feeds(id) ON DELETE CASCADE,
                    guid TEXT NOT NULL,
                    title TEXT NOT NULL,
                    url TEXT NOT NULL,
                    summary TEXT NOT NULL DEFAULT '',
                    content TEXT NOT NULL DEFAULT '',
                    author TEXT,
                    published_at TEXT,
                    fetched_at TEXT NOT NULL,
                    content_hash TEXT NOT NULL,
                    UNIQUE(feed_id, guid)
                );
                CREATE INDEX IF NOT EXISTS entries_published_at_idx ON entries(published_at);
                """
            )
            try:
                connection.execute(
                    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(entry_id UNINDEXED, title, summary, content, source, tokenize='trigram')"
                )
            except sqlite3.OperationalError:
                self.tokenizer = "unicode61"
                connection.execute(
                    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(entry_id UNINDEXED, title, summary, content, source, tokenize='unicode61')"
                )

    def upsert_feed(self, url: str, title: str, site_url: str | None) -> int:
        with self._connect() as connection:
            connection.execute(
                """
                INSERT INTO feeds(url, title, site_url) VALUES (?, ?, ?)
                ON CONFLICT(url) DO UPDATE SET title=excluded.title, site_url=excluded.site_url
                """,
                (url, title, site_url),
            )
            row = connection.execute("SELECT id FROM feeds WHERE url = ?", (url,)).fetchone()
            assert row is not None
            return int(row["id"])

    def upsert_entries(self, feed_id: int, entries: Iterable[Entry]) -> int:
        count = 0
        fetched_at = datetime.now(UTC).isoformat()
        with self._connect() as connection:
            source_row = connection.execute("SELECT title FROM feeds WHERE id = ?", (feed_id,)).fetchone()
            if source_row is None:
                raise KeyError(f"feed {feed_id} does not exist")
            source = str(source_row["title"])
            for entry in entries:
                content_hash = hashlib.sha256(
                    f"{entry.title}\n{entry.url}\n{entry.summary}\n{entry.content}".encode()
                ).hexdigest()
                connection.execute(
                    """
                    INSERT INTO entries(
                        feed_id, guid, title, url, summary, content, author,
                        published_at, fetched_at, content_hash
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(feed_id, guid) DO UPDATE SET
                        title=excluded.title, url=excluded.url, summary=excluded.summary,
                        content=excluded.content, author=excluded.author,
                        published_at=excluded.published_at, fetched_at=excluded.fetched_at,
                        content_hash=excluded.content_hash
                    """,
                    (
                        feed_id,
                        entry.guid,
                        entry.title,
                        entry.url,
                        entry.summary,
                        entry.content,
                        entry.author,
                        entry.published_at.isoformat() if entry.published_at else None,
                        fetched_at,
                        content_hash,
                    ),
                )
                row = connection.execute(
                    "SELECT id FROM entries WHERE feed_id = ? AND guid = ?", (feed_id, entry.guid)
                ).fetchone()
                assert row is not None
                entry_id = int(row["id"])
                connection.execute("DELETE FROM entries_fts WHERE entry_id = ?", (entry_id,))
                connection.execute(
                    "INSERT INTO entries_fts(entry_id, title, summary, content, source) VALUES (?, ?, ?, ?, ?)",
                    (entry_id, entry.title, entry.summary, entry.content, source),
                )
                count += 1
        return count

    def search(self, query: str, *, limit: int = 10, since_days: int | None = None) -> list[SearchResult]:
        normalized = " ".join(query.split()).strip()
        if not normalized:
            return []
        since = None
        if since_days is not None:
            since = (datetime.now(UTC) - timedelta(days=since_days)).isoformat()

        use_like = len(normalized) < 3
        params: list[object]
        if use_like:
            pattern = f"%{normalized}%"
            sql = """
                SELECT e.*, f.title AS source
                FROM entries e JOIN feeds f ON f.id = e.feed_id
                WHERE (e.title LIKE ? OR e.summary LIKE ? OR e.content LIKE ? OR f.title LIKE ?)
                  AND (? IS NULL OR e.published_at IS NULL OR e.published_at >= ?)
                ORDER BY e.published_at IS NULL, e.published_at DESC
                LIMIT ?
            """
            params = [pattern, pattern, pattern, pattern, since, since, limit]
        else:
            match_query = '"' + normalized.replace('"', '""') + '"'
            sql = """
                SELECT e.*, f.title AS source, bm25(entries_fts, 0.0, 5.0, 2.0, 1.0, 1.5) AS rank
                FROM entries_fts
                JOIN entries e ON e.id = entries_fts.entry_id
                JOIN feeds f ON f.id = e.feed_id
                WHERE entries_fts MATCH ?
                  AND (? IS NULL OR e.published_at IS NULL OR e.published_at >= ?)
                ORDER BY rank ASC, e.published_at IS NULL, e.published_at DESC
                LIMIT ?
            """
            params = [match_query, since, since, limit]

        with self._connect() as connection:
            rows = connection.execute(sql, params).fetchall()
        return [
            SearchResult(
                guid=str(row["guid"]),
                title=str(row["title"]),
                url=str(row["url"]),
                snippet=str(row["summary"] or row["content"])[:500],
                source=str(row["source"]),
                author=row["author"],
                published_at=datetime.fromisoformat(row["published_at"]) if row["published_at"] else None,
            )
            for row in rows
        ]

    def stats(self) -> dict[str, int]:
        with self._connect() as connection:
            feeds = int(connection.execute("SELECT count(*) FROM feeds").fetchone()[0])
            entries = int(connection.execute("SELECT count(*) FROM entries").fetchone()[0])
        return {"feeds": feeds, "entries": entries}
