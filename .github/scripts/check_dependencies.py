#!/usr/bin/env python3
"""
Lockfile check for the Tests workflow's install job. Run after `idf.py reconfigure` on a
clean checkout in the pinned ESP-IDF image:

  check_dependencies.py [--board tdeck|tab5] <reconfigure.log> <committed-lock> <committed-sdkconfig>
                        <generated-sdkconfig>

(reconfigure rewrites the lock and sdkconfig, so pass copies taken before it ran). The T-Deck
(default) uses dependencies.lock, sdkconfig and sdkconfig.defaults; the Tab5 uses
dependencies.lock.esp32p4, sdkconfig.tab5 and sdkconfig.defaults + sdkconfig.defaults.esp32p4.

Fails (exit 1) when
  - the component manager did not install exactly the versions in dependencies.lock
    (its "Processing N dependencies" pass, which also verifies component hashes);
  - the component manager ran a second time ("Missing kconfig option"): that re-run
    re-solves the installed components' dependencies from the registry and replaces locked
    versions with the newest releases (CMakeLists.txt seeds sdkconfig.json to prevent it);
  - reconfigure rewrote the lock file (it no longer matches main/idf_component.yml, so the
    component manager solved again: regenerate the lock and commit it);
  - a component the build compiles (managed_components/) differs from the lock;
  - a setting in sdkconfig.defaults does not take effect in the generated sdkconfig.
Reports, without failing, how far the committed sdkconfig is from the generated one.

None of these depend on what the registry publishes: a build compiles exactly the lock.
"""

import glob
import os
import re
import sys

# board: (its lock file, its sdkconfig defaults files in the order ESP-IDF applies them)
BOARDS = {
    "tdeck": ("dependencies.lock", ["sdkconfig.defaults"]),
    "tab5": ("dependencies.lock.esp32p4", ["sdkconfig.defaults", "sdkconfig.defaults.esp32p4"]),
}
RERUN = "Missing kconfig option"   # ESP-IDF re-runs the component manager after this


def error(message, file=None):
    where = f" file={file}" if file else ""
    print(f"::error{where}::{message}")


def warning(message, file=None):
    where = f" file={file}" if file else ""
    print(f"::warning{where}::{message}")


def lock_versions(text):
    """{name: version} for the registry components in dependencies.lock."""
    versions = {}
    current = None
    in_deps = False
    for line in text.splitlines():
        if line == "dependencies:":
            in_deps = True
            continue
        if in_deps and re.match(r"^\S", line):
            break
        m = re.match(r"^  ([\w./-]+):$", line)
        if m:
            current = m.group(1)
            continue
        m = re.match(r"^    version:\s*'?([^'\s]+)'?$", line)
        if m and current:
            versions[current] = m.group(1)
    versions.pop("idf", None)
    return versions


def first_install_pass(log):
    """{name: version} from the first 'Processing N dependencies' block of the log (a re-run,
    if any, prints a second one)."""
    block = re.search(r"Processing (\d+) dependencies:\s*\n((?:.*\n)*?)(?=.*(?:\[\1/\1\]).*\n)", log)
    if not block:
        return None
    count = int(block.group(1))
    lines = re.findall(r"\[(\d+)/%d\] ([\w./-]+) \(([^)]+)\)" % count, log)
    seen = {}
    for index, name, version in lines:
        if name in seen and int(index) == 1:
            break                         # The second pass starts again at [1/N]
        seen[name] = version
    seen.pop("idf", None)
    return seen


def compiled_versions(root):
    """{name: version} of the components in managed_components/ (what the build compiles)."""
    versions = {}
    for manifest in glob.glob(os.path.join(root, "managed_components", "*", "idf_component.yml")):
        folder = os.path.basename(os.path.dirname(manifest))
        name = folder.replace("__", "/", 1)
        with open(manifest, encoding="utf-8") as f:
            m = re.search(r"(?m)^version:\s*'?([^'\s]+)'?", f.read())
        if m:
            versions[name] = m.group(1)
    return versions


