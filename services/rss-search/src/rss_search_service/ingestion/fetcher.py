"""Safe HTTP feed fetching with SSRF and response-size protection."""

from __future__ import annotations

import ipaddress
import socket
from dataclasses import dataclass
from urllib.parse import urljoin, urlsplit

import httpx


class UnsafeFeedUrl(ValueError):
    pass


class FeedTooLarge(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class FetchResult:
    url: str
    content: bytes
    content_type: str | None
    etag: str | None
    last_modified: str | None
    not_modified: bool = False


def validate_feed_url(url: str) -> str:
    parsed = urlsplit(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise UnsafeFeedUrl("feed URL must use http or https")
    if parsed.username or parsed.password:
        raise UnsafeFeedUrl("feed URL credentials are not allowed")
    try:
        addresses = socket.getaddrinfo(parsed.hostname, parsed.port or (443 if parsed.scheme == "https" else 80))
    except socket.gaierror as exc:
        raise UnsafeFeedUrl("feed hostname cannot be resolved") from exc
    for address in addresses:
        ip = ipaddress.ip_address(address[4][0])
        if not ip.is_global:
            raise UnsafeFeedUrl(f"feed resolves to a non-public address: {ip}")
    return url


async def fetch_feed(
    url: str,
    *,
    etag: str | None = None,
    last_modified: str | None = None,
    timeout_seconds: float = 10.0,
    max_bytes: int = 5_000_000,
    max_redirects: int = 5,
) -> FetchResult:
    headers = {"User-Agent": "AceCode-RSS-Search/0.1 (+https://github.com/tmoonlight/acecode)"}
    if etag:
        headers["If-None-Match"] = etag
    if last_modified:
        headers["If-Modified-Since"] = last_modified

    current_url = url
    async with httpx.AsyncClient(timeout=timeout_seconds, follow_redirects=False) as client:
        for _ in range(max_redirects + 1):
            validate_feed_url(current_url)
            async with client.stream("GET", current_url, headers=headers) as response:
                if response.is_redirect:
                    location = response.headers.get("location")
                    if not location:
                        response.raise_for_status()
                    current_url = urljoin(current_url, location)
                    continue
                if response.status_code == 304:
                    return FetchResult(current_url, b"", response.headers.get("content-type"), etag, last_modified, True)
                response.raise_for_status()
                declared_size = response.headers.get("content-length")
                if declared_size and int(declared_size) > max_bytes:
                    raise FeedTooLarge(f"feed exceeds {max_bytes} bytes")
                chunks: list[bytes] = []
                size = 0
                async for chunk in response.aiter_bytes():
                    size += len(chunk)
                    if size > max_bytes:
                        raise FeedTooLarge(f"feed exceeds {max_bytes} bytes")
                    chunks.append(chunk)
                return FetchResult(
                    url=current_url,
                    content=b"".join(chunks),
                    content_type=response.headers.get("content-type"),
                    etag=response.headers.get("etag"),
                    last_modified=response.headers.get("last-modified"),
                )
    raise httpx.TooManyRedirects("too many feed redirects")
