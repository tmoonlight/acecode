from datetime import UTC, datetime

import pytest

from rss_search_service.ingestion.parser import FeedParseError, parse_feed

RSS = b"""<?xml version="1.0"?>
<rss version="2.0"><channel><title>Example RSS</title><link>https://example.com</link>
<item><guid>rss-1</guid><title>Claude release</title><link>https://example.com/claude</link>
<description><![CDATA[<p>New agent features</p>]]></description>
<pubDate>Fri, 01 Aug 2025 10:00:00 GMT</pubDate></item>
</channel></rss>"""

ATOM = b"""<?xml version="1.0"?>
<feed xmlns="http://www.w3.org/2005/Atom"><title>Example Atom</title>
<link href="https://example.org"/><entry><id>atom-1</id><title>PostgreSQL update</title>
<link href="https://example.org/postgres"/><summary>Database release</summary>
<updated>2025-08-02T12:00:00Z</updated></entry></feed>"""

JSON_FEED = b'''{
  "version": "https://jsonfeed.org/version/1.1",
  "title": "Example JSON",
  "home_page_url": "https://json.example",
  "items": [{
    "id": "json-1",
    "url": "https://json.example/ai",
    "title": "AI weekly",
    "content_html": "<p>Agent research</p>",
    "date_published": "2025-08-03T09:30:00+00:00",
    "authors": [{"name": "Ada"}]
  }]
}'''


def test_parse_rss_to_normalized_document():
    document = parse_feed(RSS, "application/rss+xml", "https://example.com/feed.xml")

    assert document.title == "Example RSS"
    assert document.site_url == "https://example.com"
    assert len(document.entries) == 1
    entry = document.entries[0]
    assert entry.guid == "rss-1"
    assert entry.title == "Claude release"
    assert entry.summary == "New agent features"
    assert entry.published_at == datetime(2025, 8, 1, 10, 0, tzinfo=UTC)


def test_parse_atom_to_normalized_document():
    document = parse_feed(ATOM, "application/atom+xml", "https://example.org/feed.atom")

    assert document.title == "Example Atom"
    assert document.entries[0].url == "https://example.org/postgres"
    assert document.entries[0].guid == "atom-1"


def test_parse_json_feed_to_normalized_document():
    document = parse_feed(JSON_FEED, "application/feed+json", "https://json.example/feed.json")

    entry = document.entries[0]
    assert document.title == "Example JSON"
    assert entry.author == "Ada"
    assert entry.content == "Agent research"
    assert entry.published_at == datetime(2025, 8, 3, 9, 30, tzinfo=UTC)


def test_invalid_feed_is_rejected():
    with pytest.raises(FeedParseError):
        parse_feed(b"not a feed", "text/plain", "https://example.com/feed")
