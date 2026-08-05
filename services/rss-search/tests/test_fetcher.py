import pytest

from rss_search_service.ingestion.fetcher import UnsafeFeedUrl, validate_feed_url


@pytest.mark.parametrize(
    "url",
    [
        "http://127.0.0.1/feed",
        "http://localhost/feed",
        "http://169.254.169.254/latest/meta-data",
        "file:///etc/passwd",
    ],
)
def test_private_or_non_http_feed_urls_are_rejected(url):
    with pytest.raises(UnsafeFeedUrl):
        validate_feed_url(url)


def test_public_https_feed_url_is_accepted(monkeypatch):
    monkeypatch.setattr(
        "rss_search_service.ingestion.fetcher.socket.getaddrinfo",
        lambda *args, **kwargs: [(None, None, None, None, ("93.184.216.34", 443))],
    )
    assert validate_feed_url("https://example.com/feed") == "https://example.com/feed"
