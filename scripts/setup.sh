#!/bin/sh
# Fetch and pin the third-party checkouts this project builds against, under
# 3rdparty/ (which .gitignore keeps out of git). Safe to re-run: an existing
# checkout is fetched and moved to the pinned commit, not re-cloned.
#
#   elekloader  the SDK, the cores and the machines companions - needed to build
#   SAM         upstream Software Automatic Mouth - only for tools/sam_parity.py
#               and tools/vendor_sam.py; the port itself is in sam/ in this repo
#
# Override either one to pin your own tested version; a tag or a full commit
# hash both work:
#
#   ELK_REPO / ELK_REF, SAM_REPO / SAM_REF
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

ELK_REPO="${ELK_REPO:-https://github.com/irpina/elekloader}"
ELK_REF="${ELK_REF:-05d571c6d7968fb274e5fbc5c415fabb27c703f3}"
SAM_REPO="${SAM_REPO:-https://github.com/s-macke/SAM}"
SAM_REF="${SAM_REF:-a7b36efac730957b59471a42a45fd779f94d77dd}"

mkdir -p "$ROOT/3rdparty"

fetch() { # <repo-url> <dir> <ref>
    if [ -d "$2/.git" ]; then
        git -C "$2" fetch --tags --quiet
    else
        git clone --quiet "$1" "$2"
    fi
    git -C "$2" checkout --quiet "$3"
    printf '  %-28s %s (%s)\n' "$(basename "$2")" "$(git -C "$2" rev-parse --short HEAD)" "$3"
}

echo "3rdparty/:"
fetch "$ELK_REPO" "$ROOT/3rdparty/elekloader" "$ELK_REF"
fetch "$SAM_REPO" "$ROOT/3rdparty/SAM"        "$SAM_REF"
echo "done"
