#!/usr/bin/env python3
"""CAN sniffer for candleLight (Canable v1.1, gs_usb firmware). Works on macOS and Linux/WSL2.

On Linux, the kernel's gs_usb module will claim the device by default. This script
detaches the kernel driver in find_candlelight() so pyusb can open the device directly.

If `lsusb` shows the device but the script can't open it, you have a permissions
problem. Fastest fix: run with `sudo`. Permanent fix: install a udev rule giving
your user (or the `plugdev` group) write access to 1d50:606f. Example udev rule
in /etc/udev/rules.d/99-candlelight.rules:

  SUBSYSTEM=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606f", MODE="0666"

Then `sudo udevadm control --reload-rules && sudo udevadm trigger`.
"""
import os
import sys
import time
import struct
import usb.core
import usb.util

# ----- HIL behavior config (defaults; override via CLI flags) -------------
# Named here so callers + docs + --help stay in sync. Per project CLAUDE.md
# "no magic numbers" rule, every behavioral knob is here, not inline.
DEFAULT_DURATION_S         = 10
RECV_TIMEOUT_MS            = 500
UDS_RESPONSE_WAIT_S        = 2.0
UDS_REQUEST_ID             = 0x7E0
UDS_RESPONSE_ID            = 0x7E8

# Max UDS single-frame payload bytes (length byte + 7 data bytes = 8-byte CAN frame).
UDS_MAX_DATA_BYTES         = 7

# candump line format: "(SEC.USEC) <iface> <ID>#<DATA>"
# Replay-compatible with canplayer / cangen.
CANDUMP_INTERFACE_LABEL    = "can0"
CANDUMP_TIMESTAMP_USEC_PER_S = 1_000_000   # truncated microseconds, matches canutils
CANDUMP_DATA_BYTE_SEP      = " "    # space-separated hex bytes per HIL doc example
ANOMALY_STDERR_PREFIX      = "ANOMALY"
ABORT_ON_ANOMALY_EXIT_CODE = 2

# Legacy pretty-print column widths (used when neither --out nor --timestamp set).
LEGACY_REL_TS_WIDTH        = 8
LEGACY_REL_TS_PRECISION    = 3
LEGACY_BYTE_SEP            = " "

# candleLight gs_usb protocol constants
GS_USB_BREQ_HOST_FORMAT = 0
GS_USB_BREQ_BITTIMING = 1
GS_USB_BREQ_MODE = 2
GS_USB_BREQ_DATA_BITTIMING = 4

GS_CAN_MODE_START = 0
GS_CAN_MODE_RESET = 1

CAN_BITRATE_500K = {
    'prop_seg': 1,
    'phase_seg1': 12,
    'phase_seg2': 2,
    'sjw': 1,
    'brp': 6,
}

VID = 0x1D50
PID = 0x606F

def find_candlelight():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if not dev:
        print("ERROR: candleLight not found (VID:PID 1d50:606f)")
        print("Check: lsusb | grep 1d50  -- device must be visible")
        print("If you see it but find() returns None, you likely need sudo or a udev rule")
        sys.exit(1)
    # On Linux/WSL2 the gs_usb kernel module may have claimed the device.
    # Detach it so pyusb can open the interface directly. No-op on macOS.
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (NotImplementedError, usb.core.USBError) as e:
        print(f"Note: kernel-driver detach skipped: {e}", file=sys.stderr)
    try:
        dev.set_configuration()
    except usb.core.USBError as e:
        print(f"ERROR: set_configuration failed: {e}", file=sys.stderr)
        print("Likely a permissions problem. Try `sudo python3 tools/can_sniff.py ...`", file=sys.stderr)
        print("Or install the udev rule (see comment block at top of file).", file=sys.stderr)
        sys.exit(1)
    return dev

def set_bittiming(dev, channel=0):
    data = struct.pack('<IIIII',
        CAN_BITRATE_500K['prop_seg'],
        CAN_BITRATE_500K['phase_seg1'],
        CAN_BITRATE_500K['phase_seg2'],
        CAN_BITRATE_500K['sjw'],
        CAN_BITRATE_500K['brp'])
    dev.ctrl_transfer(0x41, GS_USB_BREQ_BITTIMING, channel, 0, data)

