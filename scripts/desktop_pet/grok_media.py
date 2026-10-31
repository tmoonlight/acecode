"""Run one Grok Build CLI media tool call (image_edit / reference_to_video) headlessly and return the saved file.

The CLI is told to make exactly one call with the given JSON arguments; the saved file's absolute path is
read back from the session's tool output.
"""

import json
import pathlib
import re
import subprocess

GROK = str(pathlib.Path.home() / ".grok" / "bin" / "grok.exe")


def run(tool, arguments, workdir, log_path, timeout=1500):
    """Returns the absolute path of the image / video the tool saved, or None."""
    prompt = (
        f"Call the `{tool}` tool exactly once with exactly these JSON arguments and nothing else:\n"
        f"{json.dumps(arguments, ensure_ascii=False)}\n"
        "Do not call any other tool, do not retry, and do not modify the arguments. "
        "After the call, reply with only the absolute path the tool returned on a line starting with RESULT: "
        "(or RESULT: FAILED and the error)."
    )
    workdir = pathlib.Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    try:
        completed = subprocess.run([GROK, "-p", prompt, "--always-approve", "--output-format", "plain",
                                    "--disable-web-search", "--no-subagents", "--max-turns", "4"],
                                   cwd=str(workdir), capture_output=True, text=True, encoding="utf-8",
                                   errors="replace", timeout=timeout, stdin=subprocess.DEVNULL)
        output = completed.stdout + completed.stderr
    except subprocess.TimeoutExpired as error:
        output = f"TIMEOUT {error}"
    pathlib.Path(log_path).write_text(output, encoding="utf-8")
    match = re.search(r"RESULT:\s*([A-Za-z]:[\\/][^\r\n]+?\.(?:png|jpe?g|webp|mp4|webm|mov))", output)
    if not match:
        return None
    path = pathlib.Path(match[1].strip().strip("`'\""))
    return path if path.exists() else None
