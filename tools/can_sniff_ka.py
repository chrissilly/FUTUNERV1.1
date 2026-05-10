#!/usr/bin/env python3
"""can_sniff with TesterPresent keep-alive — single-process variant.

Sniffer behavior identical to tools/can_sniff.py, plus a TesterPresent
(02 3E 00) frame transmitted on 0x7E0 every --ka-interval seconds
(default 2.0). Used during the MagicMotorsport multi-cycle capture
session to keep the J533 gateway from putting the bus to sleep
between flash cycles.

pyusb cannot share a Candlelight handle across processes — libusb
claims the bulk interface exclusively. So sniff and keep-alive must
live in one process. We interleave RX and TX in a single thread:
recv with a short timeout (100 ms), then check the keep-alive timer
on every loop iteration. This avoids cross-thread USB use entirely.

Output format and CLI is a superset of can_sniff.py, so the existing
parser treats this output identically.

The keep-alive transmissions appear in the capture as 0x7E0 frames
(gs_usb echoes back our own transmits). Downstream parsers that
care about MM-sourced services should distinguish these by their
fixed 02 3E 00 ... payload pattern and ~2 s cadence.
"""
import argparse
import sys
import time
from pathlib import Path

# Reuse device functions from can_sniff to avoid duplicating gs_usb code.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from can_sniff import (find_candlelight, set_host_format, set_bittiming,
                       start_can, stop_can, recv_frame, send_frame)

TP_PAYLOAD = b'\x02\x3E\x00\x00\x00\x00\x00\x00'


def main():
    p = argparse.ArgumentParser(description='CAN sniffer + TesterPresent keep-alive')
    p.add_argument('-d', '--duration', type=int, default=10,
                   help='Sniff duration in seconds')
    p.add_argument('-f', '--filter', nargs='+', type=lambda x: int(x, 0),
                   help='Filter CAN IDs (hex) for output (RX traffic only — '
                        'TX echoes pass through this filter too)')
    p.add_argument('--ka-interval', type=float, default=2.0,
                   help='TesterPresent keep-alive interval, seconds (default 2.0)')
    p.add_argument('--ka-id', type=lambda x: int(x, 0), default=0x7E0,
                   help='CAN ID to send TesterPresent on (default 0x7E0)')
    args = p.parse_args()

    dev = find_candlelight()
    set_host_format(dev)
    set_bittiming(dev)
    start_can(dev)

    print(f"Sniffing CAN @ 500kbps for {args.duration}s "
          f"with TP keep-alive on 0x{args.ka_id:03X} every {args.ka_interval}s...",
          flush=True)
    if args.filter:
        print(f"Filtering: {', '.join(f'0x{x:03X}' for x in args.filter)}", flush=True)

    start = time.time()
    last_ka = start - args.ka_interval  # send first TP immediately
    count = 0
    ka_count = 0

    try:
        while time.time() - start < args.duration:
            now = time.time()

            # Time to send TesterPresent?
            if now - last_ka >= args.ka_interval:
                try:
                    send_frame(dev, args.ka_id, TP_PAYLOAD)
                    ka_count += 1
                except Exception as e:
                    print(f"# KA tx error: {e}", flush=True)
                last_ka = now

            # Read with short timeout so we re-check the KA timer often
            frame = recv_frame(dev, timeout_ms=100)
            if frame:
                arb_id, dlc, data = frame
                if args.filter and arb_id not in args.filter:
                    continue
                ts = time.time() - start
                print(f"{ts:8.3f}  {arb_id:03X}  [{dlc}]  {data.hex(' ')}",
                      flush=True)
                count += 1
    except KeyboardInterrupt:
        pass
    finally:
        try:
            stop_can(dev)
        except Exception:
            pass

    print(f"\n{count} frames captured, {ka_count} TP keep-alives sent")


if __name__ == '__main__':
    main()
