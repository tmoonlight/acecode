from datetime import UTC, datetime

from fastapi.testclient import TestClient

from rss_search_service.api.app import create_app
from rss_search_service.domain.models import Entry


def test_health_and_search_endpoints(tmp_path):
    app = create_app(tmp_path / "api.db")
    with TestClient(app) as client:
        db = app.state.database
        feed_id = db.upsert_feed("https://example.com/feed", "Example", "https://example.com")
        db.upsert_entries(
            feed_id,
            [
                Entry(
                    guid="api-1",
                    title="Kubernetes release",
                    url="https://example.com/kubernetes",
                    summary="Cluster update",
                    content="Cluster update",
                    author=None,
                    published_at=datetime.now(UTC),
                )
            ],
        )

        health = client.get("/health")
        assert health.status_code == 200
        assert health.json()["status"] == "ok"
        assert health.json()["entries"] == 1

        response = client.get("/v1/search", params={"q": "Kubernetes", "limit": 5})
        assert response.status_code == 200
        payload = response.json()
        assert payload["query"] == "Kubernetes"
        assert payload["backend"] == "rss"
        assert payload["results"][0]["title"] == "Kubernetes release"


def test_search_validates_query(tmp_path):
    with TestClient(create_app(tmp_path / "api.db")) as client:
        assert client.get("/v1/search", params={"q": ""}).status_code == 422
        assert client.get("/v1/search", params={"q": "x", "limit": 101}).status_code == 422
