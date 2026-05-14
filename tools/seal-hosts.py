#!/usr/bin/env python3
"""Seal an SSH host list with the Cardputer's per-device seal key.

Workflow:
  1. On the device: Settings → "show seal key" — copy the base64 line.
  2. On the Mac:
       export CLAWD_SEAL_KEY="<base64-line>"
       python3 tools/seal-hosts.py
  3. The script reads tools/hosts.json (or path argument), encrypts with
     AES-256-GCM and writes firmware/secrets/ssh_hosts.sealed.
  4. Commit the sealed file. CI builds → device pulls → unsealed at runtime.

Sealed format: nonce(12) || ciphertext || tag(16) — matches what
firmware/src/services/sealed.cpp expects.

Dependencies: just the Python stdlib + `cryptography` (pip install cryptography).
"""

from __future__ import annotations

import base64
import json
import os
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:
    print("missing dependency: pip install cryptography", file=sys.stderr)
    sys.exit(1)

REPO = Path(__file__).resolve().parent.parent
HOSTS_JSON = REPO / "tools" / "hosts.json"
SEALED_OUT = REPO / "firmware" / "secrets" / "ssh_hosts.sealed"


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

    hosts_path = Path(sys.argv[1]) if len(sys.argv) > 1 else HOSTS_JSON
    if not hosts_path.exists():
        print(f"error: {hosts_path} not found", file=sys.stderr)
        print("create it with entries like:", file=sys.stderr)
        print(
            json.dumps([{"name": "mac", "host": "192.168.x.x", "user": "you", "port": 22}], indent=2),
            file=sys.stderr,
        )
        return 1

    plaintext = hosts_path.read_bytes()
    try:
        json.loads(plaintext)
    except json.JSONDecodeError as exc:
        print(f"error: {hosts_path} is not valid JSON: {exc}", file=sys.stderr)
        return 1

    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    ct_and_tag = aesgcm.encrypt(nonce, plaintext, None)

    SEALED_OUT.parent.mkdir(parents=True, exist_ok=True)
    SEALED_OUT.write_bytes(nonce + ct_and_tag)
    print(f"wrote {SEALED_OUT} ({len(SEALED_OUT.read_bytes())} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
