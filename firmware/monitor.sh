#!/bin/bash
# Tail the dongle's serial console.
#
# Usage:
#   ./monitor.sh                  # auto-detect port
#   ./monitor.sh /dev/cu.usbXXX   # explicit port
#   ./monitor.sh -t 60            # custom timeout in seconds (default: streaming)
set -e

PORT=""
TIMEOUT=0
while [ $# -gt 0 ]; do
    case "$1" in
        -t) TIMEOUT="$2"; shift 2 ;;
        *)  PORT="$1"; shift ;;
    esac
done

PORT=${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
[ -z "$PORT" ] && { echo "No /dev/cu.usbmodem* found."; exit 1; }

echo "Monitoring $PORT at 115200 baud (Ctrl+C to exit)..."
python3 -c "
import serial, time, sys
s = serial.Serial('$PORT', 115200, timeout=0.1)
end = (time.time() + $TIMEOUT) if $TIMEOUT > 0 else None
try:
    while end is None or time.time() < end:
        d = s.read(4096)
        if d: sys.stdout.buffer.write(d); sys.stdout.flush()
except KeyboardInterrupt:
    pass
"
