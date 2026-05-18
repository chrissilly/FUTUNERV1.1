#!/bin/bash
# Build (if needed) and flash the FUTUNER v2 firmware to a connected dongle.
#
# Default behavior: flash app slot 0 only (fast, ~5 sec). Use --full to
# write bootloader + partition table + otadata + app (one-time per dongle).
#
# Usage:
#   ./flash.sh                  # build + flash app to first /dev/cu.usbmodem*
#   ./flash.sh --full           # also flash bootloader + partition table
#   ./flash.sh --prebuilt       # use prebuilt/futuner_v2.bin (skip build)
#   ./flash.sh /dev/cu.usbXXX   # explicit port

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

FULL=0
PREBUILT=0
PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --full)     FULL=1; shift ;;
        --prebuilt) PREBUILT=1; shift ;;
        *)          PORT="$1"; shift ;;
    esac
done
PORT=${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
[ -z "$PORT" ] && { echo "No /dev/cu.usbmodem* found. Plug in the dongle."; exit 1; }

PYTHON_PATH=${IDF_PYTHON:-$HOME/.espressif/python_env/idf5.5_py3.9_env/bin/python}
[ -x "$PYTHON_PATH" ] || PYTHON_PATH=python3

if [ $PREBUILT -eq 1 ]; then
    APP_BIN="$DIR/prebuilt/futuner_v2.bin"
    BOOT_BIN="$DIR/prebuilt/bootloader.bin"
    PT_BIN="$DIR/prebuilt/partition-table.bin"
    OTA_BIN="$DIR/prebuilt/ota_data_initial.bin"
else
    # Build first
    ./build.sh
    APP_BIN="$DIR/build/futuner_v2.bin"
    BOOT_BIN="$DIR/build/bootloader/bootloader.bin"
    PT_BIN="$DIR/build/partition_table/partition-table.bin"
    OTA_BIN="$DIR/build/ota_data_initial.bin"
fi

[ -f "$APP_BIN" ] || { echo "Missing $APP_BIN"; exit 1; }

CMD=("$PYTHON_PATH" -m esptool --chip esp32s3 -b 460800 --port "$PORT"
     --before default_reset --after hard_reset write_flash
     --flash_mode dio --flash_size 16MB --flash_freq 80m)

if [ $FULL -eq 1 ]; then
    echo "=== full flash to $PORT ==="
    "${CMD[@]}" \
        0x0     "$BOOT_BIN" \
        0x8000  "$PT_BIN" \
        0xe000  "$OTA_BIN" \
        0x10000 "$APP_BIN"
else
    echo "=== app-only flash to $PORT ==="
    "${CMD[@]}" 0x10000 "$APP_BIN"
fi

echo
echo "Flashed. Watch boot:  ./monitor.sh $PORT"
