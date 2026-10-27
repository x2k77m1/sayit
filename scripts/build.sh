#!/bin/sh
# Build one SAY IT machine from its folder under machines/: the device's core,
# its companion mod, and the machine itself, then lint the combination and
# write the two artifacts to out/:
#
#   machines/<machine>/out/<id>-<ver>.elemod   the mod alone - the file to hand
#                                              out and to feed to elekloader to
#                                              build a custom firmware
#   out/<id>-<os>-<ver>.syx                     a patched OS image for your own unit
#
# The .syx contains Elektron's firmware, so it is a local test image: never
# share it, never commit it (out/ and *.syx are ignored).
#
# Usage: sh scripts/build.sh MACHINE STOCK [MODVER]
#   MACHINE  digitone | digitakt - the folder under machines/ to build
#   STOCK    that device's stock OS .syx (Elektron's download; local input)
#   MODVER   stamps the build (ELE3 needs exactly 4 characters) and names it.
#            Defaults to the machine's mod.json ele3_version, which it must
#            match.
#
# Run scripts/setup.sh first (or keep your own 3rdparty/elekloader checkout).
set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

MACHINE="${1:?usage: build.sh MACHINE STOCK [MODVER]}"
STOCK="${2:?usage: build.sh MACHINE STOCK [MODVER]}"
MOD="machines/$MACHINE"
[ -f "$MOD/mod.json" ] || { echo "no $MOD/mod.json" >&2; exit 2; }
export ELEKLOADER_CROSS="${ELEKLOADER_CROSS:-m68k-elf-}"
export PYTHONPATH="$ROOT/3rdparty/elekloader:${PYTHONPATH:-}"
mkdir -p out

json() { python3 -c "import json;print(json.load(open('$MOD/mod.json'))$1)"; }
ID=$(json "['id']")
OS=$(json "['os']")
DEVICE=$(json "['device']")
MODVER="${3:-$(json "['ele3_version']")}"

# The core and the companion mod a machine needs, by device.
case "$DEVICE" in
digitone-mk1) EL=3rdparty/elekloader; CORE=$EL/mods/core-dn1;   COMPANION=$EL/mods/machines-dn1 ;;
digitakt-mk1) EL=3rdparty/elekloader; CORE=$EL/mods/core;       COMPANION=$EL/mods/machine-pages ;;
*) echo "no build recipe for device '$DEVICE'" >&2; exit 2 ;;
esac

newest() { ls -t $1 2>/dev/null | head -n 1; }

echo "== $MACHINE on $DEVICE OS $OS =="
python3 -m elekloader.sdk.build "$CORE"      --stock "$STOCK"
python3 -m elekloader.sdk.build "$COMPANION" --stock "$STOCK"
python3 -m elekloader.sdk.build "$MOD"       --stock "$STOCK"

CORE_M=$(newest "$CORE/out/*.elemod")
COMP_M=$(newest "$COMPANION/out/*.elemod")
MOD_M=$(newest  "$MOD/out/$ID-*.elemod")

python3 -m elekloader.lint "$MOD_M" --stock "$STOCK" \
    --with "$CORE_M" --with "$COMP_M"
python3 -m elekloader.patch --stock "$STOCK" \
    --mod "$CORE_M" --mod "$COMP_M" --mod "$MOD_M" \
    --out "out/$ID-$OS-$MODVER.syx" --version "$MODVER"

# The file to hand out, named for its device: the two machines of one release
# carry the same id and version, so a release page would otherwise hold two
# different files under one name.
REL="out/$ID-$MODVER-$MACHINE.elemod"
cp "$MOD_M" "$REL"

echo
echo "release artifact (share this):  $REL"
echo "                                 (elekloader's own copy: $MOD_M)"
echo "local test image (never share): out/$ID-$OS-$MODVER.syx"
