#!/usr/bin/env python3
"""CAN sniffer for candleLight on macOS - patches gs_usb to skip kernel driver detach."""
import sys
import time
import struct
import usb.core
import usb.util

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
        print("ERROR: candleLight not found")
        sys.exit(1)
    dev.set_configuration()
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

def recv_frame(dev, timeout_ms=1000):
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

def sniff(duration=10, filter_ids=None):
    dev = find_candlelight()
    set_host_format(dev)
    set_bittiming(dev)
    start_can(dev)

    print(f"Sniffing CAN @ 500kbps for {duration}s...")
    if filter_ids:
        print(f"Filtering: {', '.join(f'0x{x:03X}' for x in filter_ids)}")

    start = time.time()
    count = 0
    try:
        while time.time() - start < duration:
            frame = recv_frame(dev, timeout_ms=500)
            if frame:
                arb_id, dlc, data = frame
                if filter_ids and arb_id not in filter_ids:
                    continue
                ts = time.time() - start
                print(f"{ts:8.3f}  {arb_id:03X}  [{dlc}]  {data.hex(' ')}")
                count += 1
    except KeyboardInterrupt:
        pass
    finally:
        stop_can(dev)

    print(f"\n{count} frames captured")
    return count

def send_uds(dev, data):
    """Send a UDS request on 0x7E0 and wait for response on 0x7E8."""
    padded = bytes([len(data)]) + data + b'\x00' * (7 - len(data))
    send_frame(dev, 0x7E0, padded)

    start = time.time()
    while time.time() - start < 2.0:
        frame = recv_frame(dev, timeout_ms=500)
        if frame:
            arb_id, dlc, fdata = frame
            if arb_id == 0x7E8:
                return fdata
    return None

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='CAN sniffer for candleLight on macOS')
    parser.add_argument('-d', '--duration', type=int, default=10, help='Sniff duration in seconds')
    parser.add_argument('-f', '--filter', nargs='+', type=lambda x: int(x, 0), help='Filter CAN IDs (hex)')
    parser.add_argument('--uds', action='store_true', help='Send TesterPresent and show response')
    args = parser.parse_args()

    if args.uds:
        dev = find_candlelight()
        set_host_format(dev)
        set_bittiming(dev)
        start_can(dev)
        print("Sending TesterPresent (3E 00) on 0x7E0...")
        resp = send_uds(dev, b'\x3E\x00')
        if resp:
            print(f"Response: {resp.hex(' ')}")
        else:
            print("No response from ECU")
        stop_can(dev)
    else:
        sniff(args.duration, args.filter)
