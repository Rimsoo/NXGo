#!/bin/sh
# Packs nxgo.elf + romfs/ into an installable NSP (title 05004E58476F0000).
#
# Needs hacBrewPack (https://github.com/rlaphoenix/hacBrewPack) and the
# prod.keys dumped from your own console (Lockpick_RCM):
#   HACBREWPACK=/path/to/hacbrewpack KEYS=~/.switch/prod.keys tools/make_nsp.sh
# The NSP lands in nxgo.nsp; install it with Sphaira/DBI/Tinfoil.
# Installed homebrew titles need sigpatches (e.g. sys-patch).
set -e
cd "$(dirname "$0")/.."

TITLEID=05004e58476f0000
HACBREWPACK=${HACBREWPACK:-hacbrewpack}
KEYS=${KEYS:-$HOME/.switch/prod.keys}
VERSION=$(sed -n 's/^APP_VERSION[[:space:]]*:=[[:space:]]*//p' Makefile)
O=build/nsp

[ -f nxgo.elf ] || { echo "nxgo.elf missing: run make first" >&2; exit 1; }
[ -f "$KEYS" ] || { echo "keys not found: $KEYS" >&2; exit 1; }

rm -rf "$O"
mkdir -p "$O/exefs" "$O/control" "$O/romfs"
elf2nso nxgo.elf "$O/exefs/main"
npdmtool nsp/nxgo.json "$O/exefs/main.npdm" 2>&1 | grep -v "field not present" || true
nacptool --create "NXGo" "Rimso" "$VERSION" "$O/control/control.nacp" --titleid=0x$TITLEID
# No user picker on launch: StartupUserAccount = None
printf '\000' | dd of="$O/control/control.nacp" bs=1 seek=$((0x3025)) conv=notrunc status=none
cp icon.jpg "$O/control/icon_AmericanEnglish.dat"
cp -r romfs/. "$O/romfs/"

"$HACBREWPACK" -k "$KEYS" --titleid $TITLEID --nologo \
    --exefsdir "$O/exefs" --romfsdir "$O/romfs" --controldir "$O/control" \
    --nspdir "$O/out" --ncadir "$O/nca" --tempdir "$O/tmp" --backupdir "$O/bk" >"$O/hacbrewpack.log"
mv "$O/out/$TITLEID.nsp" nxgo.nsp
echo "nxgo.nsp ($(du -h nxgo.nsp | cut -f1))"