def set_host_format(dev):
    dev.ctrl_transfer(0x41, GS_USB_BREQ_HOST_FORMAT, 1, 0, struct.pack('<I', 0xEFBE0000))

def start_can(dev, channel=0):
    data = struct.pack('<II', GS_CAN_MODE_START, 0)
    dev.ctrl_transfer(0x41, GS_USB_BREQ_MODE, channel, 0, data)

def stop_can(dev, channel=0):
    data = struct.pack('<II', GS_CAN_MODE_RESET, 0)
    dev.ctrl_transfer(0x41, GS_USB_BREQ_MODE, channel, 0, data)

def recv_frame(dev, timeout_ms=RECV_TIMEOUT_MS):
    """Receive a CAN frame. Returns (arb_id, dlc, data) or None."""
    try:
        # Read from bulk IN endpoint (0x81)
        raw = dev.read(0x81, 64, timeout=timeout_ms)
        if raw and len(raw) >= 12:
            # gs_host_frame: echo_id(4) can_id(4) can_dlc(1) channel(1) flags(1) reserved(1) data(8)
            echo_id, can_id, can_dlc = struct.unpack_from('<IIB', raw, 0)
            data = bytes(raw[12:12+can_dlc])
            return (can_id & 0x1FFFFFFF, can_dlc, data)
    except usb.core.USBTimeoutError:
        return None
    except usb.core.USBError as e:
        if e.errno == 110:  # timeout
            return None
        raise
    return None

def send_frame(dev, arb_id, data, channel=0):
    """Send a CAN frame."""
    dlc = len(data)
    padded = data + b'\x00' * (8 - dlc)
    frame = struct.pack('<IIBBBx8s', 0, arb_id, dlc, channel, 0, padded)
    dev.write(0x02, frame)

def parse_allow_list(spec):
    """Parse a comma-separated list of hex CAN IDs. Case insensitive.
       "7E0,7e8,7DF" -> frozenset({0x7E0, 0x7E8, 0x7DF}).
       None         -> None (allowlist not configured; no frames are anomalous).
       ""           -> frozenset() (explicit empty allowlist; every frame is anomalous).
    Hex-only per the HIL handoff doc contract; unprefixed tokens parse as base 16."""
    if spec is None:
        return None
    if spec.strip() == "":
        return frozenset()
    ids = set()
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        # Hex-only: "0x7E0" or bare "7E0" both parse as 0x7E0.
        ids.add(int(tok, 16))
    return frozenset(ids)


def format_candump_line(arb_id, data, wall_ts=None):
    """Build a candump-format line. If wall_ts is provided, prepend the
    "(SEC.USEC) " timestamp prefix with truncated (not rounded) microseconds
    to match canutils' candump output exactly. Always emits "<iface> <ID>#<DATA>"."""
    data_hex = CANDUMP_DATA_BYTE_SEP.join(f"{b:02X}" for b in data)
    body = f"{CANDUMP_INTERFACE_LABEL} {arb_id:03X}#{data_hex}"
    if wall_ts is not None:
        sec = int(wall_ts)
        usec = int((wall_ts - sec) * CANDUMP_TIMESTAMP_USEC_PER_S)
        return f"({sec}.{usec:06d}) {body}"
    return body


