"""Command-line administration for feed ingestion."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path

from rss_search_service.application.ingest_feed import ingest_feed
from rss_search_service.infrastructure.database import Database


def load_feed_urls(path: str | Path) -> list[str]:
    seen: set[str] = set()
    urls: list[str] = []
    for raw_line in Path(path).read_text(encoding="utf-8").splitlines():
        url = raw_line.split("#", 1)[0].strip()
        if url and url not in seen:
            seen.add(url)
            urls.append(url)
    return urls


async def _ingest_all(database: Database, urls: list[str]) -> int:
    failures = 0
    for url in urls:
        try:
            result = await ingest_feed(database, url)
            print(
                json.dumps(
                    {
                        "status": "ok",
                        "url": url,
                        "title": result.title,
                        "entries_written": result.entries_written,
                    },
                    ensure_ascii=False,
                )
            )
        except Exception as exc:  # noqa: BLE001 - one bad feed must not stop the batch
            failures += 1
            print(json.dumps({"status": "error", "url": url, "error": str(exc)}, ensure_ascii=False))
    return 1 if failures == len(urls) and urls else 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="rss-search")
    subparsers = parser.add_subparsers(dest="command", required=True)
    ingest_parser = subparsers.add_parser("ingest", help="fetch and index configured feeds")
    ingest_parser.add_argument("--feeds-file", required=True, type=Path)
    args = parser.parse_args()

    database = Database(os.environ.get("RSS_SEARCH_DB_PATH", "rss-search.db"))
    database.initialize()
    if args.command == "ingest":
        return asyncio.run(_ingest_all(database, load_feed_urls(args.feeds_file)))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
