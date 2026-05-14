"""Pre-build hook: stamp the current short git SHA into the firmware as a
CLAWD_BUILD_SHA preprocessor macro. The updater service reads it to decide
whether the published release on GitHub is newer than what's running."""

import subprocess

Import("env")  # noqa: F821 — provided by PlatformIO


def get_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short=7", "HEAD"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return "unknown"


sha = get_sha()
env.Append(CPPDEFINES=[("CLAWD_BUILD_SHA", env.StringifyMacro(sha))])  # noqa: F821
print(f"clawdputer build sha: {sha}")
