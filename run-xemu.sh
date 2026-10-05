#!/bin/sh
# Run the player in the Xemu MEGA65 emulator, with the sd/ folder standing in for the SD card (put .M65V files there).
#   XEMU    path to the xmega65 binary
#   M65_ROM path to a MEGA65/C65 ROM image you own
# With SHOT=/path/out.png: runs the test build (make build/m65play-test.prg FRAMES=n), which plays the first file by itself and exits
# with a screenshot after n pictures.
cd "$(dirname "$0")"
XEMU="${XEMU:-$HOME/Desktop/xemu-master/build/apps/xmega65.app/Contents/MacOS/xmega65}"
M65_ROM="${M65_ROM:-$HOME/Desktop/920413.bin}"
if [ -n "$SHOT" ]; then
    exec "$XEMU" -besure -fastboot -testing -hdosvirt -hdosdir sd -rom "$M65_ROM" -prg build/m65play-test.prg -screenshot "$SHOT"
fi
make >/dev/null || exit 1
exec "$XEMU" -besure -fastboot -hdosvirt -hdosdir sd -rom "$M65_ROM" -prg build/m65play.prg
