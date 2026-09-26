#!/usr/bin/env python3
"""Prepare an observational startup executable without editing production files."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


SNAPSHOT = r'''
    // P0-12 observation only: original startup has run, event routing has not.
    // This block exists only in the generated probe, never in a shipped target.
    {
        const auto snapshot_path = acecode::getenv_utf8("ACECODE_P012_SNAPSHOT_PATH");
        const auto scenario = acecode::getenv_utf8("ACECODE_P012_SCENARIO");
        if (snapshot_path.empty() || scenario.empty()) std::_Exit(90);
        nlohmann::json snapshot;
        {
            std::lock_guard<std::mutex> lock(state.mu);
            snapshot["schema_version"] = 1;
            snapshot["scenario"] = scenario;
            snapshot["checkpoint"] = "startup-wiring-complete-before-CatchEvent";
            snapshot["limit"] = 16;
            snapshot["total_messages"] = state.conversation.size();
            snapshot["messages"] = nlohmann::json::array();
            for (size_t i = 0; i < state.conversation.size() && i < 16; ++i) {
                const auto& message = state.conversation[i];
                // These fixtures contain text only. Fail instead of losing an
                // unexpected structured summary or diff in serialization.
                if (message.summary.has_value() || message.hunks.has_value()) {
                    std::_Exit(91);
                }
                snapshot["messages"].push_back({
                    {"role", message.role},
                    {"content", message.content},
                    {"is_tool", message.is_tool},
                    {"summary", nullptr},
                    {"expanded", message.expanded},
                    {"display_override", message.display_override},
                    {"hunks", nullptr},
                    {"compact_notice_id", message.compact_notice_id},
                    {"compact_notice_complete", message.compact_notice_complete},
                    {"ask_result", message.ask_result},
                });
            }
        }
        std::ofstream output(acecode::path_from_utf8(snapshot_path), std::ios::binary);
        output << snapshot.dump(2) << '\n';
        output.close();
        if (!output) std::_Exit(92);
        // Deliberately stop the measurement here, without claiming to exercise
        // shutdown. The fixture runner owns this disposable process and data.
        std::_Exit(0);
    }

'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--ref", default="3ddb7d43")
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    revision = subprocess.check_output(
        ["git", "rev-parse", args.ref + "^{commit}"], cwd=repo, text=True
    ).strip()
    source = subprocess.check_output(
        ["git", "show", revision + ":src/main.cpp"], cwd=repo
    )
    if (repo / "src/main.cpp").read_bytes() != source:
        raise SystemExit("The worktree main.cpp must match the selected source ref")
    marker = b"    auto input_with_esc = CatchEvent("
    if source.count(marker) != 1:
        raise SystemExit("Expected exactly one original CatchEvent assembly marker")
    out.mkdir(parents=True, exist_ok=True)
    instrumented = b"#include <fstream>\n#include <cstdlib>\n" + source.replace(
        marker, SNAPSHOT.encode("utf-8") + marker, 1
    )
    probe = out / "startup_main_probe.cpp"
    probe.write_bytes(instrumented)
    cmake = f'''# P0-12 generated probe: no changes to the source checkout.
function(acecode_p012_install_probe)
  if(NOT TARGET acecode)
    message(FATAL_ERROR "P0-12 requires the original acecode target")
  endif()
  get_target_property(p012_sources acecode SOURCES)
  set(p012_original "{repo.as_posix()}/src/main.cpp")
  if(NOT p012_original IN_LIST p012_sources)
    message(FATAL_ERROR "P0-12 main.cpp is absent from acecode sources")
  endif()
  list(REMOVE_ITEM p012_sources "${{p012_original}}")
  list(APPEND p012_sources "{probe.as_posix()}")
  set_property(TARGET acecode PROPERTY SOURCES "${{p012_sources}}")
endfunction()
cmake_language(DEFER CALL acecode_p012_install_probe)
'''
    (out / "probe.cmake").write_text(cmake, encoding="utf-8", newline="\n")
    metadata = {
        "source_revision": revision,
        "source_path": "src/main.cpp",
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "generated_probe_sha256": hashlib.sha256(instrumented).hexdigest(),
        "original_checkpoint_line": source[:source.index(marker)].count(b"\n") + 1,
        "production_file_unchanged": (repo / "src/main.cpp").read_bytes() == source,
    }
    (out / "probe-source.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
