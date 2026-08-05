from datetime import UTC, datetime, timedelta

from rss_search_service.domain.models import Entry
from rss_search_service.infrastructure.database import Database


def make_entry(guid: str, title: str, content: str, published_at: datetime) -> Entry:
    return Entry(
        guid=guid,
        title=title,
        url=f"https://example.com/{guid}",
        summary=content,
        content=content,
        author="Test",
        published_at=published_at,
    )


def test_upsert_is_idempotent_and_searches_english(tmp_path):
    db = Database(tmp_path / "search.db")
    db.initialize()
    feed_id = db.upsert_feed("https://example.com/feed", "Example", "https://example.com")
    entry = make_entry("one", "Claude Code release", "New agent capabilities", datetime.now(UTC))

    db.upsert_entries(feed_id, [entry])
    db.upsert_entries(feed_id, [entry])

    results = db.search("Claude", limit=10)
    assert len(results) == 1
    assert results[0].title == "Claude Code release"
    assert db.stats()["entries"] == 1


def test_search_supports_short_chinese_and_since_filter(tmp_path):
    db = Database(tmp_path / "search.db")
    db.initialize()
    feed_id = db.upsert_feed("https://example.com/feed", "中文源", "https://example.com")
    now = datetime.now(UTC)
    db.upsert_entries(
        feed_id,
        [
            make_entry("new", "人工智能新进展", "模型能力更新", now),
            make_entry("old", "人工智能历史", "旧模型资料", now - timedelta(days=90)),
        ],
    )

    results = db.search("模型", limit=10, since_days=30)
    assert [result.guid for result in results] == ["new"]