def sniff(duration=DEFAULT_DURATION_S,
          filter_ids=None,
          out_path=None,
          timestamp_on=False,
          allow_ids=None,
          watch_anomalies=False,
          abort_on_anomaly=False,
          overwrite=False,
          tee=False):
    """Sniff CAN traffic. See module __main__ argparse for flag semantics.

    Returns (frame_count, anomaly_count). Exits with ABORT_ON_ANOMALY_EXIT_CODE
    if --abort-on-anomaly fires (after stopping the CAN device cleanly).
    """
    # --- Validate output path before opening hardware ------------------
    out_file = None
    if out_path is not None:
        if os.path.exists(out_path) and not overwrite:
            print(f"ERROR: --out path exists: {out_path}", file=sys.stderr)
            print("       Pass --overwrite to replace, or choose a different path.",
                  file=sys.stderr)
            sys.exit(1)
        # Open binary-mode to avoid newline translation surprises across hosts.
        # Line-buffered (buffering=1 is line-buf for text mode only; for bytes
        # we flush per-frame to keep the capture durable if the session crashes).
        out_file = open(out_path, "wb")

    # --- Warn on flag combinations that don't do anything --------------
    if watch_anomalies and allow_ids is None:
        # allow_ids is frozenset() for an explicit empty --allow "" — that's
        # NOT a no-op (every frame fires). Only None (allow not supplied)
        # disables anomaly detection.
        print("WARN: --watch-for-anomalies set without --allow; no allowlist means "
              "no anomalies will ever fire.", file=sys.stderr)
    if abort_on_anomaly and not watch_anomalies:
        print("WARN: --abort-on-anomaly is a no-op without --watch-for-anomalies.",
              file=sys.stderr)
    if tee and out_path is None:
        print("WARN: --tee is a no-op without --out (stdout already prints).",
              file=sys.stderr)

    # When --out is set, suppress stdout-mirror unless --tee was also given.
    # This is the doc contract: file capture defaults to silent stdout so
    # parallel sniffer invocations don't fight for terminal space.
    stdout_active = (out_path is None) or tee

    # --- Bring up CAN --------------------------------------------------
    dev = find_candlelight()
    set_host_format(dev)
    set_bittiming(dev)
    start_can(dev)

    print(f"Sniffing CAN @ 500kbps for {duration}s...", file=sys.stderr)
    if filter_ids:
        print(f"Filtering (captured to output): {', '.join(f'0x{x:03X}' for x in filter_ids)}",
              file=sys.stderr)
    if allow_ids is not None:
        allow_str = ", ".join(f"0x{x:03X}" for x in sorted(allow_ids))
        print(f"Anomaly allowlist: {allow_str}", file=sys.stderr)
    if out_path:
        print(f"Capture file: {out_path} (overwrite={overwrite}, tee={tee})",
              file=sys.stderr)

    # Use monotonic clock for the duration guard (immune to NTP step / wall
    # clock adjustments mid-session) but wall-clock time.time() for the
    # timestamps written into the capture (replay tools expect wall time).
    start_mono = time.monotonic()
    start_wall = time.time()
    frame_count = 0
    anomaly_count = 0
    aborted = False
    try:
        while time.monotonic() - start_mono < duration:
            frame = recv_frame(dev, timeout_ms=RECV_TIMEOUT_MS)
            if frame is None:
                continue
            arb_id, dlc, data = frame
            wall_ts = time.time()

            # Anomaly check runs BEFORE --filter so filtering can't mask
            # out-of-band traffic. --filter only restricts what reaches
            # the capture stream; anomalies always reach stderr.
            is_anomaly = (watch_anomalies
                          and allow_ids is not None
                          and arb_id not in allow_ids)
            if is_anomaly:
                anomaly_count += 1
                print(f"{ANOMALY_STDERR_PREFIX} t={wall_ts:.{CANDUMP_TIMESTAMP_PRECISION}f} "
                      f"id={arb_id:03X} data={data.hex()}", file=sys.stderr)
                if abort_on_anomaly:
                    aborted = True
                    break

            if filter_ids and arb_id not in filter_ids:
                continue

            # --- emit the frame ---------------------------------------
            if out_file is not None:
                # File is always full candump format with timestamp.
                line = format_candump_line(arb_id, data, wall_ts=wall_ts)
                out_file.write(line.encode("ascii") + b"\n")
                out_file.flush()  # durable capture even on crash

            if stdout_active:
                if timestamp_on:
                    # candump-style line on stdout too.
                    print(format_candump_line(arb_id, data, wall_ts=wall_ts))
                else:
                    # Legacy pretty form, kept for back-compat when no
                    # new flags are passed.
                    rel_ts = wall_ts - start_wall
                    print(f"{rel_ts:{LEGACY_REL_TS_WIDTH}.{LEGACY_REL_TS_PRECISION}f}"
                          f"  {arb_id:03X}  [{dlc}]  {data.hex(LEGACY_BYTE_SEP)}")

            frame_count += 1
    except KeyboardInterrupt:
        pass
    finally:
        stop_can(dev)
        if out_file is not None:
            out_file.close()

    print(f"\n{frame_count} frames captured, {anomaly_count} anomalies",
          file=sys.stderr)
    if aborted:
        sys.exit(ABORT_ON_ANOMALY_EXIT_CODE)
    return frame_count, anomaly_count

