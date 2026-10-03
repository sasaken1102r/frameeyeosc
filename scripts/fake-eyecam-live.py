#!/usr/bin/env python3
"""Write a fake eyecam-rec live file (the eye-camera values frameeyeosc reads; see src/eyecam_live.rs) at 90 Hz, for
trying frameeyeosc without eyecam-rec:

    python3 scripts/fake-eyecam-live.py /tmp/fake-eyecam/live --lid 0.95 --squint 0.3 &
    frameeyeosc --eyecam-live /tmp/fake-eyecam/live ...

Ctrl+C (or SIGTERM) ends it the way eyecam-rec ends cleanly: live 0 and each eye not valid. --stale-after stops
updating while still saying live, the way a crash leaves the file. Never point it at eyecam-rec's own file
(/run/user/<uid>/eyecam/live): it refuses a folder with eyecam-rec's ctl.sock in it.
"""

import argparse
import mmap
import os
import signal
import struct
import sys
import time

MAGIC = 0x4D435945  # "EYCM"
VERSION = 1
STRUCT_SIZE = 200
FILE_SIZE = 4096
EYES = (56, 128)
RATE = 90.0


def monotonic_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def publish(m, args, valid, live, frame, start_ns):
    """One write under the seqlock: seq odd while the values change, even (one further on) once they are done."""
    seq = struct.unpack_from('<I', m, 24)[0]
    seq += seq % 2  # even, in case a write was cut off
    struct.pack_into('<I', m, 24, seq + 1)
    now = monotonic_ns()
    struct.pack_into('<IIIIQ', m, 0, MAGIC, VERSION, STRUCT_SIZE, args.pid, start_ns)
    struct.pack_into('<I', m, 28, args.calib_state)
    struct.pack_into('<QQII', m, 32, now, now, int(args.recalib), int(live))
    for eye, base in enumerate(EYES):
        struct.pack_into(
            '<QQQII9fI', m, base,
            now, now, frame, int(valid[eye]), int(args.lid < 0.2),
            args.lid, max(0.0, (args.lid - 0.75) / 0.25), args.squint,
            args.pupil_mm / 12.0, args.pupil_mm, args.dilation, 0.9, 0.0, 0.0, 0)
    struct.pack_into('<I', m, 24, seq + 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('path', help='the live file to write (its folder is made if missing)')
    parser.add_argument('--lid', type=float, default=0.95,
                        help='each eye\'s eyelid, VRCFT scale: 0.75 relaxed open, 1 fully widened (default 0.95)')
    parser.add_argument('--squint', type=float, default=0.0, help='each eye\'s squint, 0..1 (default 0)')
    parser.add_argument('--pupil-mm', type=float, default=4.0, help='pupil diameter in mm (default 4)')
    parser.add_argument('--dilation', type=float, default=0.5, help='pupil dilation, 0..1 (default 0.5)')
    parser.add_argument('--calib-state', type=int, default=1,
                        help='bit 0: calibrated for this wear, bit 1: for this user (default 1)')
    parser.add_argument('--recalib', action='store_true', help='say a recalibration is suggested')
    parser.add_argument('--eyes', choices=('both', 'left', 'right'), default='both',
                        help='which eyes are valid (default both)')
    parser.add_argument('--stale-after', type=float, metavar='SECONDS',
                        help='stop updating after this long, still saying live (as a crash leaves it)')
    parser.add_argument('--duration', type=float, metavar='SECONDS', help='end cleanly after this long')
    parser.add_argument('--pid', type=int, default=os.getpid(), help='the writer pid to put in the header')
    args = parser.parse_args()

    folder = os.path.dirname(os.path.abspath(args.path))
    if os.path.exists(os.path.join(folder, 'ctl.sock')):
        sys.exit(f'{folder} has a ctl.sock: that is the real eyecam-rec\'s folder; use another path')
    os.makedirs(folder, mode=0o700, exist_ok=True)
    fd = os.open(args.path, os.O_RDWR | os.O_CREAT, 0o644)
    os.ftruncate(fd, FILE_SIZE)
    m = mmap.mmap(fd, FILE_SIZE)
    valid = (args.eyes in ('both', 'left'), args.eyes in ('both', 'right'))

    stopping = []
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: stopping.append(True))

    start_ns = monotonic_ns()
    start = time.monotonic()
    frame = 0
    stale = False
    print(f'Writing {args.path} at {RATE:.0f} Hz: lid {args.lid}, squint {args.squint}, pupil {args.pupil_mm} mm, '
          f'dilation {args.dilation}, calib_state {args.calib_state}, eyes {args.eyes}; stop with Ctrl+C',
          file=sys.stderr)
    while not stopping:
        elapsed = time.monotonic() - start
        if args.duration is not None and elapsed >= args.duration:
            break
        if args.stale_after is not None and elapsed >= args.stale_after:
            if not stale:
                print('Stopped updating (stale)', file=sys.stderr)
                stale = True
        else:
            frame += 1
            publish(m, args, valid, True, frame, start_ns)
        # On a steady 90 Hz beat
        time.sleep(max(0.0, start + frame / RATE - time.monotonic()) if not stale else 0.1)
    publish(m, args, (False, False), False, frame, start_ns)
    print('Ended cleanly (live 0)', file=sys.stderr)
    m.close()
    os.close(fd)


if __name__ == '__main__':
    main()