def read_sdkconfig(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line)
            if m:
                values[m.group(1)] = m.group(2)
                continue
            m = re.match(r"^# (CONFIG_[A-Z0-9_]+) is not set$", line)
            if m:
                values[m.group(1)] = "n"
    return values


def check(root, log, committed_lock, committed_cfg, generated_cfg, board="tdeck"):
    lock_name, defaults_files = BOARDS[board]
    problems = []
    with open(committed_lock, encoding="utf-8") as f:
        committed_lock_text = f.read()
    locked = lock_versions(committed_lock_text)

    if RERUN in log:
        problems.append(f"the component manager ran twice ('{RERUN}'), so the build may compile newer "
                        "components than the lock: make the first run see every $CONFIG{...} its "
                        "manifests use (CMakeLists.txt seeds build/config/sdkconfig.json from the sdkconfig)")
    with open(os.path.join(root, lock_name), encoding="utf-8") as f:
        if f.read() != committed_lock_text:
            problems.append(f"reconfigure rewrote {lock_name}: it does not match main/idf_component.yml. "
                            "Regenerate it in a clean copy (no build dir, no managed_components/) and commit it")

    installed = first_install_pass(log)
    if installed is None:
        problems.append("no 'Processing N dependencies' pass in the reconfigure log")
    elif installed != locked:
        for name in sorted(set(installed) | set(locked)):
            if installed.get(name) != locked.get(name):
                problems.append(f"{name}: installed {installed.get(name)} but {lock_name} has "
                                f"{locked.get(name)} (run idf.py update-dependencies and commit the lock)")

    compiled = compiled_versions(root)
    drift_rows = []
    for name in sorted(set(compiled) | set(locked)):
        lock_v, built_v = locked.get(name), compiled.get(name)
        if lock_v == built_v:
            continue
        drift_rows.append((name, lock_v, built_v))
        problems.append(f"{name}: the build compiles {built_v} but {lock_name} pins {lock_v}")

    defaults = {}   # Later files override earlier ones, as in ESP-IDF
    for name in defaults_files:
        with open(os.path.join(root, name), encoding="utf-8") as f:
            defaults.update(re.findall(r"(?m)^(CONFIG_\w+)=(.*)$", f.read()))
    generated = read_sdkconfig(generated_cfg)
    for option, value in defaults.items():
        if generated.get(option, "n") != value and option in generated:
            problems.append(f"{option}={generated[option]} in the generated sdkconfig; "
                            f"sdkconfig.defaults says {value} (update sdkconfig too)")

    committed = read_sdkconfig(committed_cfg)
    stale = sorted(k for k in set(committed) | set(generated) if committed.get(k) != generated.get(k))
    if stale:
        warning(f"committed sdkconfig differs from the generated one in {len(stale)} options "
                "(CI regenerates it; run idf.py reconfigure and commit sdkconfig to refresh it)", "sdkconfig")
    return problems, drift_rows, len(locked), len(stale)


def main():
    args = sys.argv[1:]
    board = "tdeck"
    if args[:1] == ["--board"] and len(args) > 1:
        board, args = args[1], args[2:]
    if len(args) != 4 or board not in BOARDS:
        print(__doc__)
        return 2
    with open(args[0], encoding="utf-8", errors="replace") as f:
        log = f.read()
    problems, drift, count, stale = check(os.getcwd(), log, args[1], args[2], args[3], board)
    for p in problems:
        error(p, BOARDS[board][0] if "lock" in p else None)

    lines = [f"## Dependencies ({board})", "",
             f"{count} locked components installed with verified hashes: "
             f"{'yes' if not any('installed' in p for p in problems) else '**no**'}.", ""]
    if drift:
        lines += ["| Component | Locked | Compiled |", "|---|---|---|"]
        lines += [f"| {n} | {a} | {b} |" for n, a, b in drift]
        lines.append("")
    lines.append(f"Committed sdkconfig vs generated: {stale} options differ (report only).")
    report = "\n".join(lines) + "\n"
    out = os.environ.get("GITHUB_STEP_SUMMARY")
    if out:
        with open(out, "a") as f:
            f.write(report)
    print(report)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
