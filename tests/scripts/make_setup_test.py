#!/usr/bin/env python3
"""installer/windows/make_setup.py 的单测。

场景:
  1. 条目名安全检查与 C++ 侧 is_safe_archive_entry 同规则(穿越、盘符、冒号、设备名、末尾点号)。
  2. zip_directory 产出正斜杠条目、按路径排序、时间戳固定,两次打包字节一致(可复现)。
  3. validate_payload 要求根目录有 acecode.exe,并拒绝不安全条目。
  4. payload_info 的哈希、大小与输入一致,版本 / 目标平台非法时报错。
  5. (仅 Windows)把资源嵌进一个真实 PE 的副本后,能按安装程序同样的方式读回大小。
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "installer" / "windows" / "make_setup.py"


def load_module():
    spec = importlib.util.spec_from_file_location("make_setup_under_test", SCRIPT_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


make_setup = load_module()


def zip_of(entries: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return buffer.getvalue()


class EntrySafetyTest(unittest.TestCase):
    # 场景:与 tests/installer/payload_archive_test.cpp 的 EntryNameSafety 同一组名字。
    # 期望:两边判定一致,打包阶段就能挡下安装程序会拒绝的包。
    def test_matches_cpp_rules(self):
        for name in ("acecode.exe", "share/acecode/models_dev/api.json",
                     "share\\acecode\\seed\\x.md", "channels/"):
            self.assertTrue(make_setup.is_safe_entry(name), name)
        for name in ("/etc/passwd", "C:/Windows/x.dll", "acecode.exe:evil", "a/../../b",
                     "CON", "logs/com1.txt", "trailing.", "trailing ", ""):
            self.assertFalse(make_setup.is_safe_entry(name), name)


class ZipDirectoryTest(unittest.TestCase):
    # 场景:暂存目录里有根目录 exe 与嵌套资源文件,打包两次。
    # 期望:条目名用正斜杠且有序;两次结果逐字节相同(时间戳固定),解开后内容一致。
    def test_deterministic_forward_slash_zip(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "share" / "acecode").mkdir(parents=True)
            (root / "acecode.exe").write_bytes(b"MZ-cli")
            (root / "share" / "acecode" / "seed.json").write_bytes(b"{}")
            first = make_setup.zip_directory(root)
            second = make_setup.zip_directory(root)
            self.assertEqual(first, second)
            with zipfile.ZipFile(io.BytesIO(first)) as archive:
                self.assertEqual(archive.namelist(), ["acecode.exe", "share/acecode/seed.json"])
                self.assertEqual(archive.read("acecode.exe"), b"MZ-cli")


class ValidatePayloadTest(unittest.TestCase):
    # 场景:缺 acecode.exe 的包,以及混入 ../ 条目的包。
    # 期望:两者都在打包时报错,而不是产出一个装不上的安装程序。
    def test_requires_cli_and_safe_names(self):
        make_setup.validate_payload(zip_of({"acecode.exe": b"x", "share/a.txt": b"y"}))
        with self.assertRaises(ValueError):
            make_setup.validate_payload(zip_of({"bin/acecode.exe": b"x"}))
        with self.assertRaises(ValueError):
            make_setup.validate_payload(zip_of({"acecode.exe": b"x", "../evil": b"y"}))


class PayloadInfoTest(unittest.TestCase):
    # 场景:用固定字节生成描述;再给出非法版本与目标平台。
    # 期望:哈希与 hashlib 一致、大小正确、schema 为 1;非法输入抛 ValueError。
    def test_hashes_and_validation(self):
        info = make_setup.payload_info("1.0.20", "windows-arm64", b"payload", b"stub")
        self.assertEqual(info["schema"], 1)
        self.assertEqual(info["payload_size"], 7)
        self.assertEqual(info["payload_sha256"], hashlib.sha256(b"payload").hexdigest())
        self.assertEqual(info["uninstaller_sha256"], hashlib.sha256(b"stub").hexdigest())
        with self.assertRaises(ValueError):
            make_setup.payload_info("1.0.20-beta", "windows-x64", b"", b"")
        with self.assertRaises(ValueError):
            make_setup.payload_info("1.0.20", "linux-x64", b"", b"")


@unittest.skipUnless(os.name == "nt", "UpdateResource is a Windows API")
class EmbedTest(unittest.TestCase):
    # 场景:把当前 python.exe 复制一份当作「安装程序骨架」,嵌入安装包。
    # 期望:产物里三个资源都能用 LoadLibraryEx + FindResource 找到,大小与输入一致,
    # 描述 JSON 的哈希对应嵌入的字节;临时文件不残留。
    def test_embeds_resources_readable_by_loader(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stub = root / "stub.exe"
            shutil.copyfile(sys.executable, stub)
            payload = zip_of({"acecode.exe": b"MZ" + b"x" * 4096})
            out = root / "out" / "setup.exe"
            info = make_setup.build_setup(stub, payload, "1.0.20", "windows-x64", out)
            self.assertTrue(out.exists())
            self.assertFalse(out.with_name(out.name + ".tmp").exists())
            sizes = make_setup.resource_sizes(out, [make_setup.PAYLOAD_RESOURCE,
                                                    make_setup.UNINSTALLER_RESOURCE,
                                                    make_setup.INFO_RESOURCE])
            self.assertEqual(sizes[make_setup.PAYLOAD_RESOURCE], len(payload))
            self.assertEqual(sizes[make_setup.UNINSTALLER_RESOURCE], stub.stat().st_size)
            self.assertGreater(sizes[make_setup.INFO_RESOURCE], 0)
            self.assertEqual(info["payload_sha256"], hashlib.sha256(payload).hexdigest())
            json.dumps(info)


if __name__ == "__main__":
    unittest.main()
