#!/usr/bin/env python3
"""Generate web/apps.json from the firmware App registrations.

Parses each firmware/src/apps/<name>/<name>.cpp file, extracts the
`App xxx_app = { .id = "...", .name = "...", ... };` block, and emits a
JSON manifest that the web demo loads at startup. This keeps the demo
launcher in sync with whatever apps the firmware actually ships — adding
a new app to firmware/src/apps/ automatically gives it a tile in the
hosted demo on the next deploy.

Usage:
    python3 tools/gen-web-manifest.py [--out web/apps.json]
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

APP_BLOCK_RE = re.compile(
    r'App\s+\w+_app\s*=\s*\{(?P<body>.*?)\}\s*;',
    re.DOTALL,
)
FIELD_STR_RE  = re.compile(r'\.(?P<key>\w+)\s*=\s*"(?P<val>[^"]*)"')
FIELD_BOOL_RE = re.compile(r'\.(?P<key>\w+)\s*=\s*(?P<val>true|false)\b')
FIELD_SVC_RE  = re.compile(r'\.services\s*=\s*(?P<val>[A-Za-z0-9_ |]+)')


def parse_app(cpp_path):
    with open(cpp_path, 'r', encoding='utf-8') as fh:
        text = fh.read()
    m = APP_BLOCK_RE.search(text)
    if not m:
        return None
    body = m.group('body')
    app = {}
    for fm in FIELD_STR_RE.finditer(body):
        app[fm.group('key')] = fm.group('val')
    for fm in FIELD_BOOL_RE.finditer(body):
        app[fm.group('key')] = (fm.group('val') == 'true')
    svc = FIELD_SVC_RE.search(body)
    if svc:
        app['services'] = [s.strip() for s in svc.group('val').split('|') if s.strip()]
    return app if app.get('id') else None


def find_apps(repo_root):
    pattern = os.path.join(repo_root, 'firmware', 'src', 'apps', '*', '*.cpp')
    out = []
    for cpp in sorted(glob.glob(pattern)):
        app = parse_app(cpp)
        if app:
            app['source'] = os.path.relpath(cpp, repo_root).replace(os.sep, '/')
            out.append(app)
    return out


def git_sha(repo_root):
    try:
        return subprocess.check_output(
            ['git', '-C', repo_root, 'rev-parse', '--short', 'HEAD'],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return None


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    default_out = os.path.join(repo_root, 'web', 'apps.json')

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', default=default_out, help='output JSON path')
    args = ap.parse_args()

    apps = find_apps(repo_root)
    if not apps:
        print('ERROR: no firmware apps found — bad cwd?', file=sys.stderr)
        sys.exit(1)

    manifest = {
        'generated_at': datetime.now(timezone.utc).isoformat(timespec='seconds'),
        'sha': git_sha(repo_root),
        'apps': apps,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8') as fh:
        json.dump(manifest, fh, indent=2)
        fh.write('\n')
    print(f'wrote {len(apps)} apps to {os.path.relpath(args.out, repo_root)}')


if __name__ == '__main__':
    main()
