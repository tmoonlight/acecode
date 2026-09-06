"""Map stable documentation figures to unaltered Computer Use captures."""
from hashlib import sha256
from html import escape
from pathlib import Path
import json
import re

SOURCE = Path(__file__).resolve().parent
DEST = SOURCE.parent / "help"


class Screenshots:
    def __init__(self):
        self.mapping = json.loads((SOURCE / "screenshot-map.json").read_text(encoding="utf-8"))
        self.pending = json.loads((SOURCE / "pending-captures.json").read_text(encoding="utf-8"))
        assert not set(self.mapping) & set(self.pending)
        records = json.loads((SOURCE / "captures.json").read_text(encoding="utf-8"))
        self.captures = {}
        for record in records:
            path = (DEST / record["file"]).resolve()
            assert path.is_relative_to((DEST / "assets/screenshots").resolve()), path
            assert path.suffix in (".jpg", ".png") and path.is_file(), path
            assert record["width"] > 0 and record["height"] > 0
            assert record["method"].startswith(("Computer Use", "Manual")), path
            # 用户手动补拍的图可以带红框/箭头等说明性标注；此时必须写明标注内容，图注下方也会提示读者。
            assert record["unaltered"] or record.get("annotations"), path
            self.captures[path.name] = {**record, "sha256": sha256(path.read_bytes()).hexdigest()}
        self.used = set()
        self.unfilled = set()

    def render(self, body, figures):
        for figure in figures:
            identifier = figure["id"]
            entry = self.mapping.get(identifier)
            if not entry:
                assert identifier in self.pending, identifier
                figure["status"] = "placeholder"
                figure["pending"] = self.pending[identifier]
                self.unfilled.add(identifier)
                continue
            self.used.add(identifier)
            links = []
            assets = []
            for name in entry["images"]:
                capture = self.captures[name]
                src = escape(capture["file"], quote=True)
                alt = escape(capture["description"], quote=True)
                links.append(f'<a class="screenshot-link" href="{src}" aria-label="查看原图：{alt}"><img src="{src}" width="{capture["width"]}" height="{capture["height"]}" alt="{alt}" loading="lazy" decoding="async"></a>')
                assets.append(capture)
            assert links, identifier
            caption = entry["caption"]
            annotated = any(capture.get("annotations") for capture in assets)
            hint = "Windows 实际界面 · " + ("红色标注为文档说明添加 · " if annotated else "") + "点击图片查看原图"
            replacement = f'<figure class="screenshot" data-figure="{identifier}" data-image-status="captured">' + "\n".join(links) + f'<figcaption>{escape(caption)}<span class="screenshot-hint">{hint}</span></figcaption></figure>'
            pattern = r'<figure\b[^>]*data-placeholder="' + re.escape(identifier) + r'"[^>]*>.*?</figure>'
            body, count = re.subn(pattern, lambda _: replacement, body, flags=re.S)
            assert count == 1, (identifier, count)
            figure.update(status="captured", title=entry["title"], description=caption, captures=assets)
            if entry.get("pending_details"):
                figure["pending_details"] = entry["pending_details"]
        return body

    def validate(self):
        assert self.used == set(self.mapping), set(self.mapping) - self.used
        assert self.unfilled == set(self.pending), set(self.pending) - self.unfilled
        assert {name for item in self.mapping.values() for name in item["images"]} == set(self.captures)