def send_uds(dev, data):
    """Send a UDS request on UDS_REQUEST_ID and wait for response on UDS_RESPONSE_ID."""
    padded = bytes([len(data)]) + data + b'\x00' * (UDS_MAX_DATA_BYTES - len(data))
    send_frame(dev, UDS_REQUEST_ID, padded)

    start = time.time()
    while time.time() - start < UDS_RESPONSE_WAIT_S:
        frame = recv_frame(dev, timeout_ms=RECV_TIMEOUT_MS)
        if frame:
            arb_id, dlc, fdata = frame
            if arb_id == UDS_RESPONSE_ID:
                return fdata
    return None

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(
        description='CAN sniffer for candleLight on macOS / Linux. '
                    'Capture flags match the HIL Phase 1 validation handoff contract.')
    parser.add_argument('-d', '--duration', type=int, default=DEFAULT_DURATION_S,
                        help=f'Sniff duration in seconds (default {DEFAULT_DURATION_S})')
    parser.add_argument('-f', '--filter', nargs='+', type=lambda x: int(x, 0),
                        help='Restrict CAPTURED frames to these CAN IDs (hex/dec). '
                             'Anomaly detection (--watch-for-anomalies) still sees all frames.')
    parser.add_argument('--uds', action='store_true',
                        help=f'Send TesterPresent on 0x{UDS_REQUEST_ID:03X} and print '
                             f'response from 0x{UDS_RESPONSE_ID:03X}')
    parser.add_argument('--out', dest='out_path', metavar='PATH', default=None,
                        help='Write candump-format capture to PATH (replay-compatible '
                             'with cangen/canplayer). Fails if PATH exists unless '
                             '--overwrite is also given. Suppresses stdout mirror '
                             'unless --tee is also given.')
    parser.add_argument('--timestamp', action='store_true',
                        help='Prefix every stdout line with the candump-style '
                             '"(seconds.microseconds)" timestamp. The --out file '
                             'is always candump-format regardless of this flag.')
    parser.add_argument('--allow', dest='allow_spec', metavar='ID[,ID...]', default=None,
                        help='Comma-separated hex CAN ID allowlist (e.g. "7E0,7E8,7DF"). '
                             'Defines the baseline for --watch-for-anomalies. Without '
                             '--allow, no allowlist exists and no frames are anomalous. '
                             'Case insensitive.')
    parser.add_argument('--watch-for-anomalies', dest='watch_anomalies', action='store_true',
                        help='Log every frame whose ID is NOT in --allow to stderr as '
                             '"ANOMALY t=<ts> id=<hex> data=<hex>". Default is '
                             'print-and-continue; pair with --abort-on-anomaly to '
                             'exit non-zero on first hit.')
    parser.add_argument('--abort-on-anomaly', dest='abort_on_anomaly', action='store_true',
                        help=f'Modifier on --watch-for-anomalies: exit '
                             f'{ABORT_ON_ANOMALY_EXIT_CODE} after logging the first '
                             f'anomaly. No-op without --watch-for-anomalies.')
    parser.add_argument('--overwrite', action='store_true',
                        help='Permit --out to overwrite an existing file.')
    parser.add_argument('--tee', action='store_true',
                        help='With --out, also mirror frames to stdout in real time. '
                             'No-op without --out.')
    args = parser.parse_args()

    if args.uds:
        dev = find_candlelight()
        set_host_format(dev)
        set_bittiming(dev)
        start_can(dev)
        print(f"Sending TesterPresent (3E 00) on 0x{UDS_REQUEST_ID:03X}...")
        resp = send_uds(dev, b'\x3E\x00')
        if resp:
            print(f"Response: {resp.hex(' ')}")
        else:
            print("No response from ECU")
        stop_can(dev)
    else:
        sniff(duration=args.duration,
              filter_ids=args.filter,
              out_path=args.out_path,
              timestamp_on=args.timestamp,
              allow_ids=parse_allow_list(args.allow_spec),
              watch_anomalies=args.watch_anomalies,
              abort_on_anomaly=args.abort_on_anomaly,
              overwrite=args.overwrite,
              tee=args.tee)
