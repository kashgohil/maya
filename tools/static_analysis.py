#!/usr/bin/env python3
"""Runs Clang's static analyzer on the engine's and the applications' sources (#1069, docs/acceptance.md).

Every src/ and apps/ source in the build's compile_commands.json is analyzed with its own compile command,
in parallel. A warning in the repository's own files fails the run; warnings from the SDK's or the
dependencies' headers are not the engine's and are left out. A source that cannot be analyzed fails it too.

Usage: tools/static_analysis.py [build directory] [log file]    (default: build; the log is optional)
Any warning counts, the analyzer's or the compiler's. Exits 0 when clean, 1 with warnings or sources that could
not be analyzed, and 2 when nothing was analyzed (no compile database, or none of this checkout's sources in it).
MAYA_ANALYSIS_JOBS sets how many sources are analyzed at once (default: every core).
"""

import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

source = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
build = os.path.realpath(sys.argv[1] if len(sys.argv) > 1 else "build")
log_path = sys.argv[2] if len(sys.argv) > 2 else None
database = os.path.join(build, "compile_commands.json")


def unavailable(why):
    """Exit 2: nothing was analyzed, which is never a pass."""
    print(why)
    if log_path:
        with open(log_path, "w") as log:
            log.write(why + "\n")
    sys.exit(2)


if not os.path.isfile(database):
    unavailable(f"no compile database in {build}: configure it with cmake (CMAKE_EXPORT_COMPILE_COMMANDS is on)")

roots = tuple(os.path.join(source, folder) + os.sep for folder in ("src", "apps"))
with open(database) as file:
    entries = [e for e in json.load(file) if os.path.realpath(e["file"]).startswith(roots)]
# A source built into several targets is analyzed once.
entries = list({os.path.realpath(e["file"]): e for e in entries}.values())
entries.sort(key=lambda e: e["file"])
if not entries:
    unavailable(f"no src/ or apps/ sources of {source} in {database}: is it this checkout's build?")

# A warning is the repository's own when its file is in the checkout, but not in a build tree inside it.
warning = re.compile(r"^(?P<file>/[^:]+):\d+:\d+: warning: ")


def ours(path):
    path = os.path.realpath(path)
    return path.startswith(source + os.sep) and not path.startswith(build + os.sep) and "/_deps/" not in path


def analyze(entry):
    """The analyzer's output for one source: its warnings in our files, and whether it ran."""
    arguments = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    command, skip = [], False
    for argument in arguments:  # the compile command without its object file
        if skip:
            skip = False
        elif argument == "-o":
            skip = True
        elif argument != "-c":
            command.append(argument)
    command += ["--analyze", "-Xanalyzer", "-analyzer-output=text", "-o", "/dev/null"]
    run = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
    found = [line for line in run.stderr.splitlines() if (m := warning.match(line)) and ours(m.group("file"))]
    return entry["file"], run.returncode, found, run.stderr


jobs = int(os.environ.get("MAYA_ANALYSIS_JOBS", os.cpu_count() or 4))
results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
    for result in pool.map(analyze, entries):
        results.append(result)

lines, warnings, failed = [], 0, 0
for file, code, found, output in results:
    shown = os.path.relpath(file, source)
    if code != 0:
        failed += 1
        lines.append(f"NOT ANALYZED {shown} (exit {code})")
        lines += ["    " + line for line in output.splitlines()[:20]]
    elif found:
        warnings += len(found)
        lines.append(f"WARNINGS     {shown}: {len(found)}")
        lines += ["    " + line for line in output.splitlines()]  # with the analyzer's notes, the path it took
summary = f"Static analysis: {len(results)} sources, {warnings} warnings" + (f", {failed} not analyzed" if failed else "")
text = "\n".join(lines + [summary]) + "\n"
print(text, end="")
if log_path:
    with open(log_path, "w") as log:
        log.write(text)
sys.exit(1 if warnings or failed else 0)
