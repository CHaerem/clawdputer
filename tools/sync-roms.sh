#!/usr/bin/env bash
# Mirror a ROM manifest's contents to a directory on disk.
#
# The manifest is the declarative wishlist: each non-comment line is a ROM
# entry. The repo only stores URLs + names; never binaries. Run this on
# your host machine to materialize the ROMs onto a target directory (an
# SD card mount is the common target).
#
# Usage:
#   tools/sync-roms.sh [TARGET_DIR]
#
# TARGET_DIR defaults to ./roms-out/ (gitignored). For an SD card on macOS:
#   tools/sync-roms.sh /Volumes/CARDPUTER/roms
#
# Manifest sources, in order:
#   1. web/roms-manifest.txt       — public, in the repo
#   2. web/roms-manifest.private.txt — gitignored; for ROMs you don't want
#      to publicly list (e.g. commercial ROMs you legally own from your
#      own private storage). Format identical to the public manifest.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PUBLIC_MANIFEST="$REPO_ROOT/web/roms-manifest.txt"
PRIVATE_MANIFEST="$REPO_ROOT/web/roms-manifest.private.txt"
TARGET="${1:-$REPO_ROOT/roms-out}"

mkdir -p "$TARGET"

if [[ ! -f "$PUBLIC_MANIFEST" ]]; then
    echo "error: $PUBLIC_MANIFEST not found" >&2
    exit 1
fi

count_total=0
count_skipped=0
count_downloaded=0
count_failed=0

process_manifest() {
    local manifest="$1"
    [[ -f "$manifest" ]] || return 0
    echo
    echo "→ $manifest"
    while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
        # Strip leading whitespace, skip blanks + comments.
        local line="${raw_line#"${raw_line%%[![:space:]]*}"}"
        [[ -z "$line" || "$line" =~ ^# ]] && continue

        local name url
        if [[ "$line" == *"|"* ]]; then
            name="${line%%|*}"
            url="${line#*|}"
            # Trim
            name="${name%"${name##*[![:space:]]}"}"
            name="${name#"${name%%[![:space:]]*}"}"
            url="${url%"${url##*[![:space:]]}"}"
            url="${url#"${url%%[![:space:]]*}"}"
        else
            url="${line%"${line##*[![:space:]]}"}"
            url="${url#"${url%%[![:space:]]*}"}"
            # Derive name from URL basename.
            name="${url##*/}"
            name="${name%%\?*}"
        fi

        # Pick a filename: prefer the display name on the left of "|" if it
        # has a supported extension (this is how the manifest disambiguates
        # collisions). Otherwise fall back to the URL basename.
        local fname
        case "$name" in
            *.gb|*.gbc|*.GB|*.GBC) fname="$name" ;;
            *)
                fname="${url##*/}"
                fname="${fname%%\?*}"
                ;;
        esac

        local dest="$TARGET/$fname"
        count_total=$((count_total + 1))

        if [[ -f "$dest" ]]; then
            # Compare size: if local matches HEAD's Content-Length, skip.
            local remote_len
            remote_len="$(curl -sLI "$url" --max-time 15 \
                | awk -v IGNORECASE=1 '/^content-length:/ {print $2}' \
                | tr -d '\r' | tail -1)"
            local local_len
            local_len="$(wc -c < "$dest" | tr -d ' ')"
            if [[ -n "$remote_len" && "$remote_len" == "$local_len" ]]; then
                echo "  skip   $fname ($local_len bytes, unchanged)"
                count_skipped=$((count_skipped + 1))
                continue
            fi
        fi

        echo -n "  fetch  $fname … "
        if curl -fsSL "$url" -o "$dest.part" --max-time 120; then
            mv "$dest.part" "$dest"
            local sz
            sz="$(wc -c < "$dest" | tr -d ' ')"
            echo "ok ($sz bytes)"
            count_downloaded=$((count_downloaded + 1))
        else
            rm -f "$dest.part"
            echo "FAIL"
            count_failed=$((count_failed + 1))
        fi
    done < "$manifest"
}

process_manifest "$PUBLIC_MANIFEST"
process_manifest "$PRIVATE_MANIFEST"

echo
echo "summary: $count_total entries, $count_downloaded downloaded, $count_skipped skipped, $count_failed failed"
echo "target:  $TARGET"
[[ $count_failed -eq 0 ]] || exit 1
