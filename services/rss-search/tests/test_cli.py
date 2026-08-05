from rss_search_service.cli import load_feed_urls


def test_load_feed_urls_ignores_comments_blanks_and_duplicates(tmp_path):
    feed_file = tmp_path / "feeds.txt"
    feed_file.write_text(
        """
# curated feeds
https://github.blog/feed/

https://kubernetes.io/feed.xml
https://github.blog/feed/  # duplicate
""",
        encoding="utf-8",
    )

    assert load_feed_urls(feed_file) == [
        "https://github.blog/feed/",
        "https://kubernetes.io/feed.xml",
    ]
