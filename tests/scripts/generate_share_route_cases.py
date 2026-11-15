"""Build guest-denial cases from the documented, registered owner API routes."""
import argparse
import json
from pathlib import Path
import re


def cases(root: Path):
    routes = {}
    for source in (root / "src/apps/web").rglob("*.cpp"):
        text = source.read_text(encoding="utf-8")
        for match in re.finditer(r'CROW_ROUTE\(app,\s*"([^"\n]+)"\)', text):
            methods = re.match(r"\s*\.methods\(([^)]+)\)", text[match.end():])
            names = re.findall(r"HTTPMethod::(\w+)", methods[1]) if methods else ["GET"]
            routes.setdefault(match[1], set()).update(name.upper() for name in names)
    result = {}
    documentation = (root / "docs/daemon-api.md").read_text(encoding="utf-8")
    for method, path in re.findall(r"\b(GET|POST|PUT|DELETE|PATCH)\s+(/api/[^\s`|)]+)", documentation):
        path = path.split("?")[0].rstrip(".,;:[")
        if path.startswith("/api/share/v1/"):
            continue
        target = re.sub(r":\w+", "invite-test", path)
        for route, methods in routes.items():
            pattern = re.escape(route).replace("<string>", "[^/]+").replace("<int>", "[0-9]+").replace("<uint>", "[0-9]+").replace("<path>", ".+")
            if method in methods and re.fullmatch(pattern, target):
                result[method, target] = {"method": method, "path": target}
                break
        else:
            raise ValueError(f"Documented route has no matching registration: {method} {path}")
    return list(result.values())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    target = root / "tests/fixtures/share_protected_routes.json"
    rendered = json.dumps(cases(root), indent=2) + "\n"
    if args.check:
        if target.read_text(encoding="utf-8") != rendered:
            raise SystemExit("Guest authorization cases are stale; regenerate them.")
    else:
        target.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"Verified {len(json.loads(rendered))} documented owner routes")
