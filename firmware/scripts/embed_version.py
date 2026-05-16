"""Pre-build hook: stamp the current short git SHA + commit date into the
firmware as CLAWD_BUILD_SHA / CLAWD_BUILD_DATE preprocessor macros. The
updater service reads them to decide whether the published release on GitHub
is newer than what's running, and to show 'installed on …' in Settings."""

import subprocess

Import("env")  # noqa: F821 — provided by PlatformIO


def git(args: list[str]) -> str:
    try:
        return subprocess.check_output(
            ["git", *args],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return ""


sha  = git(["rev-parse", "--short=7", "HEAD"]) or "unknown"
# %cI = committer date in strict ISO 8601, e.g. 2026-05-16T14:32:11+00:00.
# Truncate to date for display; the updater compares SHAs, not dates.
date = (git(["show", "-s", "--format=%cI", "HEAD"]) or "")[:10]

env.Append(CPPDEFINES=[  # noqa: F821
    ("CLAWD_BUILD_SHA",  env.StringifyMacro(sha)),    # noqa: F821
    ("CLAWD_BUILD_DATE", env.StringifyMacro(date)),   # noqa: F821
])
print(f"clawdputer build sha: {sha} ({date or 'no date'})")
