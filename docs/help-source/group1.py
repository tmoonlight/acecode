from pathlib import Path
import json

PAGES = json.loads((Path(__file__).parent / "getting-started.json").read_text(encoding="utf-8"))
