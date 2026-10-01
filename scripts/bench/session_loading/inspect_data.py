"""Independently verify a synthetic fixture's counts and byte composition."""
import argparse
from collections import Counter
import json
from pathlib import Path
from generate import category

def inspect(profile):
    manifest = json.loads((profile / "manifest.json").read_text(encoding="utf-8"))
    project = profile / ".acecode/projects" / manifest["project_hash"]
    results = []
    for expected in manifest["sessions"]:
        counts, sizes = Counter(), Counter()
        with (project / (expected["id"] + ".jsonl")).open("rb") as source:
            for line in source:
                record = json.loads(line)
                key = category(record)
                counts[key] += 1
                sizes[key] += len(line)
                if key == "compact_checkpoint":
                    assert len(json.dumps(record["metadata"]["replacement_history"])) < 80000
        assert dict(counts) == expected["records"], expected["id"]
        assert dict(sizes) == expected["bytes_by_type"], expected["id"]
        assert sum(sizes.values()) == expected["bytes"], expected["id"]
        results.append({"id": expected["id"], "bytes": sum(sizes.values()), "records": sum(counts.values()),
                        "composition": {key: {"bytes": size, "fraction": size / sum(sizes.values())} for key, size in sizes.items()}})
    assert len(list(project.glob("*.meta.json"))) == len(results)
    return {"session_count": len(results), "total_bytes": sum(row["bytes"] for row in results),
            "largest": sorted(results, key=lambda row: row["bytes"], reverse=True)[:5]}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    print(json.dumps(inspect(parser.parse_args().profile), indent=2))
