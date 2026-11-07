#!/bin/sh
# Build hermes-3ds.cia (Home Menu install) after `make`. Needs makerom and bannertool:
#   https://github.com/3DSGuy/Project_CTR (makerom)   https://github.com/carstene1ns/3ds-bannertool
# Put both on PATH, or set MAKEROM / BANNERTOOL. Run from the app/ folder.
set -e
cd "$(dirname "$0")/.."
MAKEROM=${MAKEROM:-makerom}
BANNERTOOL=${BANNERTOOL:-bannertool}
VERSION=${VERSION:-1.0.0}
IFS=. read MAJ MIN MIC <<V
$VERSION
V
[ -f hermes-3ds.elf ] || make
$BANNERTOOL makebanner -i cia/banner.png -a cia/jingle.wav -o cia/banner.bnr >/dev/null
$BANNERTOOL makesmdh -s "Hermes 3DS" -l "Hermes Agent on your 3DS: push to talk, voice, camera" -p "Hermes 3DS" \
  -i cia/icon.png -o cia/icon.smdh >/dev/null
$MAKEROM -f cia -o hermes-3ds.cia -elf hermes-3ds.elf -rsf cia/hermes.rsf -icon cia/icon.smdh -banner cia/banner.bnr \
  -exefslogo -target t -major "$MAJ" -minor "$MIN" -micro "$MIC" \
  -DAPP_TITLE=Hermes3DS -DAPP_PRODUCT_CODE=CTR-H-HM3D -DAPP_ROMFS=romfs -DAPP_CATEGORY=Application \
  -DAPP_UNIQUE_ID=0xF4833 -DAPP_USE_ON_SD=true -DAPP_ENCRYPTED=false -DAPP_MEMORY_TYPE=Application \
  -DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=Legacy -DAPP_CPU_SPEED=268MHz -DAPP_ENABLE_L2_CACHE=false \
  -DAPP_VERSION_MAJOR="$MAJ"
ls -la hermes-3ds.cia
