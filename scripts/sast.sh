#!/usr/bin/env bash
# Local SAST: MicroHealth's rules (MicroHealthLLC/sast-rules) at exactly the
# commit this repository's CI pins, run the way CI runs them. Copied from
# sast-rules/local/sast.sh; it reads the pin, so it never needs editing.
#
#   scripts/sast.sh [--fail-on error|warning|none] [--json out.json]
#
# Needs git, curl, python3, sha256sum and docker or nerdctl logged in to
# MicroHealth's ECR.
set -euo pipefail
root="$(git rev-parse --show-toplevel)"
pin="$(grep -rhoE 'MicroHealthLLC/sast-rules/\.github/workflows/[a-z-]+\.yml@[0-9a-f]{40}' \
  "$root/.github/workflows" | head -1 | sed 's/.*@//')"
[ -n "$pin" ] || { echo "No sast-rules pin in .github/workflows." >&2; exit 2; }
cache="${XDG_CACHE_HOME:-$HOME/.cache}/sast-rules/${pin}"
if [ ! -f "$cache/.complete" ]; then
  rm -rf "$cache"; mkdir -p "$cache"
  git -C "$cache" init -q
  git -C "$cache" fetch -q --depth 1 https://github.com/MicroHealthLLC/sast-rules.git "$pin"
  git -C "$cache" checkout -q FETCH_HEAD
  [ "$(git -C "$cache" rev-parse HEAD)" = "$pin" ] || { echo "sast-rules fetch is not ${pin}" >&2; exit 1; }
  touch "$cache/.complete"
fi
exec "$cache/scripts/scan-local.sh" "$@" "$root"
