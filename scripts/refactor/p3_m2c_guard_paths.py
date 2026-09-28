"""P3 M2c(内容提交):ChannelBoundaryGuard 测试硬编码的扫描路径改到分组布局。用法:python p3_m2c_guard_paths.py <repo>"""
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
p = root / "tests/remote_control/channel_boundary_guard_test.cpp"
raw = p.read_bytes(); nl = b"\r\n" if b"\r\n" in raw else b"\n"; s = raw.decode("utf-8").replace("\r\n", "\n")
pairs = [
    ('root / "src" / "remote_control",', 'root / "src" / "host" / "remote_control",'),
    ('root / "src" / "tui" / "commands" / "remote_control_command.cpp",', 'root / "src" / "apps" / "tui" / "commands" / "remote_control_command.cpp",'),
    ('root / "src" / "tui" / "commands" / "remote_control_command.hpp",', 'root / "src" / "apps" / "tui" / "commands" / "remote_control_command.hpp",'),
    ('root / "src" / "config" / "config.cpp",', 'root / "src" / "base" / "config" / "config.cpp",'),
    ('root / "src" / "config" / "config.hpp",', 'root / "src" / "base" / "config" / "config.hpp",'),
]
for old, new in pairs:
    assert s.count(old) == 1, old
    s = s.replace(old, new)
p.write_bytes(s.replace("\n", nl.decode()).encode("utf-8"))
print("guard scan roots -> final layout (5 lines)")
