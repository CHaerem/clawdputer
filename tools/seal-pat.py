#!/usr/bin/env python3
"""Seal a GitHub PAT with the Cardputer's per-device seal key.

Workflow:
  1. On the device: Settings → "show seal key" — copy the base64 line.
  2. On the Mac:
       export CLAWD_SEAL_KEY="<base64-line>"
       echo "ghp_..." > tools/github_pat.txt   # gitignored
       python3 tools/seal-pat.py
  3. The script encrypts the PAT with AES-256-GCM and writes
     firmware/secrets/github_pat.sealed.
  4. Commit the sealed file. CI builds → device pulls → unsealed at
     runtime by services/sealed.cpp::unsealGithubPat().

Sealed format: nonce(12) || ciphertext || tag(16) — matches what
firmware/src/services/sealed.cpp expects.

Dependencies: just the Python stdlib + `cryptography`.
"""

from __future__ import annotations

import base64
import os
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    print("missing dependency: pip install cryptography", file=sys.stderr)
    sys.exit(1)

REPO       = Path(__file__).resolve().parent.parent
PAT_TXT    = REPO / "tools" / "github_pat.txt"
SEALED_OUT = REPO / "firmware" / "secrets" / "github_pat.sealed"


def main() -> int:
    key_b64 = os.environ.get("CLAWD_SEAL_KEY")
    if not key_b64:
        print("error: CLAWD_SEAL_KEY not set (Settings → show seal key on device)",
              file=sys.stderr)
        return 1

    try:
        key = base64.b64decode(key_b64.strip())
    except Exception as exc:
        print(f"error: CLAWD_SEAL_KEY is not valid base64: {exc}", file=sys.stderr)
        return 1
    if len(key) != 32:
        print(f"error: seal key must be 32 bytes (got {len(key)})", file=sys.stderr)
        return 1

    pat_path = Path(sys.argv[1]) if len(sys.argv) > 1 else PAT_TXT
    if not pat_path.exists():
        print(f"error: {pat_path} not found", file=sys.stderr)
        print("create it with a single line containing the PAT, e.g.:",
              file=sys.stderr)
        print('  echo "ghp_yourTokenHere" > tools/github_pat.txt', file=sys.stderr)
        return 1

    plaintext = pat_path.read_text().strip().encode()
    if not plaintext.startswith(b"ghp_") and not plaintext.startswith(b"github_pat_"):
        print("warning: PAT does not start with 'ghp_' or 'github_pat_' — sealing anyway",
              file=sys.stderr)

    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    ct_and_tag = aesgcm.encrypt(nonce, plaintext, None)

    SEALED_OUT.parent.mkdir(parents=True, exist_ok=True)
    SEALED_OUT.write_bytes(nonce + ct_and_tag)
    print(f"wrote {SEALED_OUT} ({len(SEALED_OUT.read_bytes())} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
