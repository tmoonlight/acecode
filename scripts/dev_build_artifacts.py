#!/usr/bin/env python3
"""Shared development-build artifact discovery."""

from __future__ import annotations

import os
from pathlib import Path


def candidate_build_directories(build_dir: Path, max_depth: int = 2) -> list[Path]:
    if not build_dir.is_dir():
        return []
    directories = [build_dir]
    frontier = [build_dir]
    for _ in range(max_depth):
        next_frontier: list[Path] = []
        for parent in frontier:
            try:
                children = sorted(
                    (path for path in parent.iterdir() if path.is_dir() and path.suffix != ".app"),
                    key=lambda path: str(path).lower(),
                )
            except OSError:
                continue
            directories.extend(children)
            next_frontier.extend(children)
        frontier = next_frontier
    return directories


def find_named_artifacts(
    build_dir: Path,
    names: list[str],
    app_bundle: str | None = None,
    require_executable: bool = True,
) -> list[Path]:
    results: list[Path] = []
    seen: set[Path] = set()
    for directory in candidate_build_directories(build_dir):
        candidates = [directory / name for name in names]
        if app_bundle:
            candidates.insert(0, directory / app_bundle)
        for candidate in candidates:
            if not candidate.is_file() and not (app_bundle and candidate.name == app_bundle and candidate.is_dir()):
                continue
            if (
                require_executable
                and candidate.is_file()
                and os.name != "nt"
                and not os.access(candidate, os.X_OK)
            ):
                continue
            resolved = candidate.resolve()
            if resolved not in seen:
                seen.add(resolved)
                results.append(candidate)
    return results
